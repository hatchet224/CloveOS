/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit Tweaks edits update only the corresponding native preference.
 * Native settings are never re-applied from this record during launch. */
#ifndef CLOVE_NATIVE_PREFERENCES_H
#define CLOVE_NATIVE_PREFERENCES_H
struct cl_native_value { char key[64],value[64]; };
static bool cl_native_line_key(const char *line,const char *key)
{
 while(*line==' '||*line=='\t')++line;size_t n=strlen(key);
 if(strncmp(line,key,n))return false;line+=n;while(*line==' '||*line=='\t')++line;return *line=='=';
}
static bool cl_native_preferences_sync(const struct cs_store *store,
 const struct cl_preference_snapshot *before,const struct cl_preference_snapshot *after)
{
 uint32_t delta=before->shortcuts^after->shortcuts;
 bool swap=(delta&(UINT32_C(1)<<CL_TRIGGER_SWAP_SHIFT))!=0U;
 bool rate=(delta&(UINT32_C(63)<<CL_FAST_FORWARD_SHIFT))!=0U;
 bool hints=((before->appearance^after->appearance)&CL_HIDE_HINT_LABELS)!=0U;
 if(!swap&&!rate&&!hints)return true;
 int dir=openat(store->data_fd,"retroarch",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
 if(dir<0)return errno==ENOENT;
 int fd=openat(dir,"retroarch.cfg",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
 if(fd<0){bool absent=errno==ENOENT;close(dir);return absent;}
 struct stat st;if(fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_size>2*1024*1024){close(fd);close(dir);return false;}
 FILE *input=fdopen(fd,"r");if(!input){close(fd);close(dir);return false;}
 struct cl_native_value values[24]={0};size_t count=0;
 if(rate){strcpy(values[count].key,"fastforward_ratio");snprintf(values[count++].value,64,"%u",cl_setting_value(after->shortcuts,CL_FAST_FORWARD_SHIFT,6U));}
 if(hints){const char *v=(after->appearance&CL_HIDE_HINT_LABELS)?"false":"true";
  strcpy(values[count].key,"menu_show_sublabels");strcpy(values[count++].value,v);
  strcpy(values[count].key,"menu_core_enable");strcpy(values[count++].value,v);}
 size_t swap_start=count;
 if(swap)for(unsigned port=1;port<=2;port++)for(unsigned side=0;side<2;side++)for(unsigned field=0;field<2;field++)for(unsigned trigger=0;trigger<2;trigger++){
  struct cl_native_value *v=&values[count++];snprintf(v->key,64,"input_player%u_%c%s_%s",port,side?'r':'l',trigger?"2":"",field?"axis":"btn");
  if(field)strcpy(v->value,"nul");else snprintf(v->value,64,"%u",4U+side+trigger*2U);
 }
 char *line=NULL;size_t capacity=0;ssize_t length;
 while((length=getline(&line,&capacity,input))>=0){
  for(size_t i=swap_start;i<count;i++)if(cl_native_line_key(line,values[i].key)){
   const char *quote=strchr(line,'"');if(quote){const char *end=strchr(++quote,'"');if(end&&(size_t)(end-quote)<64U){memcpy(values[i].value,quote,(size_t)(end-quote));values[i].value[end-quote]=0;}}
  }
 }
 bool ok=!ferror(input);rewind(input);
 if(swap)for(size_t i=swap_start;i<count;i+=2){char value[64];strcpy(value,values[i].value);strcpy(values[i].value,values[i+1].value);strcpy(values[i+1].value,value);}
 char temporary[96];snprintf(temporary,sizeof(temporary),".tweaks.%ld.%016" PRIx64,(long)getpid(),(uint64_t)SDL_GetPerformanceCounter());
 int outfd=ok?openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600):-1;
 FILE *output=outfd>=0?fdopen(outfd,"w"):NULL;if(!output){if(outfd>=0)close(outfd);ok=false;}
 while(ok&&(length=getline(&line,&capacity,input))>=0){
  bool replace=false;for(size_t i=0;i<count;i++)if(cl_native_line_key(line,values[i].key)){replace=true;break;}
  if(!replace && fwrite(line,1,(size_t)length,output)!=(size_t)length)ok=false;
 }
 if(ferror(input))ok=false;
 if(output){
  if(ok && fputc('\n',output)==EOF)ok=false;
  for(size_t i=0;ok&&i<count;i++)if(fprintf(output,"%s = \"%s\"\n",values[i].key,values[i].value)<0)ok=false;
  if(fflush(output)!=0||fsync(fileno(output))!=0)ok=false;
  if(fclose(output)!=0)ok=false;
 }
 free(line);if(fclose(input)!=0)ok=false;
 if(ok)ok=media(store)&&renameat(dir,temporary,dir,"retroarch.cfg")==0;
 if(ok)ok=cs_flush_directory(dir,store->fat,store->qualified_fat_syncfs)==0&&media(store);
 if(!ok)unlinkat(dir,temporary,0);
 close(dir);return ok;
}
#endif
