
static void observe(unsigned recovery,unsigned available) {
 ra.feedback.observed=(struct cs_session_status){.active=1U,.session=9U,.generation=7U,.recovery=recovery,.available=available,.failure=1U,.sequence=20U};
 ra.feedback.known=true;ra.feedback.phase=MX_FEEDBACK_IDLE;
 recovery_status();
}
static void take_request(int listener,unsigned expected) {
 struct cs_control_header frame;int fd=accept(listener,NULL,NULL);assert(fd>=0);
 assert(magicx_controls_poll(&ra.controls,1001U)==0);
 assert(recv(fd,&frame,sizeof(frame),0)==sizeof(frame));
 assert(frame.type==expected && frame.length==0U && frame.token==ra.feedback.token);
 {unsigned char bytes[28];int32_t result=0;frame.type=CS_CONTROL_ACK;frame.length=4;
 memcpy(bytes,&frame,24);memcpy(bytes+24,&result,4);assert(send(fd,bytes,28,MSG_NOSIGNAL)==28);}
 assert(magicx_controls_poll(&ra.controls,1002U)==1);
 assert(ra.feedback.phase==MX_FEEDBACK_QUEUED); /* ACK never means completed. */
 assert(close(fd)==0);
}
int main(void) {
 char directory[]="/tmp/magicx-recovery-XXXXXX";
 struct sockaddr_un address={0};int listener;
 assert(mkdtemp(directory)!=NULL);
 assert(snprintf(socket_path,sizeof(socket_path),"%s/control.sock",directory)>0);
 listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(listener>=0);
 address.sun_family=AF_UNIX;strcpy(address.sun_path,socket_path);
 assert(bind(listener,(struct sockaddr*)&address,sizeof(address))==0&&chmod(socket_path,0600)==0&&listen(listener,8)==0);
 magicx_controls_init(&ra.controls);
 ra.managed=true;paused=true;menu.driver_data=&menu_data;strcpy(settings.arrays.menu_driver,"rgui");
 /* Missing-media failure status alone opens the local panel. It needs no
  * HELLO, profile reload, or mounted card to render and preserve pause. */
 observe(1U,0U);assert(ra.recovery_panel&&paused&&menu.selection_ptr==0U);
 assert(strstr(magicx_retroarch_menu_label(0),"Cancel")!=NULL);
 assert(strstr(magicx_retroarch_menu_label(2),"unavailable")!=NULL);
 magicx_retroarch_menu_action(MENU_ACTION_OK,2);assert(ra.controls.fd<0&&paused);
 magicx_retroarch_menu_action(MENU_ACTION_OK,3);assert(ra.recovery_confirm==0U&&ra.controls.fd<0);
 magicx_retroarch_menu_action(MENU_ACTION_CANCEL,0);assert(ra.recovery_panel&&paused);
 /* Confirmation selects Cancel by default, and B cannot escape or power off. */
 observe(2U,CS_RECOVERY_RETRY|CS_RECOVERY_OFF|CS_RECOVERY_MENU);
 magicx_retroarch_menu_action(MENU_ACTION_OK,3);assert(ra.recovery_confirm==1U&&menu.selection_ptr==0U&&ra.controls.fd<0);
 assert(strstr(magicx_retroarch_menu_label(1),"CONFIRM power off")!=NULL);
 magicx_retroarch_menu_action(MENU_ACTION_OK,0);assert(!ra.recovery_confirm&&ra.controls.fd<0&&paused);
 magicx_retroarch_menu_action(MENU_ACTION_OK,3);magicx_retroarch_menu_action(MENU_ACTION_CANCEL,1);
 assert(!ra.recovery_confirm&&ra.controls.fd<0&&ra.recovery_panel);
 magicx_retroarch_menu_action(MENU_ACTION_OK,3);magicx_retroarch_menu_action(MENU_ACTION_OK,1);
 assert(!ra.recovery_panel&&paused&&ra.feedback.phase==MX_FEEDBACK_QUEUED);
 take_request(listener,CS_CONTROL_RECOVERY_OFF);
 observe(2U,CS_RECOVERY_RETRY|CS_RECOVERY_MENU);
 magicx_retroarch_menu_action(MENU_ACTION_OK,4);assert(ra.recovery_confirm==2U&&menu.selection_ptr==0U&&ra.controls.fd<0);
 magicx_retroarch_menu_action(MENU_ACTION_OK,1);assert(!ra.recovery_panel&&paused);
 take_request(listener,CS_CONTROL_RECOVERY_MENU);
 /* Retry failure is correlated; it restores the panel instead of releasing
  * a possibly tainted core. Continue remains disabled by available flags. */
 observe(1U,CS_RECOVERY_RETRY);
 magicx_retroarch_menu_action(MENU_ACTION_OK,1);take_request(listener,CS_CONTROL_RECOVERY_RETRY);
 {struct cs_session_status o=ra.feedback.observed;o.operation=CS_CONTROL_RECOVERY_RETRY;o.operation_token=ra.feedback.token;o.sequence++;
 assert(magicx_feedback_observe(&ra.feedback,&o)&&ra.feedback.phase==MX_FEEDBACK_FAILED);}
 recovery_status();assert(ra.recovery_panel&&paused);
 magicx_retroarch_menu_action(MENU_ACTION_OK,2);assert(ra.controls.fd<0);
 /* Hiding cannot alter a successful native RELEASE that already unpaused. */
 paused=false;ra.feedback.observed.recovery=0U;recovery_status();assert(!ra.recovery_panel&&!paused);
 assert(close(listener)==0&&unlink(socket_path)==0&&rmdir(directory)==0);
 puts("RECOVERY_PANEL_PASS exact native functions, cached missing-card status, preserved pause, writer/partial-load disabled actions, default Cancel, second confirmation, correlated failure, no ACK success");
 return 0;
}
