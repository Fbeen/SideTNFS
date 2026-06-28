#include "include/fs_backend.h"
#include "include/fs_tnfs.h"
#include "include/tnfs_client.h"
#include "include/gemdrvemul.h"
#include "include/debug.h"
#include "wifi_config.h"

#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/ip_addr.h"

// ─── Configuration ────────────────────────────────────────────────────────────

#define CACHE_MAX         256    // max directory entries cached at once
#define CACHE_TTL_MS      5000   // cache lifetime in ms; fetch again after this
#define READDIRX_BATCH    8      // entries requested per TNFS READDIRX call
#define FETCH_TIMEOUT_MS  2000   // per-packet wait during blocking directory fetch
#define FETCH_RETRIES     3      // retransmit attempts before giving up

// ─── Directory cache ──────────────────────────────────────────────────────────

static FsEntry  s_cache[CACHE_MAX];
static int      s_cache_count   = 0;
static char     s_cache_dir[MAX_FOLDER_LENGTH] = "";
static uint32_t s_cache_ms      = 0;
static bool     s_cache_valid   = false;

// ─── TNFS session state (used only during load_dir) ───────────────────────────

static uint16_t s_session = 0;
static uint8_t  s_req_id  = 0;
static uint8_t  s_tx_buf[TNFS_MTU];
static uint8_t  s_rx_buf[TNFS_MTU];

// ─── Opaque file handle (fs_open always returns NULL in this version) ─────────

struct FsHandle { uint8_t unused; };

// ─── Internal helpers ─────────────────────────────────────────────────────────

// Wildcard match — case-sensitive, expects both sides already uppercased.
// '*.*' is a special Atari convention meaning "match everything".
static bool wildmatch(const char *pat, const char *str)
{
    if (pat[0] == '*' && pat[1] == '.' && pat[2] == '*' && pat[3] == '\0') return true;
    if (pat[0] == '*' && pat[1] == '\0')                                    return true;
    if (*pat == '\0' && *str == '\0')                                        return true;
    if (*pat == '\0' || *str == '\0')                                        return false;
    if (*pat == '?' || *pat == *str)    return wildmatch(pat + 1, str + 1);
    if (*pat == '*')
        return wildmatch(pat + 1, str) || wildmatch(pat, str + 1);
    return false;
}

// Translate a GEMDOS backslash path to a TNFS forward-slash path.
// "\\"  → "/"     "\GAMES\"  → "/GAMES"
static void gemdos_to_tnfs(const char *gemdos, char *tnfs, int max)
{
    int j = 0;
    for (int i = 0; gemdos[i] && j < max - 1; i++)
        tnfs[j++] = (gemdos[i] == '\\') ? '/' : gemdos[i];
    if (j == 0) { tnfs[0] = '/'; tnfs[1] = '\0'; return; }
    // strip trailing slash unless root
    if (j > 1 && tnfs[j - 1] == '/') j--;
    tnfs[j] = '\0';
}

// Build a TNFS packet header in s_tx_buf and return pointer to the payload area.
// Every new logical request increments s_req_id; retransmits re-send unchanged.
static uint8_t *prep(uint16_t session, uint8_t cmd)
{
    s_tx_buf[0] = (uint8_t)(session & 0xFF);
    s_tx_buf[1] = (uint8_t)(session >> 8);
    s_tx_buf[2] = s_req_id++;
    s_tx_buf[3] = cmd;
    return s_tx_buf + 4;
}

// Send s_tx_buf[0..len) inside lwIP lock.
static bool do_send(uint16_t len)
{
    cyw43_arch_lwip_begin();
    bool ok = tnfs_client_send(s_tx_buf, len);
    cyw43_arch_lwip_end();
    return ok;
}

// Busy-wait for a matching response: seq, cmd, and (for non-MOUNT) session must match.
// Stale packets (wrong seq/cmd/session) are silently discarded.
static bool wait_match(uint16_t *out_len, uint8_t expect_cmd,
                       uint16_t session, uint8_t seq)
{
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while ((to_ms_since_boot(get_absolute_time()) - start) < FETCH_TIMEOUT_MS) {
        cyw43_arch_lwip_begin();
        uint16_t n = tnfs_client_recv(s_rx_buf, TNFS_MTU - 1);
        cyw43_arch_lwip_end();
        if (n < 5) continue;
        s_rx_buf[n] = '\0';
        if (s_rx_buf[3] != expect_cmd) continue; // wrong command
        if (s_rx_buf[2] != seq)        continue; // wrong sequence
        if (expect_cmd != TNFS_CMD_MOUNT) {
            uint16_t got = (uint16_t)s_rx_buf[0] | ((uint16_t)s_rx_buf[1] << 8);
            if (got != session)        continue; // wrong session
        }
        *out_len = n;
        return true;
    }
    return false;
}

