
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
 menu.driver_ctx=&native_driver;
 /* A checkpoint error opens full native RGUI with a dismissible notice. */
 observe(1U,0U);assert(ra.save_error&&ra.quick_menu&&!ra.recovery_panel&&paused);
 assert(ra.native_notice[0]&&magicx_retroarch_menu_finish_requested(false));
 assert(op==0&&ra.controls.fd<0);
 assert(native_notice_action(MENU_ACTION_CANCEL)&&!ra.native_notice[0]&&ra.quick_menu&&paused);
 assert(!native_notice_action(MENU_ACTION_DOWN)); /* Native navigation available. */
 /* Writer/reload policy remains authoritative; unavailable Resume never queues. */
 assert(magicx_retroarch_menu_finish_requested(false));
 assert(ra.controls.fd<0&&ra.native_notice[0]&&paused);
 assert(native_notice_action(MENU_ACTION_CANCEL));
 observe(1U,CS_RECOVERY_CONTINUE|CS_RECOVERY_MENU);
 assert(magicx_retroarch_menu_finish_requested(false));
 assert(!ra.quick_menu&&paused&&ra.feedback.phase==MX_FEEDBACK_QUEUED);
 take_request(listener,CS_CONTROL_RECOVERY_CONTINUE);
 paused=false;observe(0U,0U);assert(!ra.save_error&&!paused);
 /* Explicit exit uses native A/B confirmation. B retains the paused game. */
 observe(1U,CS_RECOVERY_MENU);assert(ra.quick_menu&&paused);
 assert(native_notice_action(MENU_ACTION_CANCEL));
 native_error_exit();assert(ra.native_exit_confirm==1U&&ra.native_notice[0]);
 assert(magicx_retroarch_menu_finish_requested(false)); /* No implicit resume. */
 assert(ra.controls.fd<0);
 assert(native_notice_action(MENU_ACTION_CANCEL)&&!ra.native_exit_confirm&&ra.quick_menu);
 native_error_exit();assert(native_notice_action(MENU_ACTION_OK));
 assert(!ra.quick_menu&&paused);take_request(listener,CS_CONTROL_RECOVERY_MENU);
 /* ACK only acknowledges receipt. Correlated status is still required. */
 assert(ra.feedback.phase==MX_FEEDBACK_QUEUED);
 paused=false;observe(0U,0U);assert(!ra.save_error&&!paused);
 assert(close(listener)==0&&unlink(socket_path)==0&&rmdir(directory)==0);
 puts("NATIVE_SAVE_ERROR_MODEL_PASS native A/B notice, full navigation, policy-gated Resume, explicit unsaved exit, cancelled confirmation, no implicit release or ACK success");
 return 0;
}
