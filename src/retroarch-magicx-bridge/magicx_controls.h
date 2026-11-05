/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MAGICX_CONTROLS_H
#define MAGICX_CONTROLS_H
#include <stdbool.h>
#include <stdint.h>
#include "sessiond/control_protocol.h"
/* Single outstanding, bounded, nonblocking local request. No implicit retry:
 * a lost ACK must never duplicate a save or a load. ACK only means queued. */
struct magicx_controls {
    int fd;
    uint64_t token, deadline, next_token;
    uint32_t operation;
    bool sent, has_status;
    struct cs_session_status status;
};
/* Correlation is tied to daemon session + command token. Neither ACK nor time
 * passage is evidence of completion. A status query is read-only. */
enum magicx_feedback { MX_FEEDBACK_IDLE, MX_FEEDBACK_QUEUED, MX_FEEDBACK_BUSY,
                       MX_FEEDBACK_DONE, MX_FEEDBACK_FAILED };
struct magicx_control_feedback {
    struct cs_session_status observed;
    uint64_t token, session, baseline_generation;
    uint32_t operation;
    enum magicx_feedback phase;
    bool known;
};
bool magicx_feedback_begin(struct magicx_control_feedback *feedback,
                          uint32_t operation, uint64_t token);
bool magicx_feedback_observe(struct magicx_control_feedback *feedback,
                            const struct cs_session_status *observed);
void magicx_controls_init(struct magicx_controls *controls);
bool magicx_controls_request(struct magicx_controls *controls, const char *socket_path,
                             uint32_t operation, uint64_t now_ms);
/* 0 waiting/idle, 1 accepted by supervisor, -1 rejected/lost/timed out. */
int magicx_controls_poll(struct magicx_controls *controls, uint64_t now_ms);
void magicx_controls_close(struct magicx_controls *controls);
#endif