// Send s_tx_buf[0..tx_len) and wait for a matching response, retrying on timeout.
// Retransmits re-send the same packet (seq unchanged), matching TNFS semantics.
static bool send_recv(uint16_t tx_len, uint16_t *rx_len,
                      uint8_t expect_cmd, uint16_t session)
{
    uint8_t seq = s_tx_buf[2]; // saved once; unchanged across retries
    for (int retry = 0; retry < FETCH_RETRIES; retry++) {
        if (!do_send(tx_len)) continue;
        if (wait_match(rx_len, expect_cmd, session, seq)) return true;
        DPRINTF("[TNFS FS] %02x timeout (retry %d/%d)\n",
                expect_cmd, retry + 1, FETCH_RETRIES);
    }
    return false;
}

// ─── Blocking directory fetch ─────────────────────────────────────────────────

static bool load_dir(const char *tnfs_path)
{
    s_cache_count = 0;
    s_cache_valid = false;

    // Open socket (close any stale one first)
    ip_addr_t ip;
    ipaddr_aton(TNFS_SERVER, &ip);
    cyw43_arch_lwip_begin();
    tnfs_client_close();
    bool ok = tnfs_client_open(&ip, TNFS_PORT);
    cyw43_arch_lwip_end();
    if (!ok) {
        LOG("[TNFS FS] socket open failed for %s\n", tnfs_path);
        return false;
    }

    uint16_t rlen;

    // ── MOUNT ──────────────────────────────────────────────────────────────────
    {
        uint8_t *p = prep(0x0000, TNFS_CMD_MOUNT);
        *p++ = TNFS_PROTO_VER_MINOR;
        *p++ = TNFS_PROTO_VER_MAJOR;
        *p++ = '/'; *p++ = '\0'; // mount path
        *p++ = '\0';             // user: anonymous
        *p++ = '\0';             // password: empty
        uint16_t len = (uint16_t)(p - s_tx_buf);
        if (!send_recv(len, &rlen, TNFS_CMD_MOUNT, 0) || s_rx_buf[4] != TNFS_OK) {
            LOG("[TNFS FS] MOUNT failed\n");
            cyw43_arch_lwip_begin(); tnfs_client_close(); cyw43_arch_lwip_end();
            return false;
        }
        s_session = (uint16_t)s_rx_buf[0] | ((uint16_t)s_rx_buf[1] << 8);
        DPRINTF("[TNFS FS] mounted sid=%04x\n", s_session);
    }

    // ── OPENDIRX ───────────────────────────────────────────────────────────────
    uint8_t dir_handle;
    {
        uint8_t *p = prep(s_session, TNFS_CMD_OPENDIRX);
        *p++ = 0; *p++ = 0;      // diropts=0, sortopts=0
        *p++ = 0; *p++ = 0;      // max_results=0 (let server decide)
        *p++ = '*'; *p++ = '\0'; // pattern: all files
        size_t plen = strlen(tnfs_path);
        memcpy(p, tnfs_path, plen + 1);
        p += plen + 1;
        uint16_t len = (uint16_t)(p - s_tx_buf);
        if (!send_recv(len, &rlen, TNFS_CMD_OPENDIRX, s_session) ||
            s_rx_buf[4] != TNFS_OK) {
            LOG("[TNFS FS] OPENDIRX %s failed rc=%02x\n",
                tnfs_path, rlen >= 5 ? s_rx_buf[4] : 0xFF);
            cyw43_arch_lwip_begin(); tnfs_client_close(); cyw43_arch_lwip_end();
            return false;
        }
        dir_handle = s_rx_buf[5];
        uint16_t total;
        memcpy(&total, &s_rx_buf[6], 2);
        LOG("[TNFS FS] loading %s (%u entries)\n", tnfs_path, (unsigned)total);
    }

    // ── READDIRX loop ──────────────────────────────────────────────────────────
    bool eof = false;
    while (!eof && s_cache_count < CACHE_MAX) {
        uint8_t *p = prep(s_session, TNFS_CMD_READDIRX);
        *p++ = dir_handle;
        *p++ = READDIRX_BATCH;
        uint16_t len = (uint16_t)(p - s_tx_buf);
        if (!send_recv(len, &rlen, TNFS_CMD_READDIRX, s_session)) {
            LOG("[TNFS FS] READDIRX timeout\n");
            break;
        }
        uint8_t rc = s_rx_buf[4];
        if (rc == TNFS_EOF) { eof = true; break; }
        if (rc != TNFS_OK)  { DPRINTF("[TNFS FS] READDIRX rc=%02x\n", rc); break; }

        uint8_t batch_count  = s_rx_buf[5];
        uint8_t batch_status = s_rx_buf[6];
        if (batch_status & TNFS_DIRSTATUS_EOF) eof = true;
        if (batch_count == 0) break;

        // Entries are packed at s_rx_buf[9..rlen-1].
        // Each entry: flags(1) + size(4LE) + mtime(4) + ctime(4) + name(null-term)
        const uint8_t *ep  = s_rx_buf + 9;
        const uint8_t *end = s_rx_buf + rlen;

        for (uint8_t i = 0; i < batch_count && s_cache_count < CACHE_MAX; i++) {
            if (ep + 14 > end) break;           // truncated packet
            uint8_t flags = ep[0];
            uint32_t fsize;
            memcpy(&fsize, ep + 1, 4);          // LE uint32
            // ep[5..8]=mtime, ep[9..12]=ctime — ignored
            const char *name    = (const char *)(ep + 13);
            size_t      name_len = strnlen(name, (size_t)(end - ep - 13));
            if (name_len == 0) { ep += 14; continue; }

            // Skip entries that can't be represented as 8.3 filenames
            if (name_len > 13) { ep += 13 + name_len + 1; continue; }

            FsEntry *e = &s_cache[s_cache_count];
            memset(e, 0, sizeof(*e));

            // Uppercase the name; GEMDOS expects all-caps
            for (size_t j = 0; j < name_len; j++)
                e->name[j] = (char)toupper((unsigned char)name[j]);
            e->name[name_len] = '\0';

            e->size = fsize;
            e->attr = (flags & TNFS_DIRENTRY_DIR) ? 0x10u : 0x20u;
            e->date = 0; // timestamps not implemented in this milestone
            e->time = 0;

            s_cache_count++;
            ep += 13 + name_len + 1;
        }
    }

    // ── CLOSEDIR ───────────────────────────────────────────────────────────────
    {
        uint8_t *p = prep(s_session, TNFS_CMD_CLOSEDIR);
        *p++ = dir_handle;
        uint16_t len = (uint16_t)(p - s_tx_buf);
        // best-effort; ignore failure
        send_recv(len, &rlen, TNFS_CMD_CLOSEDIR, s_session);
    }

    cyw43_arch_lwip_begin();
    tnfs_client_close();
    cyw43_arch_lwip_end();

    LOG("[TNFS FS] loaded %d entries\n", s_cache_count);
    s_cache_valid = true;
    s_cache_ms    = to_ms_since_boot(get_absolute_time());
    strncpy(s_cache_dir, tnfs_path, sizeof(s_cache_dir) - 1);
    s_cache_dir[sizeof(s_cache_dir) - 1] = '\0';
    return true;
}

