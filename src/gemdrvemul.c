#include "include/gemdrvemul.h"
#include "include/fs_backend.h"
#include "include/net_wifi.h"
#include "include/net_test.h"

// ─── Atari GEMDRIVE protocol layer ───────────────────────────────────────────
//   Handles the ROM3 command/response protocol with the Atari 68k firmware.
//   All filesystem access goes through the fs_backend.h interface below.
//
//   Atari GEMDRIVE protocol layer  (this file)
//       → filesystem backend interface  (fs_backend.h)
//           → memory backend            (fs_memory.c)   ← active now
//           → TNFS backend              (fs_tnfs.c)     ← future

// IRQ-visible state: command pending + payload pointer
static volatile uint16_t active_command_id = 0xFFFF;
static uint16_t *payloadPtr = NULL;
static uint16_t payload_size_received = 0;
static uint32_t random_token;

// Current directory path (backslash-separated, Atari style)
static char dpath_string[MAX_FOLDER_LENGTH] = {'\\', '\0'};

// Drive letter assigned to this GEMDRIVE
#define DRIVE_LETTER 'C'
#define DRIVE_NUMBER 2  // A=0, B=1, C=2

// ── Open file descriptors ─────────────────────────────────────────────────────

#define MAX_OPEN_FILES 16
typedef struct {
    bool      in_use;
    FsHandle *handle;
} OpenFile;
static OpenFile open_files[MAX_OPEN_FILES];

// ── DTA search slots ──────────────────────────────────────────────────────────

#define MAX_DTA_SLOTS 16
typedef struct {
    uint32_t key;       // ndta Atari address (0 = empty slot)
    int      dir_index; // next index to pass to fs_list_dir()
    uint32_t attribs;   // requested search attributes
    char     path[MAX_FOLDER_LENGTH];    // search directory (backslash-separated)
    char     pattern[MAX_FOLDER_LENGTH]; // filename wildcard pattern (uppercase)
} DTASlot;
static DTASlot dta_slots[MAX_DTA_SLOTS];

// ── Helpers ───────────────────────────────────────────────────────────────────

static inline void generate_random_token_seed(const TransmissionProtocol *protocol)
{
    random_token = ((*((uint32_t *)protocol->payload) & 0xFFFF0000) >> 16) |
                   ((*((uint32_t *)protocol->payload) & 0x0000FFFF) << 16);
}

static void __not_in_flash_func(write_random_token)(uint32_t mem)
{
    *((volatile uint32_t *)(mem + GEMDRVEMUL_RANDOM_TOKEN)) = random_token;
}

static void set_shared_var(uint32_t idx, uint32_t val, uint32_t mem)
{
    *((volatile uint16_t *)(mem + GEMDRVEMUL_SHARED_VARIABLES + (idx * 4) + 2)) = val & 0xFFFF;
    *((volatile uint16_t *)(mem + GEMDRVEMUL_SHARED_VARIABLES + (idx * 4)))     = val >> 16;
}

// Convert ASCII string to uppercase in-place (dst may equal src).
static void str_upper(const char *src, char *dst, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
    dst[i] = '\0';
}

// Split "C:\DIR\PATTERN" → dir="\DIR\", pattern="PATTERN" (uppercase).
static void split_fspec(const char *fspec, char *dir, char *pattern)
{
    const char *last_slash = NULL;
    for (const char *p = fspec; *p; p++)
        if (*p == '\\' || *p == '/') last_slash = p;

    if (last_slash) {
        size_t dlen = (size_t)(last_slash - fspec) + 1;
        if (dlen == 0) dlen = 1;
        if (dlen >= MAX_FOLDER_LENGTH) dlen = MAX_FOLDER_LENGTH - 1;
        memcpy(dir, fspec, dlen);
        dir[dlen] = '\0';
        str_upper(last_slash + 1, pattern, MAX_FOLDER_LENGTH);
    } else {
        dir[0] = '\\'; dir[1] = '\0';
        str_upper(fspec, pattern, MAX_FOLDER_LENGTH);
    }
}

