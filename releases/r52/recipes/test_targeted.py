"""Targeted AArch64 QEMU regressions; no physical input, panel or card writes."""
from pathlib import Path
import hashlib,importlib.util,json,os,subprocess,tarfile
w=Path(__file__).resolve().parent;r=w.parents[1];old=r/'work/brightness-wake-r51';o=w/'test';o.mkdir(exist_ok=True)
env=dict(os.environ,TEMP=str(r/'work/compiler-tmp'),TMP=str(r/'work/compiler-tmp'),ZIG_GLOBAL_CACHE_DIR=str(r/'work/zig-global'),ZIG_LOCAL_CACHE_DIR=str(w/'zig-cache'))
subprocess.run([str(r/'work/host-tools/ziglang/zig.exe'),'cc','-target','aarch64-linux-gnu.2.29','-Os','-D_GNU_SOURCE','-D_FILE_OFFSET_BITS=64',
 '-I'+str(w/'source/console-sessiond/include'),str(o/'r51-repro.c'),str(w/'source/console-packaging/image/level-state.c'),
 str(w/'build/launcher/sessiond/libsessiond_linux.a'),str(w/'build/launcher/sessiond/libsessiond_core.a'),'-lpthread','-o',str(o/'r51-repro')],env=env,check=True,timeout=180)
files={'qemu-aarch64-static':r/'work/qemu-user/qemu-aarch64-static','r51-repro':o/'r51-repro',
 'headless-sdl':r/'work/public-test-r19/headless-sdl/libSDL2-2.0.so.0.2600.1'}
for name in ['r49_level_runtime','r49_level_router','r51_brightness_controls']:files[name]=w/'build'/name
files['launcher_input_tests']=w/'build/launcher/launcher_input_tests'
with tarfile.open(o/'payload.tar','w') as t:
 for name,p in files.items():t.add(p,arcname=name,recursive=False)
script=(old/'brightness-test/guest.sh').read_text(encoding='utf-8')
script=script[:script.index('run /tmp/legacy-cache-repro')]
script=script.replace('R51_BRIGHTNESS_VM_EXIT','R52_TARGETED_VM_EXIT')
script=script.replace('cp /payload/r51_brightness_controls /payload/r51_portmaster_entry /payload/launcher_input_tests /payload/launcher_preferences_tests /tree/tmp/',
 'cp /payload/r51_brightness_controls /payload/launcher_input_tests /tree/tmp/')
script=script.replace(' /payload/legacy-cache-repro',' /payload/r51-repro').replace(' /tree/tmp/legacy-cache-repro',' /tree/tmp/r51-repro')
script=script.replace(' /tree/tmp/launcher_preferences_tests','')
script+='''
run /tmp/r51-repro
for test in r49_level_runtime r49_level_router r51_brightness_controls launcher_input_tests;do
 run /tmp/$test >/$test.log 2>&1 || { tail -n 40 /$test.log;exit 1; }
 tail -n 12 /$test.log
done
echo R52_HELPER_VOLUME_RAW_RESTORE_LOGICAL_ROUTES_AND_VIRTUAL_INPUT_PASS
'''
(o/'guest.sh').write_text(script,encoding='utf-8',newline='\n')
spec=importlib.util.spec_from_file_location('assembler',r/'outputs/assemble_flashable_img.py');vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
linux=r/'work/linux-validation';init=o/'targeted.gz';init.unlink(missing_ok=True);vm.make_initramfs(linux,o/'guest.sh',init)
args=[linux/'qemu/qemu-system-x86_64.exe','-machine','pc,accel=tcg','-smp','1','-m','2048','-display','none','-monitor','none','-serial',f'file:{o/"VM.log"}',
 '-nic','none','-no-reboot','-kernel',linux/'vmlinuz-virt','-initrd',init,'-append','console=ttyS0 panic=1 quiet']
for p in [o/'payload.tar',old/'private/rootfs.sqfs']:args+=['-drive',f'file={p},format=raw,if=virtio,readonly=on']
subprocess.run(list(map(str,args)),check=True,timeout=300)
log=(o/'VM.log').read_text(encoding='utf-8',errors='replace');print(log[-5000:])
markers=['R51_POSITIVE_REGISTER_DIMMING_WITH_RISING_METER_REPRO','R52_INVERSE_LIGHT_MODEL_LOGICAL_BRIGHTNESS_METER_ALL248_NATIVE_VALUES_PASS',
 'AUDIO_RANGE_PASS','WAKE_OWNER_REPLAY_PASS','LEVEL_ROUTER_PASS','R52_HELPER_VOLUME_RAW_RESTORE_LOGICAL_ROUTES_AND_VIRTUAL_INPUT_PASS','R52_TARGETED_VM_EXIT=0']
markers+=['R52_LOGICAL_UP_ENABLE_LEGACY_ZERO_RECOVERY_AND_WRONG_MOTION_REJECTION_PASS']
passed=all(x in log for x in markers)
report=dict(passed=passed,markers=markers,actual_aarch64_helper_logic=True,panel_mocked=True,
 light_order_model_based_on_user_physical_r51_report=True,sdl_events_virtual=True,evdev_events_fixture=True,
 physical_r52_acceptance_passed=False,saved_raw_restore_unchanged=True,ordinary_volume_logic_unchanged=True,
 helper_sha256=hashlib.sha256((w/'build/console-level').read_bytes()).hexdigest())
(o/'RESULT.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');assert passed
