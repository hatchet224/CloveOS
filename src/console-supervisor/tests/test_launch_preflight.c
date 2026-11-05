#include "supervisor/disk.h"
#include "supervisor/tools_backend.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static unsigned int hashes,probes,slots;
static bool reject_hash;
static enum cs_store_status fixture_reader(struct cs_store *s,const struct cs_store_config *c)
{(void)s;(void)c;return CS_STORE_OK;}
static enum cl_history_result fixture_history(struct cs_store *s,struct cl_history *h,enum cs_store_status status[2])
{(void)s;memset(h,0,sizeof(*h));status[0]=status[1]=CS_STORE_EMPTY;return CL_HISTORY_EMPTY;}
static enum cs_store_status fixture_resume(struct cs_store *s,bool previous,struct cs_record *r)
{(void)s;(void)previous;(void)r;return CS_STORE_EMPTY;}
static enum cl_availability fixture_probe(struct cs_store *s,const struct cl_entry *e,
    const struct cs_identity *id,const struct cl_reference *ref)
{(void)s;(void)e;(void)id;(void)ref;++probes;return CL_MISSING;}
static enum cs_store_status fixture_slot(struct cs_store *s,const struct cs_identity *id,unsigned int slot,struct cs_record *r)
{(void)s;(void)id;(void)slot;(void)r;++slots;return CS_STORE_EMPTY;}
static bool fixture_hash(const char *p,const char expected[65],uint64_t bound)
{(void)p;(void)expected;(void)bound;++hashes;return !reject_hash;}
static bool fixture_enabled(struct sc_tools_backend *b,const struct cs_store *s,const struct sc_game *g)
{(void)b;(void)s;(void)g;return true;}
#define cs_store_open_reader fixture_reader
#define cl_history_read_catalog fixture_history
#define cs_store_read_resume fixture_resume
#define cl_history_probe fixture_probe
#define cs_store_slot_read fixture_slot
#define sc_hash_path fixture_hash
#define sc_tools_game_enabled fixture_enabled
#include "../src/disk.c"
#undef cs_store_open_reader
#undef cl_history_read_catalog
#undef cs_store_read_resume
#undef cl_history_probe
#undef cs_store_slot_read
#undef sc_hash_path
#undef sc_tools_game_enabled
int main(void)
{
    struct sc_config *c=calloc(1U,sizeof(*c));struct sc_message *q=calloc(1U,sizeof(*q));
    struct sc_disk_result *r=calloc(1U,sizeof(*r));struct sc_tools_backend backend={0};
    assert(c&&q&&r);c->game_count=1U;
    memset(c->games[0].entry.identity.game,'a',64U);
    q->type=SC_UI_PROBE;q->data.probe.entry=c->games[0].entry;q->token=UINT64_MAX;
    job(c,&backend,q,r);
    assert(r->ok&&r->message.type==SC_UI_STATES&&r->message.data.states.game_status==CL_READY);
    assert(hashes==3U&&probes==0U&&slots==0U);
    memset(r,0,sizeof(*r));hashes=0U;q->token=UINT64_C(1)<<32;
    job(c,&backend,q,r);
    assert(r->ok&&hashes==3U&&probes==2U&&slots==CL_MANUAL_MAX);
    memset(r,0,sizeof(*r));hashes=probes=slots=0U;q->token=UINT64_MAX;reject_hash=true;
    job(c,&backend,q,r);
    assert(r->message.data.states.game_status!=CL_READY&&hashes==1U&&probes==0U&&slots==0U);
    free(c);free(q);free(r);
    puts("LAUNCH_PREFLIGHT_PASS authenticates all three launch files; skips unrelated state/slot scans only for internal preflight; browser keeps full scans; hash failure rejects launch (filesystem/hash adapters mocked)");
    return 0;
}
