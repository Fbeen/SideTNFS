#ifndef TNFS_CLIENT_H_
#define TNFS_CLIENT_H_

#ifdef SIDETNFS_DEBUG

#include <stdint.h>
#include <stdbool.h>
#include "lwip/ip_addr.h"

// ─── TNFS protocol constants ──────────────────────────────────────────────────

#define TNFS_PORT              16384u
#define TNFS_MTU               512u

// Version written into MOUNT payload bytes [4..5] — minor byte first, then major
// Matches tnfscmdr reference: const char TNFS_PROTOCOL_VERSION[] = {0x02, 0x01}
#define TNFS_PROTO_VER_MINOR   0x02
#define TNFS_PROTO_VER_MAJOR   0x01

// Command opcodes (TNFS spec §4)
#define TNFS_CMD_MOUNT         0x00u
#define TNFS_CMD_UMOUNT        0x01u
#define TNFS_CMD_OPENDIR       0x10u
#define TNFS_CMD_READDIR       0x11u
#define TNFS_CMD_CLOSEDIR      0x12u
#define TNFS_CMD_OPENDIRX      0x17u   // extended opendir: returns count + handle
#define TNFS_CMD_READDIRX      0x18u   // extended readdir: returns flags + size + name per entry
#define TNFS_CMD_STAT          0x24u
#define TNFS_CMD_OPEN          0x29u
#define TNFS_CMD_READ          0x21u
#define TNFS_CMD_CLOSE         0x23u

// Result codes
#define TNFS_OK                0x00u
#define TNFS_EOF               0x21u   // end of file / end of directory

// READDIRX entry flags (bits in the flags byte of each entry)
#define TNFS_DIRENTRY_DIR      0x01u   // entry is a directory
#define TNFS_DIRENTRY_HIDDEN   0x02u   // entry is hidden
#define TNFS_DIRENTRY_SPECIAL  0x04u   // entry is a special file (device, socket, …)

// READDIRX response status byte (buf[6] of a READDIRX response)
#define TNFS_DIRSTATUS_EOF     0x01u   // this is the last batch; no more entries follow

// ─── Transport API ────────────────────────────────────────────────────────────
// All calls must be made inside cyw43_arch_lwip_begin() / cyw43_arch_lwip_end().

// Open a connected UDP PCB toward the TNFS server. Returns false on failure.
bool     tnfs_client_open(const ip_addr_t *server_ip, uint16_t server_port);

// Remove the UDP PCB. Safe to call when already closed.
void     tnfs_client_close(void);

// Send buf[len] to the server. Returns false on lwIP error.
bool     tnfs_client_send(const uint8_t *buf, uint16_t len);

// Copy the most-recently received response into dst (max max_len bytes).
// Returns bytes copied, or 0 if no new response is pending.
// Clears the pending flag — a second call returns 0 until a new packet arrives.
uint16_t tnfs_client_recv(uint8_t *dst, uint16_t max_len);

#endif // SIDETNFS_DEBUG
#endif // TNFS_CLIENT_H_
