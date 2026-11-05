/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "magicx_controls.h"
#include "sessiond/control_protocol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s (%d)\n", __FILE__, __LINE__, #x, errno);                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
int main(void)
{
    char directory[] = "/tmp/magicx-controls-XXXXXX", path[108];
    struct sockaddr_un address;
    struct magicx_controls controls;
    struct cs_control_header request;
    unsigned char reply[28];
    int32_t result;
    int server, peer;
    unsigned int scenario;
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/control.sock", directory) > 0);
    server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    CHECK(server >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1U);
    CHECK(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(chmod(path, 0600) == 0 && listen(server, 4) == 0);
    magicx_controls_init(&controls);
    CHECK(!magicx_controls_request(&controls, path, 100U, 0U));
    CHECK(chmod(path, 0666) == 0);
    CHECK(!magicx_controls_request(&controls, path, CS_CONTROL_CHECKPOINT, 0U));
    CHECK(chmod(path, 0600) == 0);
    for (scenario = 0U; scenario < 7U; ++scenario) {
        CHECK(magicx_controls_request(&controls, path, CS_CONTROL_CHECKPOINT, 100U));
        CHECK(!magicx_controls_request(&controls, path, CS_CONTROL_RESTORE, 100U));
        peer = accept(server, NULL, NULL);
        CHECK(peer >= 0);
        CHECK(magicx_controls_poll(&controls, 101U) == 0);
        CHECK(recv(peer, &request, sizeof(request), 0) == (ssize_t)sizeof(request));
        CHECK(request.magic == CS_CONTROL_MAGIC && request.version == CS_CONTROL_VERSION &&
              request.type == CS_CONTROL_CHECKPOINT && request.length == 0U && request.token != 0U);
        if (scenario == 5U) {
            CHECK(magicx_controls_poll(&controls, 2100U) == -1); /* bounded wait */
        } else if (scenario == 6U) {
            CHECK(close(peer) == 0);
            peer = -1;
            CHECK(magicx_controls_poll(&controls, 102U) == -1); /* no retry on disconnect */
        } else {
            request.type = CS_CONTROL_ACK;
            request.length = sizeof(result);
            result = scenario == 1U ? 2 : 0;
            if (scenario == 2U) {
                ++request.token;
            }
            if (scenario == 3U) {
                ++request.version;
            }
            memcpy(reply, &request, sizeof(request));
            memcpy(reply + sizeof(request), &result, sizeof(result));
            CHECK(send(peer, reply, scenario == 4U ? sizeof(reply) - 1U : sizeof(reply),
                       MSG_NOSIGNAL) > 0);
            CHECK(magicx_controls_poll(&controls, 102U) == (scenario == 0U ? 1 : -1));
        }
        CHECK(controls.fd == -1 && magicx_controls_poll(&controls, 3000U) == 0);
        if (peer >= 0) {
            CHECK(close(peer) == 0);
        }
    }
    for (scenario = 0U; scenario < 7U; ++scenario) {
        struct { struct cs_control_header header; struct cs_session_status value; } observation;
        CHECK(magicx_controls_request(&controls, path, CS_CONTROL_SESSION_STATUS, 3000U));
        peer = accept(server, NULL, NULL); CHECK(peer >= 0);
        CHECK(magicx_controls_poll(&controls, 3001U) == 0);
        CHECK(recv(peer, &request, sizeof(request), 0) == (ssize_t)sizeof(request));
        memset(&observation, 0, sizeof(observation));
        observation.header = request;
        observation.header.type = CS_CONTROL_SESSION_STATUS_REPLY;
        observation.header.length = sizeof(observation.value);
        observation.value.slot = scenario == 1U ? 16U : 3U;
        observation.value.busy = scenario == 2U ? 2U : 0U;
        observation.value.active = 1U; observation.value.session = 9U;
        observation.value.sequence = 21U;
        if (scenario == 3U) ++observation.header.token;
        if (scenario == 4U) observation.value.operation = CS_CONTROL_SAVE_SLOT; /* missing token */
        if (scenario == 5U) observation.value.operation = CS_CONTROL_MENU; /* not a manual result */
        CHECK(send(peer, &observation, sizeof(observation) - (scenario == 6U ? 1U : 0U), MSG_NOSIGNAL) > 0);
        CHECK(magicx_controls_poll(&controls, 3002U) == (scenario == 0U ? 1 : -1));
        CHECK(controls.has_status == (scenario == 0U));
        if (scenario == 0U) CHECK(controls.status.slot == 3U && controls.status.session == 9U);
        CHECK(close(peer) == 0);
    }
    {
        struct magicx_control_feedback f = {0};
        struct cs_session_status o = {.active=1U, .slot=3U, .session=9U, .generation=8U, .sequence=20U};
        CHECK(!magicx_feedback_begin(&f, CS_CONTROL_SAVE_SLOT, 1U));
        CHECK(!magicx_feedback_observe(&f, &o) && f.known);
        CHECK(magicx_feedback_begin(&f, CS_CONTROL_SAVE_SLOT, 1U));
        CHECK(f.phase == MX_FEEDBACK_QUEUED);
        CHECK(!magicx_feedback_observe(&f, &o)); /* cached idle is not completion */
        o.operation=CS_CONTROL_SAVE_SLOT; o.operation_token=2U; ++o.sequence;
        CHECK(!magicx_feedback_observe(&f,&o)); /* another token is not completion */
        o.operation_token=1U; o.busy=1U; ++o.sequence;
        CHECK(magicx_feedback_observe(&f,&o) && f.phase==MX_FEEDBACK_BUSY);
        CHECK(!magicx_feedback_begin(&f,CS_CONTROL_LOAD_SLOT,3U));
        o.busy=0U; o.generation=9U; ++o.sequence;
        CHECK(magicx_feedback_observe(&f,&o) && f.phase==MX_FEEDBACK_DONE);
        CHECK(magicx_feedback_begin(&f,CS_CONTROL_SAVE_SLOT,3U));
        o.operation_token=3U; ++o.sequence; /* terminal without generation advance fails */
        CHECK(magicx_feedback_observe(&f,&o) && f.phase==MX_FEEDBACK_FAILED);
        CHECK(magicx_feedback_begin(&f,CS_CONTROL_LOAD_SLOT,4U));
        o.operation=CS_CONTROL_LOAD_SLOT; o.operation_token=4U; ++o.sequence;
        CHECK(magicx_feedback_observe(&f,&o) && f.phase==MX_FEEDBACK_DONE);
        CHECK(magicx_feedback_begin(&f,CS_CONTROL_SLOT_UP,5U));
        o.operation=CS_CONTROL_SLOT_UP; o.operation_token=5U; o.slot=4U; ++o.sequence;
        CHECK(magicx_feedback_observe(&f,&o) && f.phase==MX_FEEDBACK_DONE);
        CHECK(magicx_feedback_begin(&f,CS_CONTROL_SCREENSHOT,6U));
        o.operation=CS_CONTROL_SCREENSHOT; o.operation_token=6U; o.failure=1U; ++o.sequence;
        CHECK(magicx_feedback_observe(&f,&o) && f.phase==MX_FEEDBACK_FAILED);
        CHECK(magicx_feedback_begin(&f,CS_CONTROL_LOAD_SLOT,7U));
        CHECK(!magicx_feedback_observe(&f,&o) && f.phase==MX_FEEDBACK_QUEUED); /* previous failure cached */
        o.failure=0U;
        ++o.session; ++o.sequence;
        CHECK(magicx_feedback_observe(&f,&o) && f.phase==MX_FEEDBACK_FAILED);
    }
    magicx_controls_close(&controls);
    CHECK(close(server) == 0 && unlink(path) == 0 && rmdir(directory) == 0);
    puts("PASS: control framing, queued ACK, rejection, token/version/length checks, timeout, no "
         "retry; authoritative status framing and token/session/generation completion correlation");
    return 0;
}