// ── DTA slot management ───────────────────────────────────────────────────────

static DTASlot *find_dta(uint32_t key)
{
    for (int i = 0; i < MAX_DTA_SLOTS; i++)
        if (dta_slots[i].key == key) return &dta_slots[i];
    return NULL;
}

static DTASlot *alloc_dta(uint32_t key)
{
    DTASlot *s = find_dta(key);
    if (s) return s;
    for (int i = 0; i < MAX_DTA_SLOTS; i++) {
        if (!dta_slots[i].key) {
            memset(&dta_slots[i], 0, sizeof(DTASlot));
            dta_slots[i].key = key;
            return &dta_slots[i];
        }
    }
    return NULL;
}

static void release_dta(uint32_t key)
{
    DTASlot *s = find_dta(key);
    if (s) memset(s, 0, sizeof(DTASlot));
}

static int count_dta(void)
{
    int n = 0;
    for (int i = 0; i < MAX_DTA_SLOTS; i++)
        if (dta_slots[i].key) n++;
    return n;
}

// ── Open-file management ──────────────────────────────────────────────────────

static int alloc_fd(FsHandle *h)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) {
            open_files[i].in_use  = true;
            open_files[i].handle  = h;
            return FIRST_FILE_DESCRIPTOR + i;
        }
    }
    return -1;
}

static OpenFile *get_fd(uint16_t fd)
{
    int slot = (int)fd - FIRST_FILE_DESCRIPTOR;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !open_files[slot].in_use)
        return NULL;
    return &open_files[slot];
}

static void free_fd(uint16_t fd)
{
    int slot = (int)fd - FIRST_FILE_DESCRIPTOR;
    if (slot >= 0 && slot < MAX_OPEN_FILES)
        open_files[slot].in_use = false;
}

// ── DTA transfer area fill ────────────────────────────────────────────────────
// Writes a filesystem entry into the 44-byte DTA transfer area in ROM3 shared
// memory so the 68k ROM driver can copy it to the Atari's DTA buffer.

static void write_dta_entry(uint32_t mem, const FsEntry *e)
{
    memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);

    // Internal name field (bytes 0-11)
    for (int i = 0; i < 12 && e->name[i]; i++)
        *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + i)) = (uint8_t)e->name[i];

    // d_attr (byte 20) + d_attrib (byte 21) — write then swap word to match Atari byte order
    *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 20)) = e->attr;
    *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 21)) = e->attr;
    CHANGE_ENDIANESS_BLOCK16(mem + GEMDRVEMUL_DTA_TRANSFER + 20, 2);

    // d_time (bytes 22-23), d_date (bytes 24-25) — raw FAT uint16
    *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 22)) = e->time;
    *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 24)) = e->date;

    // d_length (bytes 26-29) — swapped longword
    uint32_t swapped_len = ((e->size << 16) & 0xFFFF0000) | ((e->size >> 16) & 0xFFFF);
    uint16_t *lptr = (uint16_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 26);
    lptr[0] = swapped_len & 0xFFFF;
    lptr[1] = (swapped_len >> 16) & 0xFFFF;

    // d_fname (bytes 30-43) — write then endian-swap each word pair
    for (int i = 0; i < 14 && e->name[i]; i++)
        *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 30 + i)) = (uint8_t)e->name[i];
    CHANGE_ENDIANESS_BLOCK16(mem + GEMDRVEMUL_DTA_TRANSFER + 30, 14);
}

// ── IRQ handler (called from DMA IRQ context) ─────────────────────────────────

static inline void __not_in_flash_func(handle_protocol_command)(const TransmissionProtocol *protocol)
{
    if (active_command_id == 0xFFFF)
    {
        // Skip 4-byte random token at start of payload
        payloadPtr = (uint16_t *)protocol->payload + 2;
        payload_size_received = protocol->payload_size;
        generate_random_token_seed(protocol);
        active_command_id = protocol->command_id;
    }
}

