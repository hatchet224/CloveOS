from pathlib import Path
import hashlib,importlib.util,json,os,subprocess,tarfile
work=Path(__file__).resolve().parent;root=work.parents[1];folder=work/'brightness-test';folder.mkdir(exist_ok=True)
legacy='''#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ioctl.h>
static int cached=8,physical=128;
static int mock_ioctl(int fd,unsigned long cmd,...){
 (void)fd;va_list va;va_start(va,cmd);unsigned long *a=va_arg(va,unsigned long *);va_end(va);
 if(cmd==0x103UL)return cached;
 assert(cmd==0x102UL);if(cached!=(int)a[1])physical=(int)a[1];cached=(int)a[1];return 0;
}
#define ioctl mock_ioctl
'''+ '#include "'+(root/'work/cloveos-language-r50/source/console-packaging/image/backlight-runtime.h').as_posix()+'''"
int main(void){
 assert(cl_brightness_reapply(1,8U)==0&&cached==8&&physical==128);
 unsigned long off[4]={0UL,0UL,0UL,0UL};mock_ioctl(1,0x102UL,off);
 assert(cl_brightness_reapply(1,8U)==0&&physical==8);
 mock_ioctl(1,0x102UL,off);assert(cl_brightness_reapply(1,8U)==0&&physical==8);
 puts("R50_CACHED_GET_BOOT_FALSE_SUCCESS_FIRST_WAKE_DROP_REPRO repeated_wake_stable=1 physical_mock_only=1");return 0;
}
'''
(folder/'legacy-cache-repro.c').write_text(legacy,encoding='utf-8')
env=dict(os.environ,TEMP=str(root/'work/compiler-tmp'),TMP=str(root/'work/compiler-tmp'),ZIG_GLOBAL_CACHE_DIR=str(root/'work/zig-global'),ZIG_LOCAL_CACHE_DIR=str(work/'zig-cache'))
subprocess.run([str(root/'work/host-tools/ziglang/zig.exe'),'cc','-target','aarch64-linux-gnu.2.29','-Os',str(folder/'legacy-cache-repro.c'),'-o',str(folder/'legacy-cache-repro')],env=env,check=True,timeout=180)
files={'qemu-aarch64-static':root/'work/qemu-user/qemu-aarch64-static','legacy-cache-repro':folder/'legacy-cache-repro',
 'headless-sdl':root/'work/public-test-r19/headless-sdl/libSDL2-2.0.so.0.2600.1'}
for name in ['r49_standby_display','r49_level_runtime','r49_level_router','r51_brightness_controls']:files[name]=work/'build'/name
files['r51_portmaster_entry']=work/'build/r51_portmaster_entry'
files['launcher_preferences_tests']=work/'build/launcher/launcher_preferences_tests'
files['launcher_input_tests']=work/'build/launcher/launcher_input_tests'
files['five_apps_backend_tests']=work/'build/five_apps_backend_tests'
for name in ['r46-packages.json','screenshot.png','screenshot.jpg']:files[name]=root/'work/cloveos-apps'/name
with tarfile.open(folder/'payload.tar','w') as archive:
 for name,file in files.items():archive.add(file,arcname=name,recursive=False)
guest=(root/'work/psx-switch-r49/wake_guest.sh').read_text(encoding='utf-8')
guest=guest.replace('R49_WAKE_VM_EXIT','R51_BRIGHTNESS_VM_EXIT').replace(' /payload/legacy-repro',' /payload/legacy-cache-repro').replace(' /tree/tmp/legacy-repro',' /tree/tmp/legacy-cache-repro')
guest=guest.replace('run /tmp/legacy-repro','run /tmp/legacy-cache-repro').replace("grep 'R48_MISSING_SAVED_BRIGHTNESS_REPRO'","grep 'R50_CACHED_GET_BOOT_FALSE_SUCCESS_FIRST_WAKE_DROP_REPRO'")
guest=guest.replace('cp /payload/qemu-aarch64-static /payload/r49_*','cp /payload/headless-sdl /tree/tmp/libSDL2-2.0.so.0\ncp /payload/r51_brightness_controls /payload/launcher_input_tests /tree/tmp/\ncp /payload/qemu-aarch64-static /payload/r49_*')
guest=guest.replace('chmod 755 /tree/tmp/qemu-aarch64-static','chmod 755 /tree/tmp/r51_brightness_controls /tree/tmp/launcher_input_tests\nchmod 755 /tree/tmp/qemu-aarch64-static')
guest=guest.replace('cp /payload/r51_brightness_controls /payload/launcher_input_tests /tree/tmp/',
 'cp /payload/r51_brightness_controls /payload/r51_portmaster_entry /payload/launcher_input_tests /payload/launcher_preferences_tests /tree/tmp/')
