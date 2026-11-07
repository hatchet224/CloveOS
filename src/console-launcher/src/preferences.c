#include "sessiond/durability.h"
#include "preference_record.h"
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
struct cl_preferences {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *condition;
    char path[CS_PATH_MAX],uuid[65];
    struct cl_preference_snapshot snapshot;
    uint64_t issued;
    bool desired_grid,pending,stop,changed,qualified_fat_syncfs,test_root;
    unsigned int desired_tab, desired_fields;
    uint64_t desired_appearance, appearance_mask;
    uint32_t desired_shortcuts, shortcut_mask;
};
static bool media(const struct cs_store *store)
{return cs_store_check_media(store)==CS_STORE_OK;}
static bool directory_sync(const struct cs_store *store)
{
    return cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) == 0;
}
static bool write_record(const struct cs_store *store,const char *name,
                          const struct cl_preference_snapshot *s)
{
    char bytes[CL_PREFERENCE_RECORD_MAX],temp[96],hash[65];int fd=-1,n;size_t used=0U;
    bool renamed=false,ok=false;uint64_t ticket=SDL_GetPerformanceCounter();
    unsigned int schema=s->shortcuts!=0U?3U:(s->appearance==0U&&s->main_tab<5U?1U:2U);
    cl_preference_digest(store->uuid,s,schema,hash);
    if(schema==1U)
        n=snprintf(bytes,sizeof(bytes),"{\"schema\":1,\"uuid\":\"%s\",\"revision\":%" PRIu64 ",\"grid\":%s,\"main_tab\":%u,\"digest\":\"%s\"}\n",store->uuid,s->revision,s->grid?"true":"false",s->main_tab,hash);
    else if(schema==2U)
        n=snprintf(bytes,sizeof(bytes),"{\"schema\":2,\"uuid\":\"%s\",\"revision\":%" PRIu64 ",\"grid\":%s,\"main_tab\":%u,\"appearance\":%" PRIu64 ",\"digest\":\"%s\"}\n",store->uuid,s->revision,s->grid?"true":"false",s->main_tab,s->appearance,hash);
    else
        n=snprintf(bytes,sizeof(bytes),"{\"schema\":3,\"uuid\":\"%s\",\"revision\":%" PRIu64 ",\"grid\":%s,\"main_tab\":%u,\"appearance\":%" PRIu64 ",\"shortcuts\":%u,\"digest\":\"%s\"}\n",store->uuid,s->revision,s->grid?"true":"false",s->main_tab,s->appearance,(unsigned int)s->shortcuts,hash);
    if(n<0||(size_t)n>=sizeof(bytes)||!media(store))return false;
    for(unsigned int i=0U;i<8U;++i){n=snprintf(temp,sizeof(temp),".ui-preferences.%ld.%016" PRIx64 ".tmp",(long)getpid(),ticket+i);
        if(n<0||(size_t)n>=sizeof(temp))return false;
        fd=openat(store->data_fd,temp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0||errno!=EEXIST)break;}
    if(fd<0)return false;
    used=strlen(bytes);
    for(size_t i=0U;i<used;){ssize_t wrote=write(fd,bytes+i,used-i);
        if(wrote<0&&errno==EINTR)continue;
        if(wrote<=0)goto done;i+=(size_t)wrote;}
    if(fsync(fd)!=0)goto done;
    if(close(fd)!=0){fd=-1;goto done;}fd=-1;
    if(!directory_sync(store)||!media(store)||renameat(store->data_fd,temp,store->data_fd,name)!=0)goto done;
    renamed=true;ok=directory_sync(store)&&media(store);
 done:
    if(fd>=0)(void)close(fd);
    if(!renamed)(void)unlinkat(store->data_fd,temp,0);
    return ok;
}
static int lock_store(const struct cs_store *store)
{
    struct stat st;int fd;
    if(!media(store))return -1;
    fd=openat(store->data_fd,".ui-preferences.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if(fd<0)return -1;
    if(fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_nlink!=1U||st.st_dev!=store->device||
       st.st_uid!=geteuid()||(st.st_mode&0077U)!=0U||flock(fd,LOCK_EX|LOCK_NB)!=0){(void)close(fd);return -1;}
    return fd;
}
static bool commit(const struct cs_store *store,const struct cl_preference_snapshot *current,
                    struct cl_preference_snapshot *desired)
{
    struct cl_preference_snapshot fresh=*current;bool ok=false,present=false;int lock=lock_store(store);
    if(lock<0)return false;
    if(!cl_preference_load(store,&fresh,&present)||(!present&&current->revision!=0U)||fresh.error||fresh.revision!=current->revision||
       fresh.grid!=current->grid||fresh.main_tab!=current->main_tab||
       fresh.appearance!=current->appearance||fresh.shortcuts!=current->shortcuts||
       current->revision==UINT64_MAX)goto done;
    desired->revision=current->revision+1U;
    desired->has_record=true;
    if(!write_record(store,CL_PREFERENCE_PREVIOUS,current)||!write_record(store,CL_PREFERENCE_CURRENT,desired))goto done;
    ok=true;
 done:
    if(close(lock)!=0)ok=false;
    return ok;
}
static int worker_main(void *context)
{
    struct cl_preferences *w=context;struct cs_store store;struct cs_store_config config={0};
    struct cl_preference_snapshot current;bool healthy,present=false;
    (void)SDL_LockMutex(w->mutex);current=w->snapshot;SDL_UnlockMutex(w->mutex);
    config.card_path=w->path;config.card_uuid=w->uuid;config.max_state_bytes=1U;
    config.qualified_fat_syncfs=w->qualified_fat_syncfs;config.test_root=w->test_root;
    healthy=cs_store_open_reader(&store,&config)==CS_STORE_OK&&cl_preference_load(&store,&current,&present);
    current.ready=true;current.error=current.error||!healthy;current.has_record=present;
    (void)SDL_LockMutex(w->mutex);current.pending=w->pending;w->snapshot=current;w->changed=true;SDL_UnlockMutex(w->mutex);
    for(;;){struct cl_preference_snapshot desired;uint64_t request;
        (void)SDL_LockMutex(w->mutex);
        while(!w->stop&&!w->pending)(void)SDL_CondWait(w->condition,w->mutex);
        if(w->stop&&!w->pending){SDL_UnlockMutex(w->mutex);break;}
        desired=current;
        if((w->desired_fields&1U)!=0U)desired.grid=w->desired_grid;
        if((w->desired_fields&2U)!=0U)desired.main_tab=w->desired_tab;
        desired.appearance=(current.appearance&~w->appearance_mask)|
                           (w->desired_appearance&w->appearance_mask);
        desired.shortcuts=(uint32_t)((current.shortcuts&~w->shortcut_mask)|
                                    (w->desired_shortcuts&w->shortcut_mask));
        request=w->issued;
        w->pending=false;SDL_UnlockMutex(w->mutex);
        bool ok=!current.error&&healthy&&commit(&store,&current,&desired);
        if(ok)current=desired;else current.error=true;
        current.request_id=request;
        (void)SDL_LockMutex(w->mutex);
        if(current.error){w->pending=false;current.request_id=w->issued;}
        current.pending=w->pending;w->snapshot=current;w->changed=true;
        SDL_UnlockMutex(w->mutex);
    }
    healthy=healthy&&!current.error&&cs_store_sync(&store)==CS_STORE_OK;
    cs_store_close(&store);return healthy?0:1;
}
static struct cl_preferences *start(const char *path,const char *uuid,bool fat,bool grid,
                                     unsigned int tab,bool test_root)
{
    struct cl_preferences *w;
    if(path==NULL||uuid==NULL||strnlen(path,CS_PATH_MAX)>=CS_PATH_MAX||
       strnlen(uuid,65U)>=65U||tab>CL_MAX_MAIN_TAB)return NULL;
    w=calloc(1U,sizeof(*w));if(w==NULL)return NULL;
    strcpy(w->path,path);strcpy(w->uuid,uuid);w->qualified_fat_syncfs=fat;w->test_root=test_root;
    w->snapshot.grid=grid;w->snapshot.main_tab=tab;
    w->mutex=SDL_CreateMutex();w->condition=SDL_CreateCond();
    if(w->mutex==NULL||w->condition==NULL)goto fail;
    w->thread=SDL_CreateThread(worker_main,"ui-preferences",w);if(w->thread==NULL)goto fail;
    return w;
 fail:
    if(w->condition!=NULL)SDL_DestroyCond(w->condition);
    if(w->mutex!=NULL)SDL_DestroyMutex(w->mutex);
    free(w);return NULL;
}
struct cl_preferences *cl_preferences_start(const char *path,const char *uuid,bool fat,bool grid,unsigned int tab)
{return start(path,uuid,fat,grid,tab,false);}
#ifdef CL_PREFERENCES_TESTING
struct cl_preferences *cl_preferences_test_start(const char *path,const char *uuid,bool grid,unsigned int tab)
{return start(path,uuid,true,grid,tab,true);}
#endif
static bool submit(struct cl_preferences *w,bool grid,unsigned int tab,
                   uint64_t appearance_mask,uint64_t appearance,
                   uint32_t shortcut_mask,uint32_t shortcuts)
{
    bool ok;
    if(w==NULL||tab>CL_MAX_MAIN_TAB)return false;
    (void)SDL_LockMutex(w->mutex);
    ok=!w->stop&&w->snapshot.ready&&!w->snapshot.error&&w->issued!=UINT64_MAX;
    if(ok){
        w->desired_grid=grid;w->desired_tab=tab;w->desired_fields=3U;
        w->desired_appearance=(w->desired_appearance&~appearance_mask)|(appearance&appearance_mask);
        w->appearance_mask|=appearance_mask;
        w->desired_shortcuts=(uint32_t)((w->desired_shortcuts&~shortcut_mask)|(shortcuts&shortcut_mask));
        w->shortcut_mask|=shortcut_mask;
        ++w->issued;w->pending=true;w->snapshot.pending=true;w->changed=true;SDL_CondSignal(w->condition);
    }
    SDL_UnlockMutex(w->mutex);return ok;
}
bool cl_preferences_submit(struct cl_preferences *w,bool grid,unsigned int tab)
{return submit(w,grid,tab,0U,0U,0U,0U);}
bool cl_preferences_submit_display(struct cl_preferences *w,bool grid,unsigned int tab,
                                   uint64_t appearance)
{return (appearance&CL_BATTERY_PERCENTAGE_MASK)!=CL_BATTERY_PERCENTAGE_MASK &&
        submit(w,grid,tab,CL_PREFERENCE_MASK,appearance,0U,0U);}
bool cl_preferences_submit_ui(struct cl_preferences *w,bool grid,unsigned int tab,
                              uint64_t appearance,uint32_t shortcuts)
{return (appearance&CL_BATTERY_PERCENTAGE_MASK)!=CL_BATTERY_PERCENTAGE_MASK &&
        submit(w,grid,tab,CL_PREFERENCE_MASK,appearance,CL_SHORTCUTS_MASK,shortcuts);}
bool cl_preferences_take(struct cl_preferences *w,struct cl_preference_snapshot *snapshot)
{
    bool changed;
    if(w==NULL||snapshot==NULL)return false;
    (void)SDL_LockMutex(w->mutex);changed=w->changed;if(changed){*snapshot=w->snapshot;w->changed=false;}SDL_UnlockMutex(w->mutex);return changed;
}
bool cl_preferences_finish_ui(struct cl_preferences *w,bool have_grid,bool grid,
                                   bool have_tab,unsigned int tab,
                                   uint64_t appearance_mask,uint64_t appearance,
                                   uint32_t shortcut_mask,uint32_t shortcuts)
{
    int result=0;bool accepted=true;
    if(w==NULL)return !have_grid&&!have_tab&&appearance_mask==0U&&shortcut_mask==0U;
    (void)SDL_LockMutex(w->mutex);
    if(have_grid||have_tab||appearance_mask!=0U||shortcut_mask!=0U) {
        accepted=(!have_tab||tab<=CL_MAX_MAIN_TAB) &&
                 !w->stop && !w->snapshot.error && w->issued!=UINT64_MAX;
        if(accepted) {
            /* Keep any already accepted field if a partial final update races
             * it. Merge against the loaded durable pair on the worker. */
            if(have_grid){w->desired_grid=grid;w->desired_fields|=1U;}
            if(have_tab){w->desired_tab=tab;w->desired_fields|=2U;}
            w->desired_appearance=(w->desired_appearance&~appearance_mask)|(appearance&appearance_mask);
            w->appearance_mask|=appearance_mask;
            w->desired_shortcuts=(uint32_t)((w->desired_shortcuts&~shortcut_mask)|(shortcuts&shortcut_mask));
            w->shortcut_mask|=shortcut_mask;
            ++w->issued;
            w->pending=true;w->snapshot.pending=true;w->changed=true;
        }
    }
    w->stop=true;SDL_CondSignal(w->condition);SDL_UnlockMutex(w->mutex);
    SDL_WaitThread(w->thread,&result);SDL_DestroyCond(w->condition);SDL_DestroyMutex(w->mutex);free(w);
    return accepted&&result==0;
}
bool cl_preferences_finish_display(struct cl_preferences *w,bool have_grid,bool grid,
                                   bool have_tab,unsigned int tab,
                                   uint64_t appearance_mask,uint64_t appearance)
{return cl_preferences_finish_ui(w,have_grid,grid,have_tab,tab,appearance_mask,appearance,0U,0U);}
bool cl_preferences_finish_patch(struct cl_preferences *w,bool have_grid,bool grid,
                                 bool have_tab,unsigned int tab)
{return cl_preferences_finish_display(w,have_grid,grid,have_tab,tab,0U,0U);}
bool cl_preferences_finish_latest(struct cl_preferences *w,bool have_latest,bool grid,unsigned int tab)
{return cl_preferences_finish_patch(w,have_latest,grid,have_latest,tab);}
bool cl_preferences_stop(struct cl_preferences *w)
{return cl_preferences_finish_latest(w,false,false,0U);}
