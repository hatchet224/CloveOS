#ifndef SESSIOND_CONTROL_PROTOCOL_H
#define SESSIOND_CONTROL_PROTOCOL_H
#include <stdint.h>
/* Tiny subset of the private local supervisor SOCK_SEQPACKET protocol. Linux
 * same-build ABI, header 24 bytes, no paths/pointers. Endpoint ACK means queued,
 * never that checkpoint bytes are durable. BRIDGE_PROTOCOL.md is unchanged. */
#define CS_CONTROL_MAGIC UINT32_C(0x53434e31)
#define CS_CONTROL_VERSION 3U
#define CS_CONTROL_ACK 5U
#define CS_CONTROL_RESULT_ACCEPTED 0
#define CS_CONTROL_MENU 26U
#define CS_CONTROL_CHECKPOINT 23U
#define CS_CONTROL_RESTORE 24U
#define CS_CONTROL_HOTKEY_CONSUMED 25U
#define CS_CONTROL_QUICK_MENU 30U
#define CS_CONTROL_SAVE_SLOT 31U
#define CS_CONTROL_LOAD_SLOT 32U
#define CS_CONTROL_SLOT_UP 33U
#define CS_CONTROL_SLOT_DOWN 34U
#define CS_CONTROL_SCREENSHOT 35U
#define CS_CONTROL_QUICK_MENU_CLOSE 36U
#define CS_CONTROL_SESSION_STATUS 40U
#define CS_CONTROL_SESSION_STATUS_REPLY 41U
#define CS_CONTROL_RECOVERY_RETRY 42U
#define CS_CONTROL_RECOVERY_CONTINUE 43U
#define CS_CONTROL_RECOVERY_OFF 44U
#define CS_CONTROL_RECOVERY_MENU 45U
#define CS_RECOVERY_RETRY 1U
#define CS_RECOVERY_CONTINUE 2U
#define CS_RECOVERY_OFF 4U
#define CS_RECOVERY_MENU 8U
/* Read-only authoritative daemon observation. Operation/token correlate an
 * actual processed manual command; a queued ACK is never completion. */
struct cs_session_status {
    uint32_t slot, busy, failure, active, quick_menu, operation, recovery, available;
    uint64_t generation, session, operation_token, sequence;
};
struct cs_control_header {
    uint32_t magic, version, type, length;
    uint64_t token;
};
#endif
