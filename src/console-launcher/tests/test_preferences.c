#ifdef NDEBUG
#error "Preferences fixture requires active assertions"
#endif
#include "launcher/preferences.h"
#include "launcher/i18n.h"
#include "sessiond/json.h"
#include "sessiond/store.h"
#include <SDL.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
static const char *const uuid="C0280002";
static void join(char out[1024],const char *root,const char *name)
{int n=snprintf(out,1024,"%s/%s",root,name);assert(n>0&&n<1024);}
static void card(char out[1024],const char *root,const char *name)
{char dir[1024];join(out,root,name);assert(mkdir(out,0700)==0);join(dir,out,".console");assert(mkdir(dir,0700)==0);}
static struct cl_preference_snapshot wait_snapshot(struct cl_preferences *w,uint64_t request)
{
 struct cl_preference_snapshot s={0};for(unsigned int i=0;i<5000;++i){(void)cl_preferences_take(w,&s);
  if(s.ready&&!s.pending&&s.request_id>=request)return s;SDL_Delay(2);}
 assert(0 && "preferences timeout");return s;
}
static void write_text(const char *root,const char *name,const char *text)
{char path[1024];join(path,root,name);int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);assert(fd>=0);assert(write(fd,text,strlen(text))==(ssize_t)strlen(text));assert(fsync(fd)==0&&close(fd)==0);}
static bool has_file(const char *root,const char *name)
{char p[1024];join(p,root,name);return lstat(p,&(struct stat){0})==0;}
int main(int argc,char **argv)
{
 char path[1024],directory[1024],other[1024],record[1024];struct cl_preferences *w,*second;struct cl_preference_snapshot s,before;struct statfs fs;
 assert(argc==2&&SDL_Init(0)==0);assert(mkdir(argv[1],0700)==0);assert(statfs(argv[1],&fs)==0);
 card(path,argv[1],"early-stop");w=cl_preferences_test_start(path,uuid,false,0);assert(w);
 assert(cl_preferences_finish_latest(w,true,true,3));
 w=cl_preferences_test_start(path,uuid,false,0);s=wait_snapshot(w,0);assert(!s.error&&s.grid&&s.main_tab==3&&s.revision==1&&cl_preferences_stop(w));
 /* A quick main-tab edit must preserve the saved grid choice before load. */
 card(path,argv[1],"partial");w=cl_preferences_test_start(path,uuid,false,0);assert(w);
 assert(cl_preferences_finish_latest(w,true,false,2));
 w=cl_preferences_test_start(path,uuid,true,0);assert(w);
 assert(cl_preferences_finish_patch(w,false,true,true,3));
 w=cl_preferences_test_start(path,uuid,true,0);s=wait_snapshot(w,0);
 assert(!s.error&&!s.grid&&s.main_tab==3&&cl_preferences_stop(w));
 w=cl_preferences_test_start(path,uuid,true,0);assert(w);
 assert(cl_preferences_finish_patch(w,true,true,false,0));
 w=cl_preferences_test_start(path,uuid,false,0);s=wait_snapshot(w,0);
 assert(!s.error&&s.grid&&s.main_tab==3&&cl_preferences_stop(w));
 card(path,argv[1],"normal");w=cl_preferences_test_start(path,uuid,false,0);assert(w);s=wait_snapshot(w,0);assert(!s.error&&!s.grid&&s.main_tab==0&&s.revision==0);
 assert(cl_preferences_submit(w,true,3));assert(cl_preferences_stop(w)); /* immediate exit must drain */
 w=cl_preferences_test_start(path,uuid,false,0);assert(w);s=wait_snapshot(w,0);assert(!s.error&&s.grid&&s.main_tab==3&&s.revision==1);
 assert(!cl_preferences_submit(w,false,6));uint64_t base=s.revision;
 for(unsigned int i=0;i<200;++i)assert(cl_preferences_submit(w,(i%2U)!=0U,i%4U));
 s=wait_snapshot(w,200);assert(!s.error&&s.grid&&s.main_tab==3&&s.revision>base&&s.revision<base+200);
 assert(cl_preferences_stop(w));printf("PREFERENCES_COALESCED accepted=200 committed=%llu\n",(unsigned long long)(s.revision-base));
 /* Frozen CAS prevents two UIs from overwriting a newer durable selection. */
 w=cl_preferences_test_start(path,uuid,false,0);second=cl_preferences_test_start(path,uuid,false,0);assert(w&&second);
 before=wait_snapshot(second,0);(void)wait_snapshot(w,0);assert(cl_preferences_submit(w,false,1));s=wait_snapshot(w,1);assert(!s.error&&!s.grid&&s.main_tab==1);
 assert(cl_preferences_submit(second,true,2));s=wait_snapshot(second,1);assert(s.error&&s.grid==before.grid&&s.main_tab==before.main_tab&&s.revision==before.revision);
 assert(!cl_preferences_stop(second));assert(cl_preferences_stop(w));
 /* Corrupt current recovers previous visibly, blocks implicit repair. */
 join(directory,path,".console");join(record,directory,"ui-preferences.previous.json");int fd=open(record,O_RDONLY|O_CLOEXEC);char bytes[385];assert(fd>=0);ssize_t n=read(fd,bytes,sizeof(bytes));assert(n>0&&close(fd)==0);struct cs_json parsed;bool prior_grid;uint64_t prior_tab,prior_revision;
 assert(cs_json_parse(&parsed,bytes,(size_t)n)&&cs_json_bool(&parsed,"grid",&prior_grid)&&cs_json_u64(&parsed,"main_tab",&prior_tab)&&cs_json_u64(&parsed,"revision",&prior_revision));
 write_text(directory,"ui-preferences.json","broken-current");w=cl_preferences_test_start(path,uuid,false,0);s=wait_snapshot(w,0);assert(s.ready&&s.error&&s.recovered&&s.grid==prior_grid&&s.main_tab==prior_tab&&s.revision==prior_revision);assert(!cl_preferences_submit(w,false,0)&&!cl_preferences_stop(w));
 join(record,directory,"ui-preferences.json");fd=open(record,O_RDONLY|O_CLOEXEC);assert(fd>=0);char current_bad[32];assert(read(fd,current_bad,sizeof(current_bad))==14&&memcmp(current_bad,"broken-current",14)==0&&close(fd)==0);
 join(record,directory,"ui-preferences.previous.json");fd=openat(AT_FDCWD,record,O_RDONLY|O_CLOEXEC);assert(fd>=0);char unchanged[385];assert(read(fd,unchanged,sizeof(unchanged))==n&&memcmp(bytes,unchanged,(size_t)n)==0&&close(fd)==0);
 /* No previous fallback and malformed checksum both fail closed. */
 card(path,argv[1],"corrupt");join(directory,path,".console");write_text(directory,"ui-preferences.json","{\"schema\":1,\"grid\":true}");w=cl_preferences_test_start(path,uuid,false,0);s=wait_snapshot(w,0);assert(s.error&&!s.recovered&&!s.grid&&!cl_preferences_submit(w,true,1)&&!cl_preferences_stop(w));
 /* A correctly shaped record with a modified canonical digest is rejected. */
 card(path,argv[1],"checksum");join(directory,path,".console");assert(n>0&&(size_t)n<sizeof(bytes));bytes[n]='\0';char *checksum=strstr(bytes,"\"digest\":\"");assert(checksum);checksum+=10;*checksum=*checksum=='a'?'b':'a';write_text(directory,"ui-preferences.json",bytes);
 w=cl_preferences_test_start(path,uuid,false,0);s=wait_snapshot(w,0);assert(s.error&&!s.recovered&&!cl_preferences_submit(w,true,1)&&!cl_preferences_stop(w));
 /* Missing card / metadata directory must never be created as a fallback. */
 join(path,argv[1],"missing");w=cl_preferences_test_start(path,uuid,false,0);assert(!cl_preferences_finish_latest(w,true,true,2)&&!has_file(argv[1],"missing"));
 join(path,argv[1],"empty-card");assert(mkdir(path,0700)==0);w=cl_preferences_test_start(path,uuid,false,0);assert(wait_snapshot(w,0).error&&!cl_preferences_stop(w)&&!has_file(path,".console"));
 /* Replacing a live mountpoint path must not redirect writes to its replacement. */
 card(path,argv[1],"media");w=cl_preferences_test_start(path,uuid,false,0);(void)wait_snapshot(w,0);assert(cl_preferences_submit(w,true,2));before=wait_snapshot(w,1);assert(!before.error);
 join(other,argv[1],"media-removed");assert(rename(path,other)==0&&mkdir(path,0700)==0);join(directory,path,".console");assert(mkdir(directory,0700)==0);
 assert(cl_preferences_submit(w,false,3));s=wait_snapshot(w,2);assert(s.error&&s.grid==before.grid&&s.main_tab==before.main_tab&&!has_file(directory,"ui-preferences.json")&&!cl_preferences_stop(w));
 if(fs.f_type!=0x4d44){
  char safe[1024],safe_metadata[1024],safe_record[1024];
  card(safe,argv[1],"safe-card");w=cl_preferences_test_start(safe,uuid,false,0);(void)wait_snapshot(w,0);assert(cl_preferences_submit(w,true,2)&&cl_preferences_stop(w));
  join(safe_metadata,safe,".console");join(safe_record,safe_metadata,"ui-preferences.json");
  card(path,argv[1],"symlink-record");join(directory,path,".console");join(record,directory,"ui-preferences.json");assert(symlink(safe_record,record)==0);
  w=cl_preferences_test_start(path,uuid,false,0);assert(wait_snapshot(w,0).error&&!cl_preferences_submit(w,true,2)&&!cl_preferences_stop(w));
  join(path,argv[1],"symlink-card");assert(symlink(safe,path)==0);w=cl_preferences_test_start(path,uuid,false,0);assert(wait_snapshot(w,0).error&&!cl_preferences_stop(w));
  join(path,argv[1],"symlink-metadata");assert(mkdir(path,0700)==0);join(record,path,".console");assert(symlink(safe_metadata,record)==0);
  w=cl_preferences_test_start(path,uuid,false,0);assert(wait_snapshot(w,0).error&&!cl_preferences_stop(w));
  card(path,argv[1],"hardlink-record");join(directory,path,".console");join(record,directory,"ui-preferences.json");assert(link(safe_record,record)==0);
  w=cl_preferences_test_start(path,uuid,false,0);assert(wait_snapshot(w,0).error&&!cl_preferences_stop(w));assert(unlink(record)==0);
  w=cl_preferences_test_start(safe,uuid,false,0);s=wait_snapshot(w,0);assert(!s.error&&s.grid&&s.main_tab==2&&s.revision==1&&cl_preferences_stop(w));
 }

 card(path,argv[1],"appearance");
 w=cl_preferences_test_start(path,uuid,false,1U);assert(w);
 assert(cl_preferences_finish_latest(w,true,true,4U));
 w=cl_preferences_test_start(path,uuid,false,1U);assert(w);
 assert(cl_preferences_finish_display(w,false,false,false,0U,CL_HIDE_RECENT,CL_HIDE_RECENT));
 w=cl_preferences_test_start(path,uuid,false,1U);assert(w);
 s=wait_snapshot(w,0);
 assert(!s.error&&s.grid&&s.main_tab==4U&&s.appearance==CL_HIDE_RECENT&&s.revision==2U);
 assert(cl_preferences_submit_display(w,true,1U,CL_HIDE_RECENT|CL_HIDE_HINT_LABELS));
 assert(cl_preferences_stop(w));
 w=cl_preferences_test_start(path,uuid,false,0U);assert(w);
 s=wait_snapshot(w,0);
 assert(!s.error&&s.grid&&s.main_tab==1U&&s.appearance==(CL_HIDE_RECENT|CL_HIDE_HINT_LABELS));
 assert(cl_preferences_submit_display(w,true,1U,s.appearance|CL_DISABLE_AUTO_RESUME|CL_APPS_REVERSE));
 assert(cl_preferences_stop(w));
 struct cs_store store;
 struct cs_store_config config={.card_path=path,.card_uuid=uuid,.max_state_bytes=1U,.test_root=true,.qualified_fat_syncfs=true};
 assert(cs_store_open_reader(&store,&config)==CS_STORE_OK);
 assert(!cl_preferences_auto_resume(&store));
 assert((cl_preferences_settings(&store)&CL_APPS_REVERSE)!=0U);
 cs_store_close(&store);
 w=cl_preferences_test_start(path,uuid,false,0U);assert(w);
 s=wait_snapshot(w,0);assert(!s.error&&s.shortcuts==0U);
 assert(cl_preferences_submit_ui(w,s.grid,s.main_tab,s.appearance,0x1002U));
 assert(cl_preferences_stop(w));
 w=cl_preferences_test_start(path,uuid,false,0U);assert(w);
 assert(cl_preferences_finish_ui(w,false,false,false,0U,0U,0U,0x00ffU,0x0003U));
 w=cl_preferences_test_start(path,uuid,false,0U);assert(w);
 s=wait_snapshot(w,0);
 assert(!s.error&&s.shortcuts==0x1003U&&(s.appearance&CL_APPS_REVERSE)!=0U);
 uint32_t game_controls=(UINT32_C(1)<<CL_TRIGGER_SWAP_SHIFT)|(UINT32_C(50)<<CL_FAST_FORWARD_SHIFT);
 assert(cl_preferences_submit_ui(w,s.grid,s.main_tab,s.appearance,s.shortcuts|game_controls));
 assert(cl_preferences_stop(w));
 assert(cs_store_open_reader(&store,&config)==CS_STORE_OK);
 uint32_t runtime=0U;
 assert((cl_preferences_runtime(&store,&runtime)&CL_APPS_REVERSE)!=0U);
 assert(runtime==(0x1003U|game_controls));
 cs_store_close(&store);
 /* Language persists independently of all game controls and existing flags. */
 w=cl_preferences_test_start(path,uuid,false,0U);assert(w);s=wait_snapshot(w,0);
 assert(cl_preferences_submit_ui(w,s.grid,s.main_tab,s.appearance,s.shortcuts|(UINT32_C(1)<<CL_LANGUAGE_SHIFT)));
 assert(cl_preferences_stop(w));
 w=cl_preferences_test_start(path,uuid,false,0U);assert(w);s=wait_snapshot(w,0);
 assert(!s.error&&s.shortcuts==(runtime|(UINT32_C(1)<<CL_LANGUAGE_SHIFT)));
 cl_language_set((s.shortcuts&CL_LANGUAGE_MASK)>>CL_LANGUAGE_SHIFT);
 assert(cl_language_get()==CL_SPANISH&&strcmp(CL_TR("Settings"),"Ajustes")==0);
 assert(cl_preferences_submit_ui(w,s.grid,s.main_tab,s.appearance,(s.shortcuts&~CL_LANGUAGE_MASK)|(UINT32_C(2)<<CL_LANGUAGE_SHIFT)));
 assert(cl_preferences_stop(w));
 w=cl_preferences_test_start(path,uuid,false,0U);assert(w);s=wait_snapshot(w,0);
 assert(!s.error&&s.shortcuts==(runtime|(UINT32_C(2)<<CL_LANGUAGE_SHIFT)));
 cl_language_set((s.shortcuts&CL_LANGUAGE_MASK)>>CL_LANGUAGE_SHIFT);
 assert(cl_language_get()==CL_ENGLISH&&strcmp(CL_TR("Settings"),"Settings")==0);
 assert(cl_preferences_stop(w));
 puts("R50_LANGUAGE_PERSIST_RESTART_RESERVED_FALLBACK_PASS");
 puts("IN_GAME_PREFERENCES_DURABLE_MERGE_PASS");
 puts("MAINUI_SHORTCUT_PREFERENCES_V2_MIGRATION_AND_MERGE_PASS");
 puts("APPEARANCE_PREFERENCES_V1_MIGRATION_AND_RELOAD_PASS");
 printf("PREFERENCES_PASS fs_magic=%lx durable_reload immediate_exit_drain coalescing CAS corrupt_recovery missing_media nofallback symlink_checks early_loading_final_latest\n",(unsigned long)fs.f_type);
 SDL_Quit();return 0;
}
