#include "supervisor/config.h"
#include "supervisor/process.h"
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
static bool fixture_published, matches;
static unsigned int identity_reads, terminations;
static int fixture_lstat(const char *path, struct stat *info)
{ (void)path; (void)info; if (fixture_published) return 0; errno = ENOENT; return -1; }
static bool fixture_identity(const char *path, const struct cs_identity *identity)
{ (void)path; (void)identity; ++identity_reads; bool ready = fixture_published && matches;
  fixture_published = true; /* Simulate atomic publication between read and lstat. */
  return ready; }
static bool fixture_term(pid_t pid, bool *sent)
{ (void)pid; ++terminations; *sent = true; return true; }
#define lstat fixture_lstat
#define sc_identity_file fixture_identity
#define sc_term_once fixture_term
int unused_supervisor_main(int argc, char **argv);
#define main unused_supervisor_main
#include "../src/main.c"
#undef main
#undef lstat
#undef sc_identity_file
#undef sc_term_once
static struct supervisor *setup(void)
{
    struct supervisor *s = calloc(1U, sizeof(*s)); assert(s);
    s->selected = &s->config.games[0]; s->ra_pid = 77;
    s->ui_fd = -1; s->hook_pending = true; s->hook.request.operation = CS_OP_START;
    s->hook_deadline = 1000U; matches = true; fixture_published = false;
    identity_reads = terminations = 0U;
    return s;
}
static void finish(struct supervisor *s)
{ sc_discard(&s->hook_out); sc_discard(&s->ui_out); free(s); }
int main(void)
{
    struct supervisor *s = setup();
    service_hook(s, 1U);
    assert(s->hook_pending && identity_reads == 0U && terminations == 0U);
    fixture_published = true; service_hook(s, 2U);
    assert(!s->hook_pending && identity_reads == 1U && terminations == 0U);
    struct sc_message *reply = s->hook_out.packets[s->hook_out.head];
    assert(reply && reply->data.result == CS_RESULT_COMPLETED); finish(s);
    s = setup(); fixture_published = true; matches = false; service_hook(s, 1U);
    assert(!s->hook_pending && terminations == 1U);
    reply = s->hook_out.packets[s->hook_out.head]; assert(reply->data.result == CS_RESULT_FAILED);
    finish(s);
    s = setup(); service_hook(s, 1000U); assert(!s->hook_pending && terminations == 1U); finish(s);
    puts("READINESS_ATOMIC_PUBLICATION_RACE_PASS real_service_hook absent_wait_valid_accept_mismatch_reject_deadline_reject");
    return 0;
}
