#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile exact production native disk dispatch and selector with callback fixtures."""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cc',required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
module=Path(__file__).resolve().parents[1];source=(module/'magicx_retroarch.c').read_text()
functions=source[source.index('static bool disc_index('):source.index('static uint64_t controls_now(')]
start=source.index('    case CMD_EVENT_DISK_NEXT:',source.index('bool magicx_retroarch_command_allowed('));end=source.index('    case CMD_EVENT_DISK_EJECT_TOGGLE:',start);commands=source[start:end]
start=source.index('    if (entry.enum_idx == MENU_ENUM_LABEL_DISK_INDEX && magicx_retroarch_playlist_menu())');end=source.index('    if (entry.type >= MENU_SETTINGS_CORE_OPTION_START)',start);menu=source[start:end]
code=r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"native disk fixture line=%d %s\n",__LINE__,#x);exit(1);}}while(0)
struct retro_disk_control_ext_callback {unsigned (*get_num_images)(void);unsigned (*get_image_index)(void);bool (*get_eject_state)(void);bool (*set_image_index)(unsigned);bool (*set_eject_state)(bool);};
static struct {struct {struct {struct {struct retro_disk_control_ext_callback cb;}disk_control;}system;} runloop;
static struct {unsigned disc_count;bool disc_fault,quick_menu,recovery_panel;}ra;
static bool paused=true,ejected;static unsigned active,images=2,fail_index,fail_close,calls;
static unsigned get_count(void){return images;}static unsigned get_index(void){return active;}static bool get_eject(void){return ejected;}
static bool set_index(unsigned value){++calls;if(fail_index){--fail_index;return false;}active=value;return true;}
static bool set_eject(bool value){++calls;if(!value&&fail_close){--fail_close;return false;}ejected=value;return true;}
#define runloop_state_get_ptr() (&runloop)
static bool ra_paused(void *p){(void)p;return paused;}
""".replace('static struct {struct {struct {struct {struct retro_disk_control_ext_callback cb;}disk_control;}system;} runloop;','static struct {struct {struct {struct retro_disk_control_ext_callback cb;}disk_control;}system;} runloop;')+functions+r"""
enum {CMD_EVENT_DISK_NEXT=1,CMD_EVENT_DISK_PREV,MENU_ACTION_NOOP,MENU_ACTION_LEFT,MENU_ACTION_RIGHT,MENU_ACTION_OK,MENU_ACTION_UP,MENU_ENUM_LABEL_DISK_INDEX};
static bool command_allowed(unsigned cmd){switch(cmd){
"""+commands+r"""default:return true;}}
static bool command_event(unsigned cmd,void *p){(void)p;return command_allowed(cmd);}
static bool magicx_retroarch_playlist_menu(void){return ra.disc_count!=0;}
static struct {unsigned enum_idx;}entry={MENU_ENUM_LABEL_DISK_INDEX};
static unsigned menu_action(unsigned action){
"""+menu+r"""return action;}
static void reset(void){active=0;ejected=false;images=2;fail_index=fail_close=calls=0;paused=true;ra.disc_count=2;ra.disc_fault=false;ra.quick_menu=true;ra.recovery_panel=false;}
int main(void){runloop.system.disk_control.cb=(struct retro_disk_control_ext_callback){get_count,get_index,get_eject,set_index,set_eject};reset();
 CHECK(menu_action(MENU_ACTION_RIGHT)==MENU_ACTION_NOOP&&active==1&&!ejected&&!ra.disc_fault);
 CHECK(menu_action(MENU_ACTION_LEFT)==MENU_ACTION_NOOP&&active==0&&!ejected);
 CHECK(menu_action(MENU_ACTION_OK)==MENU_ACTION_NOOP&&active==1&&!ejected);
 unsigned prior=calls;CHECK(menu_action(MENU_ACTION_UP)==MENU_ACTION_NOOP&&calls==prior);
 reset();fail_index=1;menu_action(MENU_ACTION_RIGHT);CHECK(active==0&&!ejected&&!ra.disc_fault);
 reset();fail_close=1;menu_action(MENU_ACTION_RIGHT);CHECK(active==0&&!ejected&&!ra.disc_fault);
 reset();fail_index=2;menu_action(MENU_ACTION_RIGHT);CHECK(ra.disc_fault);prior=calls;menu_action(MENU_ACTION_RIGHT);CHECK(calls==prior);
 reset();paused=false;menu_action(MENU_ACTION_RIGHT);CHECK(active==0&&calls==0);
 reset();ra.recovery_panel=true;menu_action(MENU_ACTION_RIGHT);CHECK(active==0&&calls==0);
 reset();images=3;menu_action(MENU_ACTION_RIGHT);CHECK(active==0&&calls==0);
 reset();CHECK(!disc_select(2)&&active==0&&calls==0);
 puts("NATIVE_DISC_REAL_DISPATCH_NEXT_PREVIOUS_OK_TRAY_VERIFICATION_ROLLBACK_AND_FAULT_GATE_PASS");return 0;}
"""
c=a.output/'native-disc.c';c.write_text(code,encoding='utf-8',newline='\n');exe=a.output/'native-disc-test.exe'
subprocess.run([a.cc,'cc','-O2','-UNDEBUG','-Wall','-Werror',str(c),'-o',str(exe)],check=True)
result=subprocess.run([str(exe.resolve())],capture_output=True,text=True,check=True);print(result.stdout)
(a.output/'RESULT.json').write_text(json.dumps(dict(passed=True,actual_production_functions_extracted=True,production_sha256=hashlib.sha256(source.encode()).hexdigest(),disk_callbacks_are_fixtures=True,physical_hardware=False),indent=2)+'\n')
