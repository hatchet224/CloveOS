from pathlib import Path
import importlib.util,subprocess,tarfile,io,json,sys
work=Path(__file__).resolve().parent;root=work.parents[1];linux=root/'work/linux-validation'
out=work/'test';out.mkdir(exist_ok=True)
script=(root/'work/r48-clean-card/launcher.sh').read_text(encoding='utf-8').replace('set -eu','set -eux')
script=script.replace('unsquashfs -no-progress -processors 4 -d /rootcopy /dev/vda5','unsquashfs -no-progress -processors 2 -d /rootcopy /dev/vda')
script=script.replace('truncate -s 64M /fixture.img','truncate -s 256M /fixture.img')
script=script.replace('mount -t vfat -o ro,utf8 /dev/vda10 /fixture\n','')
script=script.replace('cp -a /fixture/. /rootcopy/mnt/SDCARD/\numount /fixture\n','mkdir -p /rootcopy/mnt/SDCARD/Roms /rootcopy/mnt/SDCARD/BIOS /rootcopy/mnt/SDCARD/.console\n')
script=script.replace('tar -xf /dev/vdb -C /payload', r'''tar -xf /dev/vdb -C /payload
chmod 0755 /payload/qemu-aarch64-static
modprobe binfmt_misc
mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
mkdir -p /tmp
cp /payload/qemu-aarch64-static /tmp/qemu-aarch64-static
chmod 0755 /payload/test-binfmt
chmod 0755 /payload/test-subreaper
/payload/test-binfmt''')
script=script.replace('mount --bind /rootcopy/system /rootcopy/system','''tar -xf /payload/runtime.tar -C /rootcopy
cp /payload/headless-sdl /rootcopy/system/portmaster/lib/libSDL2-2.0.so.0
cp /payload/console-ports /rootcopy/system/bin/
cp /payload/console-portmaster-launch /rootcopy/system/bin/
chmod 0755 /rootcopy/system/bin/console-ports /rootcopy/system/bin/console-portmaster-launch
cp /payload/r50_language_tests /payload/launcher_preferences_tests /rootcopy/tmp/ 2>/dev/null || true
mount --bind /rootcopy/system /rootcopy/system''')
# /tmp is mounted after payload copy in the base script, so copy tests below.
start=script.index('cp /payload/qemu-aarch64-static /payload/public_boot_tests /rootcopy/tmp/')
script=script.replace('code=$?; trap - EXIT;', 'code=$?; trap - EXIT; tar -cf /dev/vdc -C /rootcopy/tmp es-settings.bmp es-language.bmp es-tweaks.bmp es-dialog.bmp es-fallback.bmp portmaster-home.bmp portmaster-portrait.bmp native-port-portrait.bmp native-port-result.log 2>/dev/null || true;')
script=script[:script.index('cp /payload/qemu-aarch64-static /payload/public_boot_tests /rootcopy/tmp/')]+'''cp /payload/qemu-aarch64-static /payload/r50_language_tests /payload/launcher_preferences_tests /payload/r50_ports_tests /payload/clove-native-demo.zip /payload/pm_smoke.py /rootcopy/tmp/
cp /payload/headless-sdl /rootcopy/tmp/libSDL2-2.0.so.0
chmod 0755 /rootcopy/tmp/qemu-aarch64-static /rootcopy/tmp/r50_language_tests /rootcopy/tmp/launcher_preferences_tests /rootcopy/tmp/r50_ports_tests
run() { timeout -s KILL 300 chroot /rootcopy /tmp/qemu-aarch64-static -E SDL_VIDEODRIVER=dummy -E SDL_AUDIODRIVER=dummy -E LD_LIBRARY_PATH=/system/portmaster/lib:/tmp:/system/lib:/usr/magicx/lib:/lib:/usr/lib "$@"; }
run /tmp/r50_language_tests
run /tmp/launcher_preferences_tests /mnt/SDCARD/preferences-test
mkdir -p /rootcopy/mnt/SDCARD/Roms/ports
cp -R /rootcopy/system/portmaster/template/. /rootcopy/mnt/SDCARD/Roms/ports/PortMaster/
mkdir -p /rootcopy/mnt/SDCARD/Roms/ports/PortMaster/home
run /system/portmaster/python/bin/python3.12 /tmp/pm_smoke.py
timeout -s KILL 300 /payload/test-subreaper chroot /rootcopy /tmp/qemu-aarch64-static -E SDL_VIDEODRIVER=dummy -E SDL_AUDIODRIVER=dummy -E LD_LIBRARY_PATH=/system/portmaster/lib:/tmp:/system/lib:/usr/magicx/lib:/lib:/usr/lib /tmp/r50_ports_tests
cat /rootcopy/tmp/native-port-result.log
tar -cf /dev/vdc -C /rootcopy/tmp es-settings.bmp es-language.bmp es-tweaks.bmp es-dialog.bmp es-fallback.bmp portmaster-home.bmp portmaster-portrait.bmp native-port-portrait.bmp native-port-result.log
sync
umount /rootcopy/mnt/SDCARD
fsck.fat -n /fixture.img
echo R50_VM_LANGUAGE_GUI_COMPLETE
'''
assert '\x00' not in script
ports_only='--ports-only' in sys.argv
if ports_only:
 script=script.replace('run /tmp/r50_language_tests\n','').replace('run /tmp/launcher_preferences_tests /mnt/SDCARD/preferences-test\n','')
 script=script.replace('run /system/portmaster/python/bin/python3.12 /tmp/pm_smoke.py', 'unzip -o /payload/clove-native-demo.zip -d /rootcopy/mnt/SDCARD/Roms/ports >/dev/null')
 script=script.replace('es-settings.bmp es-language.bmp es-tweaks.bmp es-dialog.bmp es-fallback.bmp portmaster-home.bmp portmaster-portrait.bmp native-port-portrait.bmp native-port-result.log', 'native-port-portrait.bmp native-port-result.log')
