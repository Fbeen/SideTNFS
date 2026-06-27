#include "include/gemdrvemul.h"

// IRQ-visible state: command pending + payload pointer
static volatile uint16_t active_command_id = 0xFFFF;
static uint16_t *payloadPtr = NULL;
static uint16_t payload_size_received = 0;
static uint32_t random_token;

// Current directory path (backslash-separated, Atari style)
static char dpath_string[MAX_FOLDER_LENGTH] = {'\\', '\0'};

// Drive letter assigned to this fake drive
#define DRIVE_LETTER 'C'
#define DRIVE_NUMBER 2  // C = index 2 (A=0, B=1, C=2)

// ── Fake filesystem ──────────────────────────────────────────────────────────
// One file: README.TXT in the root directory

static const char readme_content[] =
    "SIDETNFS - Atari ST TNFS GEMDOS Network Drive\r\n"
    "Milestone 1 - Fake in-memory read-only filesystem\r\n"
    "This file is served from Raspberry Pi Pico W RAM.\r\n"
    "Future milestones will add TNFS network access.\r\n";
#define README_SIZE ((uint32_t)(sizeof(readme_content) - 1))

// FAT date/time for README.TXT: 2024-01-01 12:00:00
// FAT date: bits[15:9]=year-1980, bits[8:5]=month, bits[4:0]=day
// 2024-1980=44 -> (44<<9)|(1<<5)|1 = 22528+32+1 = 22561 = 0x5821
// FAT time: bits[15:11]=hours, bits[10:5]=minutes, bits[4:0]=sec/2
// 12:00:00 -> (12<<11) = 0x6000
#define README_DATE 0x5821
#define README_TIME 0x6000
#define README_ATTR 0x20   // Archive

// Open file descriptors (fd = FIRST_FILE_DESCRIPTOR + slot_index)
#define MAX_OPEN_FILES 16
typedef struct {
    bool     in_use;
    uint32_t offset;  // current read position
} FakeOpenFile;
static FakeOpenFile open_files[MAX_OPEN_FILES];

// DTA search slots
#define MAX_DTA_SLOTS 16
typedef struct {
    uint32_t key;      // ndta Atari address (0 = empty)
    bool     returned; // have we returned README.TXT already?
    uint32_t attribs;  // requested search attributes
    char     path[MAX_FOLDER_LENGTH];    // search directory (backslash)
    char     pattern[MAX_FOLDER_LENGTH]; // filename pattern (uppercase)
} FakeDTASlot;
static FakeDTASlot dta_slots[MAX_DTA_SLOTS];

// ── Helpers ──────────────────────────────────────────────────────────────────

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

// Simple upper-case conversion
static void str_upper(const char *src, char *dst, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
    dst[i] = '\0';
}

// Wildcard match: pattern uses * and ?  (both uppercase already)
static bool wildmatch(const char *pat, const char *str)
{
    if (*pat == '*' && *(pat + 1) == '\0') return true;
    if (*pat == '\0' && *str == '\0')      return true;
    if (*pat == '\0' || *str == '\0')      return false;
    if (*pat == '?' || *pat == *str)       return wildmatch(pat + 1, str + 1);
    if (*pat == '*')
        return wildmatch(pat + 1, str) || wildmatch(pat, str + 1);
    return false;
}