void __not_in_flash_func(gemdrvemul_dma_irq_handler_lookup_callback)(void)
{
    uint32_t addr = (uint32_t)dma_hw->ch[lookup_data_rom_dma_channel].al3_read_addr_trig;
    if (addr >= ROM3_START_ADDRESS)
        parse_protocol((uint16_t)(addr & 0xFFFF), handle_protocol_command);
    dma_hw->ints1 = 1u << lookup_data_rom_dma_channel;
}

// ── Main command loop ─────────────────────────────────────────────────────────

void init_gemdrvemul(void)
{
    LOG("SIDETNFS GEMDRIVE emulator starting (M2 backend interface)\n");

    dpath_string[0] = '\\';
    dpath_string[1] = '\0';

    active_command_id = 0xFFFF;
    memset(open_files, 0, sizeof(open_files));
    memset(dta_slots,  0, sizeof(dta_slots));

    // ── Filesystem backend initialisation ─────────────────────────────────────
    fs_init();

    uint32_t mem  = ROM3_START_ADDRESS;  // shared memory (ROM3 bank)
    uint32_t code = ROM4_START_ADDRESS;  // firmware code  (ROM4 bank)

    // Zero shared memory
    memset((void *)mem, 0, 0x5000);

    // Pre-populate status fields expected by the 68k driver
    *((volatile uint32_t *)(mem + GEMDRVEMUL_RTC_STATUS))     = 0x0;
    *((volatile uint32_t *)(mem + GEMDRVEMUL_NETWORK_STATUS)) = 0x0;
    *((volatile uint32_t *)(mem + GEMDRVEMUL_RTC_ENABLED))    = 0x0;
    *((volatile uint32_t *)(mem + GEMDRVEMUL_RTC_Y2K_PATCH))  = 0x0;
    WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_TIMEOUT_SEC, 0);

    // Shared variables expected by the 68k driver
    set_shared_var(SHARED_VARIABLE_FIRST_FILE_DESCRIPTOR, FIRST_FILE_DESCRIPTOR, mem);
    set_shared_var(SHARED_VARIABLE_DRIVE_LETTER, (uint32_t)DRIVE_LETTER, mem);
    set_shared_var(SHARED_VARIABLE_DRIVE_NUMBER, DRIVE_NUMBER, mem);
    set_shared_var(SHARED_VARIABLE_BUFFER_TYPE, 0, mem);
    set_shared_var(SHARED_VARIABLE_FAKE_FLOPPY,  0, mem);

    DPRINTF("Entering GEMDRIVE command loop...\n");

#ifdef DIAGNOSTIC_PASSTHROUGH
    *((volatile uint16_t *)(code + 0x0806)) = 0x4E71;
    *((volatile uint16_t *)(code + 0x0828)) = 0x4E71;
    LOG("DIAGNOSTIC: gemdrive_trap patched → pure pass-through (both variants)\n");
