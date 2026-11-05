static void reset_host(void) {
 device_count=1;matching_index=0;buttons=22;hats=0;init_fails=open_fails=all_match=0;
 axes=4;
 initialized=0;init_calls=quit_calls=close_calls=0;
 memset(&settings,0x55,sizeof(settings));memset(input_config_binds,0x77,sizeof(input_config_binds));
}
static void publish(void) {
 struct cl_input_verification v={0};
 strcpy(v.baseline.guid,expected_guid);v.baseline.hat=-1;
 v.baseline.button[CL_UP]=13;v.baseline.button[CL_DOWN]=16;v.baseline.button[CL_LEFT]=14;v.baseline.button[CL_RIGHT]=15;
 v.baseline.button[CL_A]=0;v.baseline.button[CL_B]=1;v.baseline.button[CL_X]=2;v.baseline.button[CL_Y]=3;
 v.baseline.button[CL_L1]=4;v.baseline.button[CL_R1]=5;v.baseline.button[CL_MENU]=21;v.baseline.button[CL_VOL_DOWN]=19;v.baseline.button[CL_VOL_UP]=20;v.baseline.button[CL_START]=11;v.baseline.button[CL_SELECT]=10;
 v.checked=v.baseline;v.checked.button[CL_A]=1;v.checked.button[CL_B]=0;v.checked.button[CL_X]=3;v.checked.button[CL_Y]=2;
 assert(cl_input_runtime_publish(&v));
}
static void rejected(void) {
 struct host_settings before=settings;struct host_bind binds[1][64];char error[MAGICX_ERROR_MAX]="";
 memcpy(binds,input_config_binds,sizeof(binds));assert(!apply_verified_input(error));assert(error[0]);
 assert(memcmp(&settings,&before,sizeof(before))==0);assert(memcmp(input_config_binds,binds,sizeof(binds))==0);
}
static void edit(const char *needle,const char *replacement) {
 char bytes[4097],changed[4097];char *p;ssize_t n;int fd=open("/run/console/config/verified-input.json",O_RDONLY|O_CLOEXEC);
 assert(fd>=0);n=read(fd,bytes,sizeof(bytes)-1);assert(n>0);bytes[n]=0;assert(close(fd)==0);p=strstr(bytes,needle);assert(p);
 assert(snprintf(changed,sizeof(changed),"%.*s%s%s",(int)(p-bytes),bytes,replacement,p+strlen(needle))<(int)sizeof(changed));
 fd=open("/run/console/config/verified-input.json",O_WRONLY|O_TRUNC|O_CLOEXEC);assert(fd>=0);assert(write(fd,changed,strlen(changed))==(ssize_t)strlen(changed));assert(fsync(fd)==0);assert(close(fd)==0);
}
int main(void) {
 char error[MAGICX_ERROR_MAX]="";int fd;
 assert(mkdir("/run",0700)==0||errno==EEXIST);assert(mkdir("/run/console",0700)==0||errno==EEXIST);assert(mkdir("/run/console/config",0700)==0||errno==EEXIST);
 reset_host();assert(unsetenv("MAGICX_INPUT_PROFILE")==0);assert(apply_verified_input(error));assert(init_calls==0);
 assert(setenv("MAGICX_INPUT_PROFILE","/run/console/config/verified-input.json",1)==0);
 assert(unlink("/run/console/config/verified-input.json")==0||errno==ENOENT);rejected();
 publish();assert(apply_verified_input(error));assert(settings.uints.input_joypad_index[0]==0);
 assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_A].joykey==1);assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_B].joykey==0);
 assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_X].joykey==3);assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_Y].joykey==2);
 assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_UP].joykey==13);assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_DOWN].joykey==16);
 assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_START].joykey==11);
 assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_SELECT].joykey==10);
 assert(input_config_binds[0][RARCH_SCREENSHOT].joykey==10);
 assert(input_config_binds[0][RARCH_QUIT_KEY].joykey==11);
 assert(input_config_binds[0][RARCH_ENABLE_HOTKEY].joykey==21);assert(input_config_binds[0][RARCH_ENABLE_HOTKEY].joyaxis==AXIS_NONE);
 assert(input_config_binds[0][RARCH_FAST_FORWARD_KEY].joykey==5);assert(input_config_binds[0][RARCH_SLOWMOTION_KEY].joykey==4);
 assert(input_config_binds[0][RARCH_STATE_SLOT_MINUS].joykey==14);assert(input_config_binds[0][RARCH_STATE_SLOT_PLUS].joykey==15);
 for(unsigned axis=0;axis<4;++axis){unsigned id=RARCH_ANALOG_LEFT_X_PLUS+axis*2U;
  assert(input_config_binds[0][id].joyaxis==AXIS_POS(axis));
  assert(input_config_binds[0][id+1U].joyaxis==AXIS_NEG(axis));
  assert(input_config_binds[0][id].joykey==NO_BTN&&input_config_binds[0][id+1U].joykey==NO_BTN);}
 assert(init_calls==1&&quit_calls==1&&close_calls==1);
 reset_host();initialized=SDL_INIT_JOYSTICK;assert(apply_verified_input(error));
 assert(init_calls==0&&quit_calls==0&&close_calls==1); /* Retain RA-owned SDL lifetime. */
 reset_host();axes=2;assert(apply_verified_input(error));
 assert(input_config_binds[0][RARCH_ANALOG_LEFT_X_PLUS+4U].joyaxis==AXIS_NONE);
 reset_host();axes=0;assert(apply_verified_input(error));
 assert(input_config_binds[0][RARCH_ANALOG_LEFT_X_PLUS].joyaxis==AXIS_NONE);
 reset_host();axes=-1;rejected();
 reset_host();publish();edit("\"schema\":2","\"schema\":1");edit(",\"start\":11,\"select\":10","");
 assert(apply_verified_input(error));
 assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_START].joykey==0x7777);
 assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_SELECT].joykey==0x7777);
 publish();
 reset_host();device_count=2;matching_index=1;assert(apply_verified_input(error));assert(settings.uints.input_joypad_index[0]==1);
 reset_host();device_count=0;rejected();reset_host();matching_index=9;rejected();reset_host();device_count=2;all_match=1;rejected();
 reset_host();init_fails=1;rejected();reset_host();open_fails=1;rejected();reset_host();buttons=21;rejected();
 reset_host();publish();edit("\"hat\":65535","\"hat\":0");rejected();hats=1;assert(apply_verified_input(error));assert(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_UP].joykey==HAT_MAP(0,HAT_UP_MASK));
 reset_host();publish();edit("\"start\":11","\"start\":22");rejected();
 publish();edit("\"select\":10","\"select\":21");rejected();
 publish();edit("\"menu\":21","\"menu\":22");rejected();
 publish();edit("\"menu\":21","\"menu\":65535");rejected();
 publish();edit("\"a\":1","\"a\":65535");rejected();
 publish();edit("\"up\":13","\"up\":65535");rejected();
 publish();edit("\"b\":0","\"b\":1");rejected();
 publish();edit("\"schema\":2","\"schema\":2,\"schema\":2");rejected();
 publish();edit("\"coverage\":\"navigation-8\"","\"coverage\":\"all-buttons\"");rejected();
 publish();assert(chmod("/run/console/config/verified-input.json",0644)==0);rejected();
 publish();fd=open("/run/console/config/verified-input.json",O_RDONLY|O_CLOEXEC);assert(fd>=0);assert(fchown(fd,1,0)==0);assert(close(fd)==0);rejected();
 publish();assert(link("/run/console/config/verified-input.json","/run/console/config/hard.json")==0);rejected();assert(unlink("/run/console/config/hard.json")==0);
 publish();assert(rename("/run/console/config/verified-input.json","/run/console/config/real.json")==0);assert(symlink("/run/console/config/real.json","/run/console/config/verified-input.json")==0);rejected();assert(unlink("/run/console/config/verified-input.json")==0);assert(unlink("/run/console/config/real.json")==0);
 publish();assert(setenv("MAGICX_INPUT_PROFILE","/tmp/untrusted-input.json",1)==0);rejected();
 puts("VERIFIED_INPUT_COMPONENT_PASS actual_function=1 production_serializer=1 protected_files=1 paired_face_swap=1 MENU21=1 schema2_start_select=1 legacy_schema1=1 preexisting_sdl_retained=1 index1=1 hats=1 rejected_missing_changed_duplicate_disabled_bounds_modes_links=1");return 0;
}
