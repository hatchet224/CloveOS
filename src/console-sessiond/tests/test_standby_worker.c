#include "../src/worker.c"
#include <assert.h>
#include <stdlib.h>
static enum cs_result lifecycle_fixture(void *context, const struct cs_request *r,
                                        bool active, bool degraded)
{
    unsigned int *calls=context;(void)active;(void)degraded;
    assert(r->operation==CS_OP_STANDBY_LEAVE);++*calls;return CS_RESULT_COMPLETED;
}
int main(void)
{
    struct cs_worker *w=calloc(1U,sizeof(*w));struct cs_job job={0};struct cs_event result;
    unsigned int calls=0U;assert(w!=NULL);w->managed=true;
    w->lifecycle=lifecycle_fixture;w->lifecycle_context=&calls;
    job.request.operation=CS_OP_STANDBY_LEAVE;
    result=run_job(w,&job);assert(result.result==CS_RESULT_COMPLETED&&calls==1U);
    job.request.operation=CS_OP_STANDBY_ENTER;
    result=run_job(w,&job);assert(result.result==CS_RESULT_FAILED&&calls==1U);
    assert((result.flags&CS_REPLY_MEDIA_UNAVAILABLE)!=0U);
    free(w);puts("STANDBY_WORKER_PASS missing card cannot prevent display restore; missing card forbids display-off acknowledgement");
    return 0;
}