#endif

    static uint32_t seed_counter = 1;

    while (true)
    {
        *((volatile uint32_t *)(mem + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = ++seed_counter;
        tight_loop_contents();

#ifdef SIDETNFS_DEBUG
        // Print a status banner every time USB serial connects or reconnects.
        // Checked once per second to avoid overhead; fires on every DTR rising
        // edge so the user sees current state whenever they open the terminal,
        // regardless of how late in the Atari boot sequence that happens.
        {
            static bool     prev_usb      = false;
            static uint32_t usb_check_ms  = 0;
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            if (now_ms - usb_check_ms >= 1000) {
                usb_check_ms = now_ms;
                bool cur_usb = stdio_usb_connected();
                if (cur_usb && !prev_usb) {
                    LOG("--- SIDETNFS GEMDRIVE ready, C: active ---\n");
                    net_wifi_log_status();
                    net_test_log_result();
                }
                prev_usb = cur_usb;
            }
        }
#endif

        // Check WiFi status once per second; logs connect/fail/timeout.
        // No-op after WiFi is resolved (connected or failed).
        net_wifi_poll();

        uint16_t cmd = active_command_id;
        if (cmd == 0xFFFF)
            continue;

        LOG("[DISPATCH] 0x%04x\n", cmd);
        switch (cmd)
        {
        // ── Handshake ──────────────────────────────────────────────────────
        case GEMDRVEMUL_PING:
        {
            LOG("[CMD] PING\n");
            *((volatile uint16_t *)(mem + GEMDRVEMUL_PING_STATUS)) = 0x1;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_CANCEL:
        {
            DPRINTF("CANCEL\n");
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Vector patching ────────────────────────────────────────────────
        case GEMDRVEMUL_SAVE_VECTORS:
        {
            uint32_t old_addr  = ((uint32_t)payloadPtr[0] << 16) | payloadPtr[1];
            payloadPtr += 2;
            uint32_t xbra_addr = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            uint32_t xbra_offset = xbra_addr - ATARI_ROM4_START_ADDRESS;
            LOG("[CMD] SAVE_VECTORS: raw_old=0x%08lx xbra_atari=0x%08lx offset=0x%04lx\n",
                   (unsigned long)old_addr, (unsigned long)xbra_addr, (unsigned long)xbra_offset);
            if (xbra_offset < 0x10000) {
                *((volatile uint16_t *)(code + xbra_offset))     = old_addr & 0xFFFF;
                *((volatile uint16_t *)(code + xbra_offset + 2)) = old_addr >> 16;
                LOG("[CMD] SAVE_VECTORS: old_handler patched at ROM4+0x%04lx\n", (unsigned long)xbra_offset);
            } else {
                LOG("[CMD] SAVE_VECTORS: ERROR xbra_offset 0x%04lx out of ROM4 range!\n", (unsigned long)xbra_offset);
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_SAVE_XBIOS_VECTOR:
        {
            uint32_t xbios_old = ((uint32_t)payloadPtr[0] << 16) | payloadPtr[1];
            *((volatile uint16_t *)(mem + GEMDRVEMUL_OLD_XBIOS_TRAP))     = xbios_old & 0xFFFF;
            *((volatile uint16_t *)(mem + GEMDRVEMUL_OLD_XBIOS_TRAP + 2)) = xbios_old >> 16;
            LOG("[CMD] SAVE_XBIOS_VECTOR %08lx\n", (unsigned long)xbios_old);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_SHOW_VECTOR_CALL:
        {
            LOG("[CMD] SHOW_VECTOR_CALL %04x\n", (uint16_t)payloadPtr[0]);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Reentry guards ─────────────────────────────────────────────────
        case GEMDRVEMUL_REENTRY_LOCK:
        {
            LOG("[CHKPT] REENTRY_LOCK → handler entered, GEMDOS vector OK\n");
            *((volatile uint16_t *)(mem + GEMDRVEMUL_REENTRY_TRAP)) = 0xFFFF;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_REENTRY_UNLOCK:
        {
            LOG("[CHKPT] REENTRY_UNLOCK\n");
            *((volatile uint16_t *)(mem + GEMDRVEMUL_REENTRY_TRAP)) = 0x0;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_REENTRY_XBIOS_LOCK:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0xFFFF;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_REENTRY_XBIOS_UNLOCK:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0x0;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Shared variables ───────────────────────────────────────────────
        case GEMDRVEMUL_SET_SHARED_VAR:
        {
            uint32_t idx = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;
            uint32_t val = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            set_shared_var(idx, val, mem);
            LOG("[CMD] SET_SHARED_VAR[%lu]=0x%08lx\n", (unsigned long)idx, (unsigned long)val);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Drive info ─────────────────────────────────────────────────────
        case GEMDRVEMUL_DGETDRV_CALL:
        {
            DPRINTF("DGETDRV\n");
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DFREE_CALL:
        {
            DPRINTF("DFREE\n");
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT,      256);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT + 4,  512);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT + 8,  512);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT + 12, 1);
            *((volatile uint32_t *)(mem + GEMDRVEMUL_DFREE_STATUS)) = GEMDOS_EOK;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Directory path ─────────────────────────────────────────────────
        case GEMDRVEMUL_DGETPATH_CALL:
        {
            DPRINTF("DGETPATH: %s\n", dpath_string);
            char tmp[MAX_FOLDER_LENGTH];
            strncpy(tmp, dpath_string, MAX_FOLDER_LENGTH - 1);
            tmp[MAX_FOLDER_LENGTH - 1] = '\0';
            size_t len = strlen(tmp);
            if (len > 1 && tmp[len - 1] == '\\') tmp[len - 1] = '\0';
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(tmp, mem + GEMDRVEMUL_DEFAULT_PATH, MAX_FOLDER_LENGTH);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DSETPATH_CALL:
        {
            LOG("[DSETPATH_RAW psize=%u]", payload_size_received);
            for (int _i = 0; _i < 20; _i++) LOG(" %04x", payloadPtr[_i]);
            LOG("\n");
            payloadPtr += 6;
            char new_path[MAX_FOLDER_LENGTH] = {0};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, new_path, MAX_FOLDER_LENGTH);
            DPRINTF("DSETPATH: %s\n", new_path);

            if (new_path[1] == ':') memmove(new_path, new_path + 2, strlen(new_path) - 1);

            bool is_root = (new_path[0] == '\0' || (new_path[0] == '\\' && new_path[1] == '\0'));
            if (is_root) {
                dpath_string[0] = '\\'; dpath_string[1] = '\0';
                *((volatile uint16_t *)(mem + GEMDRVEMUL_SET_DPATH_STATUS)) = GEMDOS_EOK;
            } else {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_SET_DPATH_STATUS)) = GEMDOS_EPTHNF;
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── DTA management ─────────────────────────────────────────────────
        case GEMDRVEMUL_FSETDTA_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            DPRINTF("FSETDTA ndta=%08x\n", ndta);
            if (!find_dta(ndta)) alloc_dta(ndta);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DTA_EXIST_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            bool exists = find_dta(ndta) != NULL;
            DPRINTF("DTA_EXIST ndta=%08x -> %d\n", ndta, exists);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DTA_EXIST, exists ? ndta : 0);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DTA_RELEASE_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            DPRINTF("DTA_RELEASE ndta=%08x\n", ndta);
            release_dta(ndta);
            memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DTA_RELEASE, (uint32_t)count_dta());
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Directory search ───────────────────────────────────────────────
        case GEMDRVEMUL_FSFIRST_CALL:
        {
            uint32_t ndta    = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;
            uint32_t attribs = payloadPtr[0];
            payloadPtr += 2;
            payloadPtr += 2;  // fspec Atari address — skip
            char raw_fspec[MAX_FOLDER_LENGTH] = {0};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, raw_fspec, MAX_FOLDER_LENGTH);
            DPRINTF("FSFIRST ndta=%08x attribs=%04x fspec=%s\n", ndta, attribs, raw_fspec);

            char fspec[MAX_FOLDER_LENGTH];
            if (raw_fspec[1] == ':') strncpy(fspec, raw_fspec + 2, MAX_FOLDER_LENGTH - 1);
            else                     strncpy(fspec, raw_fspec,     MAX_FOLDER_LENGTH - 1);
            fspec[MAX_FOLDER_LENGTH - 1] = '\0';

            char dir[MAX_FOLDER_LENGTH], pat[MAX_FOLDER_LENGTH];
            split_fspec(fspec, dir, pat);
            DPRINTF("  dir=%s pattern=%s\n", dir, pat);

            DTASlot *slot = alloc_dta(ndta);
            if (!slot) {
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_EINTRN;
                write_random_token(mem);
                active_command_id = 0xFFFF;
                break;
            }
            slot->attribs   = attribs;
            slot->dir_index = 0;
            strncpy(slot->path,    dir, MAX_FOLDER_LENGTH - 1);
            strncpy(slot->pattern, pat, MAX_FOLDER_LENGTH - 1);

            FsEntry entry;
            if (fs_list_dir(dir, pat, 0, &entry)) {
                slot->dir_index = 1;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = 0;
                write_dta_entry(mem, &entry);
                DPRINTF("  found=%s\n", entry.name);
            } else {
                release_dta(ndta);
                memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_EFILNF;
                DPRINTF("  not found\n");
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FSNEXT_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            DPRINTF("FSNEXT ndta=%08x\n", ndta);
            DTASlot *slot = find_dta(ndta);
            if (!slot) {
                memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_EINTRN;
                write_random_token(mem);
                active_command_id = 0xFFFF;
                break;
            }
            FsEntry entry;
            if (fs_list_dir(slot->path, slot->pattern, slot->dir_index, &entry)) {
                slot->dir_index++;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = 0;
                write_dta_entry(mem, &entry);
                DPRINTF("  found=%s\n", entry.name);
            } else {
                release_dta(ndta);
                memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_ENMFIL;
                DPRINTF("  no more files\n");
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── File open/close/seek/read ──────────────────────────────────────
        case GEMDRVEMUL_FOPEN_CALL:
        {
            LOG("[FOPEN_RAW psize=%u]", payload_size_received);
            for (int _i = 0; _i < 20; _i++) LOG(" %04x", payloadPtr[_i]);
            LOG("\n");
            uint16_t mode = payloadPtr[0];
            payloadPtr += 6;  // skip mode + 5 address/padding words
            char filename[MAX_FOLDER_LENGTH] = {0};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, filename, MAX_FOLDER_LENGTH);
            DPRINTF("FOPEN mode=%d file=%s\n", mode, filename);

            // Strip drive letter and leading path separator
            char *fptr = filename;
            if (fptr[1] == ':') fptr += 2;
            if (*fptr == '\\' || *fptr == '/') fptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(fptr, upper, MAX_FOLDER_LENGTH);

            if (mode != 0) {
                // Backend is read-only; refuse write or read-write opens.
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)(int32_t)GEMDOS_EACCDN);
            } else {
                int16_t err = GEMDOS_EFILNF;
                FsHandle *h = fs_open(upper, &err);
                if (!h) {
                    WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)(int32_t)err);
                } else {
                    int fd = alloc_fd(h);
                    if (fd < 0) {
                        fs_close(h);
                        WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)(int32_t)GEMDOS_ENHNDL);
                    } else {
                        WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)fd);
                        DPRINTF("  opened fd=%d\n", fd);
                    }
                }
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FCLOSE_CALL:
        {
            uint16_t fd = payloadPtr[0];
            DPRINTF("FCLOSE fd=%d\n", fd);
            OpenFile *f = get_fd(fd);
            if (!f) {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EIHNDL;
            } else {
                fs_close(f->handle);
                free_fd(fd);
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EOK;
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FSEEK_CALL:
        {
            uint16_t fd     = payloadPtr[0];
            payloadPtr += 2;
            uint32_t offset = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;
            uint16_t whence = payloadPtr[0];
            DPRINTF("FSEEK fd=%d offset=%d whence=%d\n", fd, offset, whence);

            OpenFile *f = get_fd(fd);
            if (!f) {
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FSEEK_STATUS, (uint32_t)(int32_t)GEMDOS_EIHNDL);
            } else {
                int32_t new_pos = fs_seek(f->handle, (int32_t)offset, whence);
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FSEEK_STATUS, (uint32_t)new_pos);
                DPRINTF("  new_pos=%d\n", new_pos);
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_READ_BUFF_CALL:
        {
            uint16_t fd        = payloadPtr[0];
            payloadPtr += 2;
            uint32_t req_bytes = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;
            DPRINTF("READ_BUFF fd=%d req=%d\n", fd, req_bytes);

            OpenFile *f = get_fd(fd);
            if (!f) {
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_READ_BYTES, (uint32_t)(int32_t)GEMDOS_EIHNDL);
            } else {
                if (req_bytes > DEFAULT_FOPEN_READ_BUFFER_SIZE)
                    req_bytes = DEFAULT_FOPEN_READ_BUFFER_SIZE;

                uint8_t *dst  = (uint8_t *)(mem + GEMDRVEMUL_READ_BUFF);
                uint32_t n    = fs_read(f->handle, dst, req_bytes);
                uint32_t swap = n + (n & 1);
                if (n & 1) dst[n] = 0;
                CHANGE_ENDIANESS_BLOCK16(mem + GEMDRVEMUL_READ_BUFF, swap);

                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_READ_BYTES, n);
                DPRINTF("  read %d bytes\n", n);
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── File attributes / date-time ────────────────────────────────────
        case GEMDRVEMUL_FATTRIB_CALL:
        {
            uint16_t flag = payloadPtr[0];
            payloadPtr += 6;
            char filename[MAX_FOLDER_LENGTH] = {0};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, filename, MAX_FOLDER_LENGTH);
            char *fptr = filename;
            if (fptr[1] == ':') fptr += 2;
            if (*fptr == '\\' || *fptr == '/') fptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(fptr, upper, MAX_FOLDER_LENGTH);
            DPRINTF("FATTRIB flag=%d file=%s\n", flag, upper);

            FsEntry entry;
            if (fs_stat(upper, &entry))
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FATTRIB_STATUS)) = entry.attr;
            else
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FATTRIB_STATUS)) = (uint16_t)(int16_t)GEMDOS_EFILNF;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FDATETIME_CALL:
        {
            uint16_t flag = payloadPtr[0];
            payloadPtr += 6;
            char filename[MAX_FOLDER_LENGTH] = {0};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, filename, MAX_FOLDER_LENGTH);
            char *fptr = filename;
            if (fptr[1] == ':') fptr += 2;
            if (*fptr == '\\' || *fptr == '/') fptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(fptr, upper, MAX_FOLDER_LENGTH);
            DPRINTF("FDATETIME flag=%d file=%s\n", flag, upper);

            FsEntry entry;
            if (fs_stat(upper, &entry)) {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_DATE))   = entry.date;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_TIME))   = entry.time;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_STATUS)) = GEMDOS_EOK;
            } else {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_STATUS)) = (uint16_t)(int16_t)GEMDOS_EFILNF;
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Write/create/delete — read-only FS, return access denied ───────
        case GEMDRVEMUL_FCREATE_CALL:
        {
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FCREATE_HANDLE, (uint32_t)(int32_t)GEMDOS_EACCDN);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DCREATE_CALL:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_DCREATE_STATUS)) = (uint16_t)(int16_t)GEMDOS_EACCDN;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DDELETE_CALL:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_DDELETE_STATUS)) = (uint16_t)(int16_t)GEMDOS_EACCDN;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FDELETE_CALL:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_FDELETE_STATUS)) = (uint16_t)(int16_t)GEMDOS_EACCDN;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_PEXEC_CALL:
        {
            LOG("[PEXEC_RAW psize=%u]", payload_size_received);
            for (int _i = 0; _i < 10; _i++) LOG(" %04x", payloadPtr[_i]);
            LOG("\n");
            // WRITE_AND_SWAP_LONGWORD swaps the two 16-bit words before storing,
            // so the 68k reads back the original value. The ROM checks
            // CMPI.W #0, $FB4184 (reads the HIGH WORD of PEXEC_MODE).
            // Value 0x00010000 → HIGH WORD = 0x0001 → not equal → BNE → FA08E4
            //   → old_handler (original TOS Pexec) instead of crashing FOPEN path.
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_PEXEC_MODE, 0x00010000);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        default:
            LOG("[CMD] Unhandled cmd 0x%04x — clearing\n", cmd);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }
    }
}
