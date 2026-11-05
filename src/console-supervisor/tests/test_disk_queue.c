#ifdef NDEBUG
#error "Queue fixture requires assertions"
#endif
#include "../src/disk.c"
#include <assert.h>

static void fixture_open(struct sc_disk *d)
{
    memset(d, 0, sizeof(*d));
    assert(pthread_mutex_init(&d->mutex, NULL) == 0);
    assert(pthread_cond_init(&d->condition, NULL) == 0);
    d->event_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(d->event_fd >= 0);
}
static void fixture_close(struct sc_disk *d)
{
    for (size_t i = 0U; i < d->job_count; ++i)
        free(d->jobs[(d->job_head + i) % SC_DISK_JOBS]);
    assert(d->result_count == 0U);
    (void)close(d->event_fd);
    (void)pthread_cond_destroy(&d->condition);
    (void)pthread_mutex_destroy(&d->mutex);
}
static void *produce_results(void *opaque)
{
    struct sc_disk *d = opaque;
    for (uint64_t i = 0U; i < 12U; ++i) {
        struct sc_disk_result *r = calloc(1U, sizeof(*r));
        assert(r != NULL);
        r->message.token = i;
        publish(d, r);
    }
    return NULL;
}
int main(void)
{
    struct sc_disk *d = calloc(1U, sizeof(*d));
    struct sc_message *q = calloc(1U, sizeof(*q));
    struct sc_disk_result *result = calloc(1U, sizeof(*result));
    pthread_t producer;
    assert(d != NULL && q != NULL && result != NULL);
    fixture_open(d);
    for (uint64_t i = 0U; i < 1000U; ++i) {
        q->type = SC_UI_STATUS; q->token = i;
        assert(sc_disk_submit(d, q));
    }
    assert(d->job_count == 1U && d->jobs[0]->token == 999U);
    assert(!foreground_waiting(d));
    q->type = SC_DIAGNOSTICS;
    assert(sc_disk_submit(d, q));
    q->type = SC_UI_LIBRARY; q->token = UINT64_C(1) << 32;
    assert(sc_disk_submit(d, q));
    for (uint64_t i = 1U; i < 1000U; ++i) {
        q->token = (UINT64_C(1) << 32) | i;
        assert(sc_disk_submit(d, q));
    }
    assert(d->job_count == 3U);
    assert(foreground_waiting(d));
    q->type = SC_UI_PROBE; q->token = (UINT64_C(1) << 32) | 2000U;
    assert(sc_disk_submit(d, q));
    q->token = UINT64_MAX; /* Internal preflight may not replace the UI probe. */
    assert(sc_disk_submit(d, q));
    assert(d->job_count == 5U);
    q->type = SC_UI_TOOL; q->data.tool_request.operation = CL_TOOL_CONNECT_NETWORK;
    for (uint64_t i = 0U; i < 3U; ++i) {
        q->token = i;
        assert(sc_disk_submit(d, q));
    }
    assert(d->job_count == SC_DISK_JOBS && !sc_disk_submit(d, q));
    sc_disk_finish(d);
    assert(d->job_count == 3U);
    for (size_t i = 0U; i < 3U; ++i)
        assert(d->jobs[i]->type == SC_UI_TOOL && d->jobs[i]->token == i);
    assert(!sc_disk_submit(d, q));
    fixture_close(d);
    fixture_open(d);
    /* Startup requests Systems and Favorites concurrently. Neither may erase
     * the other's awaited response; only a newer request of the same mode wins. */
    memset(q, 0, sizeof(*q));
    q->type = SC_UI_LIBRARY;
    for (unsigned int mode = 0U; mode < 3U; ++mode) {
        q->token = (UINT64_C(2) << 32) | mode;
        q->data.history.library_mode = (enum cl_library_mode)mode;
        q->data.history.library_query_id = mode + 1U;
        assert(sc_disk_submit(d, q));
    }
    assert(d->job_count == 3U);
    q->data.history.library_mode = CL_LIBRARY_SYSTEMS;
    q->data.history.library_query_id = 99U;
    assert(sc_disk_submit(d, q));
    assert(d->job_count == 3U);
    for (size_t i = 0U; i < 3U; ++i) {
        const struct sc_message *m = d->jobs[i];
        assert(m->data.history.library_query_id ==
               (m->data.history.library_mode == CL_LIBRARY_SYSTEMS ? 99U :
                (uint64_t)m->data.history.library_mode + 1U));
    }
    fixture_close(d);
    fixture_open(d);
    assert(pthread_create(&producer, NULL, produce_results, d) == 0);
    for (size_t spins = 0U; spins < 3000U; ++spins) {
        size_t count;
        (void)pthread_mutex_lock(&d->mutex); count = d->result_count;
        (void)pthread_mutex_unlock(&d->mutex);
        if (count == 4U) break;
        (void)usleep(1000U);
    }
    for (uint64_t i = 0U; i < 12U; ++i) {
        bool received = false;
        for (size_t spins = 0U; spins < 3000U && !received; ++spins) {
            received = sc_disk_result(d, result);
            if (!received) (void)usleep(1000U);
        }
        assert(received && result->message.token == i);
    }
    assert(pthread_join(producer, NULL) == 0);
    fixture_close(d);
    free(result); free(q); free(d);
    puts("DISK_QUEUE_PASS telemetry/library coalescing; UI/preflight isolation; bounded admission; accepted mutations retained on shutdown; twelve acknowledgements across four result slots");
    return 0;
}
