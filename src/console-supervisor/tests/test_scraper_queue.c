#ifdef NDEBUG
#error "Scraper queue regression needs assertions"
#endif
#ifndef DISK_SOURCE
#define DISK_SOURCE "../src/disk.c"
#endif
#include DISK_SOURCE
#include <assert.h>
int main(void)
{
    struct sc_disk *d=calloc(1U,sizeof(*d));struct sc_message m;
    assert(d && pthread_mutex_init(&d->mutex,NULL)==0 && pthread_cond_init(&d->condition,NULL)==0);
    d->online_cancel_epoch=7U;
    sc_message_init(&m,SC_UI_TOOL,1U);m.data.tool_request.operation=CL_TOOL_SCRAPE_MISSING;
    m.data.tool_request.revision=0U;assert(sc_disk_submit(d,&m));
    assert(d->jobs[0]->data.tool_request.revision==0U);
    m.data.tool_request.revision=15U;m.data.tool_request.enabled_mask=(UINT64_C(3)<<32U)|2U;
    assert(sc_disk_submit(d,&m));
    assert(d->jobs[1]->data.tool_request.revision==15U && d->jobs[1]->data.tool_request.enabled_mask==m.data.tool_request.enabled_mask);
#ifndef REPRO_BEFORE
    assert(queued_cancel_epoch(d->jobs[0])==7U && queued_cancel_epoch(d->jobs[1])==7U);
    d->online_job_epoch=queued_cancel_epoch(d->jobs[1]);assert(!online_cancelled(d));
    m.data.tool_request.operation=CL_TOOL_CANCEL_ONLINE;assert(sc_disk_submit(d,&m));
    assert(d->job_count==2U && d->online_cancel_epoch==8U && online_cancelled(d));
    m.data.tool_request.operation=CL_TOOL_SCRAPE_MISSING;m.data.tool_request.revision=16U;
    assert(sc_disk_submit(d,&m));assert(d->jobs[2]->data.tool_request.revision==16U && queued_cancel_epoch(d->jobs[2])==8U);
    d->online_job_epoch=queued_cancel_epoch(d->jobs[2]);assert(!online_cancelled(d));
    puts("SCRAPER_BATCH_CURSOR_COUNTS_PRESERVED_CANCEL_EPOCH_INDEPENDENT_PASS");
#endif
    for(size_t i=0U;i<d->job_count;++i)free(d->jobs[i]);
    assert(pthread_cond_destroy(&d->condition)==0 && pthread_mutex_destroy(&d->mutex)==0);free(d);return 0;
}