// ─── fs_backend.h API ─────────────────────────────────────────────────────────

void fs_init(void)
{
    s_cache_valid = false;
    s_cache_count = 0;
    s_session     = 0;
    s_req_id      = 0;
}

bool fs_list_dir(const char *dir, const char *pat, int index, FsEntry *out)
{
    char tnfs_path[MAX_FOLDER_LENGTH];
    gemdos_to_tnfs(dir, tnfs_path, sizeof(tnfs_path));

    // Cache validity check
    uint32_t now     = to_ms_since_boot(get_absolute_time());
    bool     expired = (now - s_cache_ms) >= CACHE_TTL_MS;
    bool     same    = s_cache_valid && (strcmp(tnfs_path, s_cache_dir) == 0);

    if (same && !expired) {
        DPRINTF("[TNFS FS] cache hit  %s\n", tnfs_path);
    } else {
        if (same && expired)
            LOG("[TNFS FS] cache miss (expired) %s\n", tnfs_path);
        else
            LOG("[TNFS FS] cache miss %s\n", tnfs_path);

        if (!load_dir(tnfs_path)) {
            // Network failure: return empty directory rather than crashing
            s_cache_valid = false;
            s_cache_count = 0;
            return false;
        }
    }

    // Walk the cache, counting entries that match the pattern
    int match = 0;
    for (int i = 0; i < s_cache_count; i++) {
        if (wildmatch(pat, s_cache[i].name)) {
            if (match == index) {
                *out = s_cache[i];
                return true;
            }
            match++;
        }
    }
    return false;
}

bool fs_stat(const char *path, FsEntry *out)
{
    (void)path; (void)out;
    return false; // not implemented yet
}

FsHandle *fs_open(const char *path, int16_t *gemdos_err)
{
    (void)path;
    *gemdos_err = GEMDOS_EFILNF; // not implemented yet
    return NULL;
}

uint32_t fs_read(FsHandle *h, void *buf, uint32_t len)
{
    (void)h; (void)buf; (void)len;
    return 0;
}

int32_t fs_seek(FsHandle *h, int32_t offset, int whence)
{
    (void)h; (void)offset; (void)whence;
    return GEMDOS_EINTRN;
}

void fs_close(FsHandle *h)
{
    (void)h;
}