// Parse "C:\DIR\PATTERN" → dir="\DIR\", pattern="PATTERN"
static void split_fspec(const char *fspec, char *dir, char *pattern)
{
    const char *last_slash = NULL;
    for (const char *p = fspec; *p; p++)
        if (*p == '\\' || *p == '/') last_slash = p;

    if (last_slash) {
        size_t dlen = (last_slash - fspec) + 1;
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

static FakeDTASlot *find_dta(uint32_t key)
{
    for (int i = 0; i < MAX_DTA_SLOTS; i++)
        if (dta_slots[i].key == key) return &dta_slots[i];
    return NULL;
}

static FakeDTASlot *alloc_dta(uint32_t key)
{
    FakeDTASlot *s = find_dta(key);
    if (s) return s;
    for (int i = 0; i < MAX_DTA_SLOTS; i++) {
        if (!dta_slots[i].key) {
            memset(&dta_slots[i], 0, sizeof(FakeDTASlot));
            dta_slots[i].key = key;
            return &dta_slots[i];
        }
    }
    return NULL;
}

static void release_dta(uint32_t key)
{
    FakeDTASlot *s = find_dta(key);
    if (s) memset(s, 0, sizeof(FakeDTASlot));
}

static int count_dta(void)
{
    int n = 0;
    for (int i = 0; i < MAX_DTA_SLOTS; i++)
        if (dta_slots[i].key) n++;
    return n;
}

// ── Open-file management ─────────────────────────────────────────────────────

static int alloc_fd(void)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) {
            open_files[i].in_use  = true;
            open_files[i].offset  = 0;
            return FIRST_FILE_DESCRIPTOR + i;
        }
    }
    return -1;
}

static FakeOpenFile *get_fd(uint16_t fd)
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

// ── DTA transfer area fill ───────────────────────────────────────────────────

static void write_dta_readme(uint32_t mem)
{
    // Zero the 44-byte DTA transfer area
    memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);

    // Internal name field (bytes 0-11) — write byte-by-byte then endian-swap
    const char *fname = "README.TXT";
    for (int i = 0; i < 12 && fname[i]; i++)
        *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + i)) = (uint8_t)fname[i];

    // d_offset_drive (bytes 12-15), d_curbyt (16-17), d_curcl (18-19) — all 0

    // d_attr (byte 20), d_attrib (byte 21)
    *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 20)) = README_ATTR;
    *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 21)) = README_ATTR;
    // Swap the two attribute bytes to match Atari byte order
    CHANGE_ENDIANESS_BLOCK16(mem + GEMDRVEMUL_DTA_TRANSFER + 20, 2);

    // d_time (bytes 22-23), d_date (bytes 24-25) — raw uint16 as FAT format
    *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 22)) = README_TIME;
    *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 24)) = README_DATE;

    // d_length (bytes 26-29) — swapped longword
    uint32_t swapped_len = ((README_SIZE << 16) & 0xFFFF0000) | ((README_SIZE >> 16) & 0xFFFF);
    uint16_t *lptr = (uint16_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 26);
    lptr[0] = swapped_len & 0xFFFF;
    lptr[1] = (swapped_len >> 16) & 0xFFFF;

    // d_fname (bytes 30-43) — write then endian-swap each word
    for (int i = 0; i < 14 && fname[i]; i++)
        *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 30 + i)) = (uint8_t)fname[i];
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
    printf("SIDETNFS GEMDRIVE emulator starting (M1 fake FS)\n");

    dpath_string[0] = '\\';
    dpath_string[1] = '\0';

    active_command_id = 0xFFFF;
    memset(open_files, 0, sizeof(open_files));
    memset(dta_slots,  0, sizeof(dta_slots));

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
    // Patch the 68k GEMDOS handler to bypass exec_trapped_handler.
    // Both gemdrive_trap and gemdrive_trap_megaste16 have a
    //   beq.s exec_trapped_handler
    // instruction that we replace with nop, making both handlers
    // immediately fall through to the old_handler chain code.
    //
    // MegaSTE handler entry: beq.s at Atari 0xFA0806, ROM4 offset 0x0806, word 1027
    *((volatile uint16_t *)(code + 0x0806)) = 0x4E71;
    // Standard handler entry: beq.s at Atari 0xFA0828, ROM4 offset 0x0828, word 1044
    *((volatile uint16_t *)(code + 0x0828)) = 0x4E71;
    printf("DIAGNOSTIC: gemdrive_trap patched → pure pass-through (both variants)\n");