guest=guest.replace('chmod 755 /tree/tmp/r51_brightness_controls /tree/tmp/launcher_input_tests',
 'chmod 755 /tree/tmp/r51_* /tree/tmp/launcher_input_tests /tree/tmp/launcher_preferences_tests')
guest=guest.replace('-E LD_LIBRARY_PATH=', '-E SDL_VIDEODRIVER=dummy -E SDL_AUDIODRIVER=dummy -E LD_LIBRARY_PATH=/tmp:')
guest=guest.replace('r49_failed_launch_recovery r49_managed_control','r51_brightness_controls launcher_input_tests')
guest=guest.replace('r51_brightness_controls launcher_input_tests;do','r51_brightness_controls launcher_input_tests r51_portmaster_entry;do')
guest+='''
modprobe vfat;modprobe nls_cp437;modprobe nls_utf8
truncate -s 64M /preferences.img
mkfs.fat -F32 -i C0280002 /preferences.img >/dev/null
mkdir -p /tree/mnt/SDCARD
mount -t vfat -o loop,rw,nosuid,nodev,noexec,fmask=0077,dmask=0077 /preferences.img /tree/mnt/SDCARD
run /tmp/launcher_preferences_tests /mnt/SDCARD/preference-test
umount /tree/mnt/SDCARD
fsck.fat -n /preferences.img
'''
backend=(root/'work/cloveos-apps/guest.sh').read_text(encoding='utf-8')
backend=backend[backend.index('truncate -s 64M /card.img'):backend.index('umount /tree/tmp /tree/sys /tree/dev /tree/proc')]
guest+='\nmount -t tmpfs tmpfs /tree/sys\ncp /payload/five_apps_backend_tests /tree/tmp/\nchmod 755 /tree/tmp/five_apps_backend_tests\nmkdir -p /dev/disk/by-uuid\n'+backend
(folder/'guest.sh').write_text(guest,encoding='utf-8',newline='\n')
spec=importlib.util.spec_from_file_location('assembler',root/'outputs/assemble_flashable_img.py');vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm);linux=root/'work/linux-validation'
init=folder/'brightness.gz';init.unlink(missing_ok=True);vm.make_initramfs(linux,folder/'guest.sh',init)
args=[linux/'qemu/qemu-system-x86_64.exe','-machine','pc,accel=tcg','-smp','1','-m','2048','-display','none','-monitor','none','-serial',f'file:{folder/"VM.log"}','-nic','none','-no-reboot','-kernel',linux/'vmlinuz-virt','-initrd',init,'-append','console=ttyS0 panic=1 quiet']
for p in [folder/'payload.tar',root/'work/cloveos-language-r50/private/rootfs.sqfs']:args+=['-drive',f'file={p},format=raw,if=virtio,readonly=on']
subprocess.run(list(map(str,args)),check=True,timeout=420)
text=(folder/'VM.log').read_text(encoding='utf-8',errors='replace');print(text[-6000:],flush=True)
markers=['R51_BRIGHTNESS_VM_EXIT=0','R50_CACHED_GET_BOOT_FALSE_SUCCESS_FIRST_WAKE_DROP_REPRO',
 'R51_FIRST_AND_REPEATED_WAKE_CACHED_VS_APPLIED_PASS','WAKE_OWNER_REPLAY_PASS',
 'R51_MAINUI_PHYSICAL_MENU_VOLUME_POLARITY_REPEAT_RELEASE_AND_SETTINGS_PASS','LEVEL_ROUTER_PASS',
 'R51_PORTMASTER_SYSTEM_ENTRY_DIRECT_CATALOG_INSTALLED_PORTS_TOGGLE_AND_SPANISH_PASS',
 'R51_PORTMASTER_VISIBILITY_PERSIST_RESTART_INDEPENDENT_LANGUAGE_AND_CONTROLS_PASS']
markers+=['R51_PORTMASTER_FILTER_BEFORE_PAGINATION_GLOBAL_TOTALS_BOUNDARY_AND_CLAMP_PASS']
passed=all(marker in text for marker in markers)
(folder/'RESULT.json').write_text(json.dumps({'passed':passed,'markers':markers,'hardware_mocked':True,'real_sdl_virtual_joystick_events':True,'physical_handheld_tested':False},indent=2)+'\n',encoding='utf-8')
assert passed
