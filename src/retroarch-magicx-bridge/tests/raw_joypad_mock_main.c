int main(void){
 struct retro_keybind binds[16],auto_binds[16];rarch_joypad_info_t info={0,auto_binds,0.5f};unsigned i;
 static const unsigned indices[]={13,16,14,15,1,0,3,2};
 static const unsigned ids[]={RETRO_DEVICE_ID_JOYPAD_UP,RETRO_DEVICE_ID_JOYPAD_DOWN,RETRO_DEVICE_ID_JOYPAD_LEFT,RETRO_DEVICE_ID_JOYPAD_RIGHT,RETRO_DEVICE_ID_JOYPAD_A,RETRO_DEVICE_ID_JOYPAD_B,RETRO_DEVICE_ID_JOYPAD_X,RETRO_DEVICE_ID_JOYPAD_Y};
 managed=true;sdl_pad_connect(0);assert(raw_opens==1&&controller_opens==0);assert(sdl_pads[0].controller==NULL&&sdl_pads[0].num_buttons==22);assert(strcmp(sdl_joypad_name(0),"magicx raw22")==0);
 for(i=0;i<16;++i){binds[i].joykey=NO_BTN;binds[i].joyaxis=AXIS_NONE;auto_binds[i]=binds[i];}
 for(i=0;i<8;++i)binds[ids[i]].joykey=indices[i];
 for(i=0;i<8;++i){memset(raw_buttons,0,sizeof(raw_buttons));raw_buttons[indices[i]]=1;assert(sdl_joypad_button(0,(uint16_t)indices[i])==1);assert((uint16_t)sdl_joypad_state(&info,binds,0)==(uint16_t)(1U<<ids[i]));}
 memset(raw_buttons,0,sizeof(raw_buttons));raw_buttons[21]=1;assert(sdl_joypad_button(0,21)==1);assert(controller_reads==0&&raw_reads>0);
 for(i=0;i<4;++i){
  raw_axes[i]=21000;assert(sdl_joypad_axis_state(&sdl_pads[0],0,0xffff0000U|i)==21000);
  assert(sdl_joypad_axis_state(&sdl_pads[0],0,0xffffU|(i<<16))==0);
  raw_axes[i]=-22000;assert(sdl_joypad_axis_state(&sdl_pads[0],0,0xffffU|(i<<16))==-22000);
  assert(sdl_joypad_axis_state(&sdl_pads[0],0,0xffff0000U|i)==0);
  raw_axes[i]=0;
 }
 memset(sdl_pads,0,sizeof(sdl_pads));managed=false;sdl_pad_connect(0);assert(controller_opens==1);assert(sdl_pads[0].controller!=NULL);assert(sdl_pads[0].num_buttons==SDL_CONTROLLER_BUTTON_MAX);assert(strcmp(sdl_joypad_name(0),"semantic controller")==0);
 controller_buttons[SDL_CONTROLLER_BUTTON_DPAD_UP]=1;assert(sdl_joypad_button(0,SDL_CONTROLLER_BUTTON_DPAD_UP)==1);assert(sdl_joypad_button(0,21)==0);assert(controller_reads>0);
 puts("RAW_JOYPAD_PASS actual_sdl_pad_connect_and_read_state=1 managed_raw22=1 Dpad13_16_14_15=1 MENU21=1 paired_face_swap=1 unmanaged_GameController_unchanged=1");return 0;
}
