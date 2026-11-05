#include "preference_record.h"
#include "sessiond/json.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum record_status { RECORD_OK, RECORD_MISSING, RECORD_BAD, RECORD_IO };

void cl_preference_digest(const char *uuid,const struct cl_preference_snapshot *s,
                   unsigned int schema,char hash[65])
{
    char canonical[160];
    int n;
    if(schema==1U)
        n=snprintf(canonical,sizeof(canonical),"console-ui-preferences-v1\n%s\n%" PRIu64 "\n%u\n%u\n",uuid,s->revision,s->grid?1U:0U,s->main_tab);
    else if(schema==2U)
        n=snprintf(canonical,sizeof(canonical),"console-ui-preferences-v2\n%s\n%" PRIu64 "\n%u\n%u\n%" PRIu64 "\n",uuid,s->revision,s->grid?1U:0U,s->main_tab,s->appearance);
    else
        n=snprintf(canonical,sizeof(canonical),"console-ui-preferences-v3\n%s\n%" PRIu64 "\n%u\n%u\n%" PRIu64 "\n%u\n",uuid,s->revision,s->grid?1U:0U,s->main_tab,s->appearance,(unsigned int)s->shortcuts);
    if(n<0||(size_t)n>=sizeof(canonical)){hash[0]='\0';return;}
    cs_sha256_hex(canonical,(size_t)n,hash);
}
static enum record_status read_record(const struct cs_store *store,const char *name,
                                      struct cl_preference_snapshot *out)
{
    static const char *const keys[]={"schema","uuid","revision","grid","main_tab","digest"};
    static const char *const keys_v2[]={"schema","uuid","revision","grid","main_tab","appearance","digest"};
    static const char *const keys_v3[]={"schema","uuid","revision","grid","main_tab","appearance","shortcuts","digest"};
    struct cs_json json;struct cl_preference_snapshot parsed={0};struct stat st;
    char bytes[CL_PREFERENCE_RECORD_MAX],expected[65];uint64_t schema,tab,appearance=0U,shortcuts=0U;size_t used=0U;
    const char *uuid,*hash;enum record_status status=RECORD_BAD;
    int fd=openat(store->data_fd,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?RECORD_MISSING:(errno==ELOOP?RECORD_BAD:RECORD_IO);
    if(fstat(fd,&st)!=0){status=RECORD_IO;goto done;}
    if(!S_ISREG(st.st_mode)||st.st_nlink!=1U||st.st_dev!=store->device||
       st.st_uid!=geteuid()||(st.st_mode&0077U)!=0U||st.st_size<1||st.st_size>(off_t)sizeof(bytes))goto done;
    while(used<(size_t)st.st_size){ssize_t n=read(fd,bytes+used,(size_t)st.st_size-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){status=RECORD_IO;goto done;}used+=(size_t)n;}
    if(!cs_json_parse(&json,bytes,used)||!cs_json_u64(&json,"schema",&schema)||
       (schema<1U||schema>3U)||
       !(schema==1U?cs_json_keys(&json,keys,6U):
         schema==2U?cs_json_keys(&json,keys_v2,7U):cs_json_keys(&json,keys_v3,8U))||
       !cs_json_u64(&json,"revision",&parsed.revision)||!cs_json_bool(&json,"grid",&parsed.grid)||
       !cs_json_u64(&json,"main_tab",&tab)||tab>CL_MAX_MAIN_TAB ||
       (schema==1U && tab==CL_MAX_MAIN_TAB))goto done;
    if(schema>=2U&&(!cs_json_u64(&json,"appearance",&appearance)||
                   (appearance&CL_BATTERY_PERCENTAGE_MASK)==CL_BATTERY_PERCENTAGE_MASK))goto done;
    if(schema==3U&&(!cs_json_u64(&json,"shortcuts",&shortcuts)||shortcuts>CL_SHORTCUTS_MASK))goto done;
    uuid=cs_json_string(&json,"uuid");hash=cs_json_string(&json,"digest");
    if(uuid==NULL||strcmp(uuid,store->uuid)!=0||hash==NULL||!cs_hex_valid(hash,64U))goto done;
    parsed.main_tab=(unsigned int)tab;parsed.appearance=appearance;parsed.shortcuts=(uint32_t)shortcuts;
    cl_preference_digest(store->uuid,&parsed,(unsigned int)schema,expected);
    if(strcmp(expected,hash)!=0)goto done;
    *out=parsed;status=RECORD_OK;
 done:
    if(close(fd)!=0)status=RECORD_IO;
    return status;
}
bool cl_preference_load(const struct cs_store *store,struct cl_preference_snapshot *out,bool *present)
{
    struct cl_preference_snapshot current={0},previous={0};
    enum record_status a,b;
    if(!(cs_store_check_media(store)==CS_STORE_OK))return false;
    *present=false;
    a=read_record(store,CL_PREFERENCE_CURRENT,&current);
    b=read_record(store,CL_PREFERENCE_PREVIOUS,&previous);
    if(a==RECORD_OK){current.error=b==RECORD_BAD||b==RECORD_IO;*out=current;*present=true;return (cs_store_check_media(store)==CS_STORE_OK);}
    if(a==RECORD_IO)return false;
    if(b==RECORD_OK){previous.recovered=true;previous.error=true;*out=previous;*present=true;return (cs_store_check_media(store)==CS_STORE_OK);}
    return a==RECORD_MISSING&&b==RECORD_MISSING&&(cs_store_check_media(store)==CS_STORE_OK);
}
uint64_t cl_preferences_runtime(const struct cs_store *store, uint32_t *shortcuts)
{
    struct cl_preference_snapshot current = {0};
    bool present = false;
    bool loaded = cl_preference_load(store,&current,&present) && present;
    if (shortcuts != NULL) *shortcuts = loaded ? current.shortcuts : 0U;
    return loaded ? current.appearance : 0U;
}
uint64_t cl_preferences_settings(const struct cs_store *store)
{ return cl_preferences_runtime(store, NULL); }
bool cl_preferences_auto_resume(const struct cs_store *store)
{ return (cl_preferences_settings(store)&CL_DISABLE_AUTO_RESUME)==0U; }