#endif

    // Monotonically incrementing seed so the 68k always sees a fresh token.
    // An oscillating XOR (0 ↔ 0x12345678) would allow the 68k to match the
    // seed against RANDOM_TOKEN left over from the previous command, causing
    // false "command complete" detection and leaving old_handler un-patched.
    static uint32_t seed_counter = 1;

    while (true)
    {
        *((volatile uint32_t *)(mem + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = ++seed_counter;
        tight_loop_contents();

        // Single volatile read — prevents a race where the DMA IRQ fires between
        // the switch's range check (reads 0xFFFF) and the default-case guard re-read
        // (now reads the real command), which caused every command to hit the default
        // case, write the token anyway, and skip the actual handler logic.
        uint16_t cmd = active_command_id;
        if (cmd == 0xFFFF)
            continue;

        printf("[DISPATCH] 0x%04x\n", cmd);
        switch (cmd)
        {
        // ── Handshake ──────────────────────────────────────────────────────
        case GEMDRVEMUL_PING:
        {
            printf("[CMD] PING\n");
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
            // The 68k driver sends payload low-word first (send_sync encoding):
            //   payloadPtr[0] = d3 low word  (low half of old GEMDOS vector)
            //   payloadPtr[1] = d3 high word (high half of old GEMDOS vector)
            //   payloadPtr[2] = d4 low word  (low half of &old_handler in ROM4)
            //   payloadPtr[3] = d4 high word (high half of &old_handler in ROM4)
            //
            // Note: old_addr reconstruction appears swapped, but the write
            // (lo before hi) cancels it out — the 68k ends up reading the
            // correct 32-bit vector value in big-endian order.
            uint32_t old_addr  = ((uint32_t)payloadPtr[0] << 16) | payloadPtr[1];
            payloadPtr += 2;
            uint32_t xbra_addr = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            uint32_t xbra_offset = xbra_addr - ATARI_ROM4_START_ADDRESS;
            printf("[CMD] SAVE_VECTORS: raw_old=0x%08lx xbra_atari=0x%08lx offset=0x%04lx\n",
                   (unsigned long)old_addr, (unsigned long)xbra_addr, (unsigned long)xbra_offset);
            if (xbra_offset < 0x10000) {
                *((volatile uint16_t *)(code + xbra_offset))     = old_addr & 0xFFFF;
                *((volatile uint16_t *)(code + xbra_offset + 2)) = old_addr >> 16;
                printf("[CMD] SAVE_VECTORS: old_handler patched at ROM4+0x%04lx\n", (unsigned long)xbra_offset);
            } else {
                printf("[CMD] SAVE_VECTORS: ERROR xbra_offset 0x%04lx out of ROM4 range!\n", (unsigned long)xbra_offset);
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
            printf("[CMD] SAVE_XBIOS_VECTOR %08lx\n", (unsigned long)xbios_old);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_SHOW_VECTOR_CALL:
        {
            printf("[CMD] SHOW_VECTOR_CALL %04x\n", (uint16_t)payloadPtr[0]);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Reentry guards ─────────────────────────────────────────────────
        case GEMDRVEMUL_REENTRY_LOCK:
        {
            // First REENTRY_LOCK means the GEMDOS handler fired for the first time
            printf("[CHKPT] REENTRY_LOCK → handler entered, GEMDOS vector OK\n");
            *((volatile uint16_t *)(mem + GEMDRVEMUL_REENTRY_TRAP)) = 0xFFFF;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_REENTRY_UNLOCK:
        {
            printf("[CHKPT] REENTRY_UNLOCK\n");
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
            printf("[CMD] SET_SHARED_VAR[%lu]=0x%08lx\n", (unsigned long)idx, (unsigned long)val);
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
            // Report a plausible small disk: 256 free clusters, 512 total,
            // 512 bytes/sector, 1 sector/cluster
            DPRINTF("DFREE\n");
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT,      256);  // free clusters
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT + 4,  512);  // total clusters
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT + 8,  512);  // bytes/sector
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT + 12, 1);    // sectors/cluster
            *((volatile uint32_t *)(mem + GEMDRVEMUL_DFREE_STATUS)) = GEMDOS_EOK;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Directory path ─────────────────────────────────────────────────
        case GEMDRVEMUL_DGETPATH_CALL:
        {
            DPRINTF("DGETPATH: %s\n", dpath_string);
            // Return current path without trailing backslash
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
            printf("[DSETPATH_RAW psize=%u]", payload_size_received);
            for (int _i = 0; _i < 20; _i++) printf(" %04x", payloadPtr[_i]);
            printf("\n");
            payloadPtr += 6;  // skip mode/address words
            char new_path[MAX_FOLDER_LENGTH] = {0};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, new_path, MAX_FOLDER_LENGTH);
            DPRINTF("DSETPATH: %s\n", new_path);

            // Strip drive letter if present
            if (new_path[1] == ':') memmove(new_path, new_path + 2, strlen(new_path) - 1);

            // Only root directory exists in M1
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
            // fspec address — skip
            payloadPtr += 2;
            // fspec string (byte-swapped)
            char raw_fspec[MAX_FOLDER_LENGTH] = {0};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, raw_fspec, MAX_FOLDER_LENGTH);
            DPRINTF("FSFIRST ndta=%08x attribs=%04x fspec=%s\n", ndta, attribs, raw_fspec);

            // Strip drive letter
            char fspec[MAX_FOLDER_LENGTH];
            if (raw_fspec[1] == ':')
                strncpy(fspec, raw_fspec + 2, MAX_FOLDER_LENGTH - 1);
            else
                strncpy(fspec, raw_fspec, MAX_FOLDER_LENGTH - 1);
            fspec[MAX_FOLDER_LENGTH - 1] = '\0';

            char dir[MAX_FOLDER_LENGTH], pat[MAX_FOLDER_LENGTH];
            split_fspec(fspec, dir, pat);
            DPRINTF("  dir=%s pattern=%s\n", dir, pat);

            // Allocate or reuse DTA slot
            FakeDTASlot *slot = alloc_dta(ndta);
            if (!slot) {
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_EINTRN;
                write_random_token(mem);
                active_command_id = 0xFFFF;
                break;
            }
            slot->attribs  = attribs;
            slot->returned = false;
            strncpy(slot->path,    dir, MAX_FOLDER_LENGTH - 1);
            strncpy(slot->pattern, pat, MAX_FOLDER_LENGTH - 1);

            // Check if README.TXT is in root and matches the pattern
            bool in_root = (dir[0] == '\\' && (dir[1] == '\0' || (dir[1] == '\\' && dir[2] == '\0')));
            char readme_upper[] = "README.TXT";
            bool matches = in_root && wildmatch(pat, readme_upper);
            DPRINTF("  in_root=%d matches=%d\n", in_root, matches);

            if (matches) {
                slot->returned = true;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = 0;
                write_dta_readme(mem);
            } else {
                release_dta(ndta);
                memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_EFILNF;
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FSNEXT_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            DPRINTF("FSNEXT ndta=%08x\n", ndta);
            FakeDTASlot *slot = find_dta(ndta);
            if (slot && slot->returned) {
                // Only one file — no more
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_ENMFIL;
                release_dta(ndta);
            } else {
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_EINTRN;
                if (slot) release_dta(ndta);
            }
            memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── File open/close/seek/read ──────────────────────────────────────
        case GEMDRVEMUL_FOPEN_CALL:
        {
            printf("[FOPEN_RAW psize=%u]", payload_size_received);
            for (int _i = 0; _i < 20; _i++) printf(" %04x", payloadPtr[_i]);
            printf("\n");
            uint16_t mode = payloadPtr[0];
            payloadPtr += 6;  // skip mode + 5 address words
            char filename[MAX_FOLDER_LENGTH] = {0};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, filename, MAX_FOLDER_LENGTH);
            DPRINTF("FOPEN mode=%d file=%s\n", mode, filename);

            // Strip drive letter and leading backslash
            char *fptr = filename;
            if (fptr[1] == ':') fptr += 2;
            if (*fptr == '\\' || *fptr == '/') fptr++;

            // Compare case-insensitive to README.TXT
            char upper[MAX_FOLDER_LENGTH];
            str_upper(fptr, upper, MAX_FOLDER_LENGTH);

            if (strcmp(upper, "README.TXT") == 0) {
                if (mode == 0) {  // read-only
                    int fd = alloc_fd();
                    if (fd < 0) {
                        WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)(int32_t)GEMDOS_ENHNDL);
                    } else {
                        WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)fd);
                        DPRINTF("  opened fd=%d\n", fd);
                    }
                } else {
                    WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)(int32_t)GEMDOS_EACCDN);
                }
            } else {
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)(int32_t)GEMDOS_EFILNF);
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FCLOSE_CALL:
        {
            uint16_t fd = payloadPtr[0];
            DPRINTF("FCLOSE fd=%d\n", fd);
            FakeOpenFile *f = get_fd(fd);
            if (!f) {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EIHNDL;
            } else {
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

            FakeOpenFile *f = get_fd(fd);
            if (!f) {
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FSEEK_STATUS, (uint32_t)(int32_t)GEMDOS_EIHNDL);
            } else {
                uint32_t new_pos;
                if (whence == 0)      new_pos = offset;
                else if (whence == 1) new_pos = f->offset + offset;
                else                  new_pos = README_SIZE + offset;  // from end (offset usually <= 0)

                if (new_pos > README_SIZE) new_pos = README_SIZE;
                f->offset = new_pos;
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FSEEK_STATUS, new_pos);
                DPRINTF("  new_pos=%d\n", new_pos);
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_READ_BUFF_CALL:
        {
            uint16_t fd         = payloadPtr[0];
            payloadPtr += 2;
            uint32_t req_bytes  = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;
            // pending_bytes_to_read — available but not used for single-chunk read
            DPRINTF("READ_BUFF fd=%d req=%d\n", fd, req_bytes);

            FakeOpenFile *f = get_fd(fd);
            if (!f) {
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_READ_BYTES, (uint32_t)(int32_t)GEMDOS_EIHNDL);
            } else {
                uint32_t avail = (f->offset < README_SIZE) ? (README_SIZE - f->offset) : 0;
                uint32_t to_read = (req_bytes < avail) ? req_bytes : avail;
                if (to_read > DEFAULT_FOPEN_READ_BUFFER_SIZE)
                    to_read = DEFAULT_FOPEN_READ_BUFFER_SIZE;

                // Copy data to READ_BUFF, then endian-swap word pairs
                uint8_t *dst = (uint8_t *)(mem + GEMDRVEMUL_READ_BUFF);
                memcpy(dst, readme_content + f->offset, to_read);
                // Pad to even byte count before swapping
                uint32_t swap_size = to_read + (to_read & 1);
                if (to_read & 1) dst[to_read] = 0;
                CHANGE_ENDIANESS_BLOCK16(mem + GEMDRVEMUL_READ_BUFF, swap_size);

                f->offset += to_read;
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_READ_BYTES, to_read);
                DPRINTF("  read %d bytes, new offset=%d\n", to_read, f->offset);
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

            if (strcmp(upper, "README.TXT") == 0)
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FATTRIB_STATUS)) = README_ATTR;
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

            if (strcmp(upper, "README.TXT") == 0) {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_DATE))   = README_DATE;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_TIME))   = README_TIME;
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
            // Print raw payload for diagnostics
            printf("[PEXEC_RAW psize=%u]", payload_size_received);
            for (int _i = 0; _i < 10; _i++) printf(" %04x", payloadPtr[_i]);
            printf("\n");

            // WRITE_AND_SWAP_LONGWORD swaps the two 16-bit words before storing,
            // so the 68k reads back the original value. The ROM checks
            // CMPI.W #0, $FB4184 (reads the HIGH WORD of PEXEC_MODE).
            // Value 0x00000001 → HIGH WORD = 0x0000 → equal → BEQ → FOPEN crash.
            // Value 0x00010000 → HIGH WORD = 0x0001 → not equal → BNE → FA08E4
            //   → old_handler (original TOS Pexec), which avoids the null-fname FOPEN.
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_PEXEC_MODE, 0x00010000);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        default:
            printf("[CMD] Unhandled cmd 0x%04x — clearing\n", cmd);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }
    }
}