(out/'guest.sh').write_text(script,encoding='utf-8',newline='\n')
files={'test-subreaper':work/'test-subreaper','test-binfmt':work/'test-binfmt','qemu-aarch64-static':root/'work/qemu-user/qemu-aarch64-static',
 'r50_language_tests':work/'build/r50_language_tests',
 'launcher_preferences_tests':work/'build/launcher/launcher_preferences_tests',
 'r50_ports_tests':work/'build/r50_ports_tests','clove-native-demo.zip':work/'test/clove-native-demo.zip',
 'console-ports':work/'build/console-ports',
 'console-portmaster-launch':work/'source/console-supervisor/init/console-portmaster-launch',
 'pm_smoke.py':work/'pm_smoke.py','runtime.tar':work/'portmaster-runtime.tar',
 'headless-sdl':root/'work/public-test-r19/headless-sdl/libSDL2-2.0.so.0.2600.1'}
with tarfile.open(out/'payload.tar','w') as archive:
 for name,file in files.items():archive.add(file,arcname=name,recursive=False)
spec=importlib.util.spec_from_file_location('assembler',root/'outputs/assemble_flashable_img.py');vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
init=out/'test.gz';init.unlink(missing_ok=True);vm.make_initramfs(linux,out/'guest.sh',init)
export=out/'export.raw'
with export.open('wb') as f:f.truncate(32*1024**2)
args=[linux/'qemu/qemu-system-x86_64.exe','-machine','pc,accel=tcg','-smp','1','-m','2048','-display','none','-monitor','none','-serial',f'file:{out/"VM.log"}','-nic','none','-no-reboot','-kernel',linux/'vmlinuz-virt','-initrd',init,'-append','console=ttyS0 panic=1 quiet']
for file,ro in ((root/'work/psx-switch-r49/public/rootfs.sqfs',True),(out/'payload.tar',True),(export,False)):
 args+=['-drive',f'file={file},format=raw,if=virtio'+(',readonly=on' if ro else '')]
try:subprocess.run(list(map(str,args)),check=True,timeout=480)
finally:print((out/'VM.log').read_text(errors='replace')[-11000:],flush=True)
text=(out/'VM.log').read_text(errors='replace')
with tarfile.open(export) as archive:archive.extractall(out,filter='data')
assert 'R50_VM_LANGUAGE_GUI_COMPLETE' in text
