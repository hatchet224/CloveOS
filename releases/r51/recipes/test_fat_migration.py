"""Offline generated-card migration/cache and loop flush diagnostic."""
from pathlib import Path
import importlib.util,subprocess,tarfile,json
w=Path(__file__).resolve().parent;r=w.parents[1];o=w/'fat-migration-test';o.mkdir(exist_ok=True)
script=(w/'online-test/guest.sh').read_text(encoding='utf-8')
script=script[:script.index('modprobe virtio_net\n')]
script=script.replace('tar -xf /payload/runtime.tar -C /rootcopy','tar -xf /payload/runtime.tar -C /rootcopy\nrm -f /payload/runtime.tar')
script=script.replace('finish() {', 'finish() {')
start=script.index('finish() {');end=script.index('\ntrap finish EXIT',start)
script=script[:start]+"finish(){ code=$?;trap - EXIT;set +e;tar -cf /dev/vdc -C /tmp fsck-before-detach.log fsck-after-detach.log;echo R51_FAT_MIGRATION_EXIT=$code;poweroff -f; }"+script[end:]
script+='''
mkdir -p /rootcopy/mnt/SDCARD/Roms/ports
cp -R /rootcopy/system/portmaster/template/. /rootcopy/mnt/SDCARD/Roms/ports/PortMaster/
tar -xf /payload/portmaster-test-cache.tar -C /rootcopy/mnt/SDCARD/Roms/ports
rm -f /payload/portmaster-test-cache.tar
printf 'user-config-retained\\n' >/rootcopy/mnt/SDCARD/Roms/ports/PortMaster/config/r51-user-probe
touch /rootcopy/mnt/SDCARD/Roms/ports/PortMaster/.clove-template-r50
chroot /rootcopy /tmp/qemu-aarch64-static /bin/sh /system/bin/console-portmaster-launch setup
test "$(cat /rootcopy/mnt/SDCARD/Roms/ports/PortMaster/config/r51-user-probe)" = user-config-retained
sync
umount /rootcopy/mnt/SDCARD
df -h / || true
ls -l /fixture.img "$(command -v fsck.fat)"
command -v fsck.fat
fsck.fat -n /fixture.img >/tmp/fsck-before-detach.log 2>&1 || true
cat /tmp/fsck-before-detach.log
losetup -d "$loop"
sync
set +e
fsck.fat -n /fixture.img >/tmp/fsck-after-detach.log 2>&1
result=$?
set -e
cat /tmp/fsck-after-detach.log
echo FSCK_RESULT=$result
dd if=/fixture.img of=/dev/vdd bs=1048576 conv=fsync
dmesg | tail -n 12
test "$result" = 0
echo R51_FAT_CACHE_MIGRATION_FLUSH_DETACH_PASS
'''
(o/'guest.sh').write_text(script,encoding='utf-8',newline='\n')
files={'test-subreaper':r/'work/cloveos-language-r50/test-subreaper','test-binfmt':r/'work/cloveos-language-r50/test-binfmt',
 'qemu-aarch64-static':r/'work/qemu-user/qemu-aarch64-static','console-ports':w/'build/console-ports',
 'console-portmaster-launch':w/'source/console-supervisor/init/console-portmaster-launch',
 'online_probe.py':w/'chain_probe.py','runtime.tar':w/'portmaster-runtime.tar',
 'portmaster-test-cache.tar':w/'online-test/portmaster-test-cache.tar',
 'headless-sdl':r/'work/public-test-r19/headless-sdl/libSDL2-2.0.so.0.2600.1'}
with tarfile.open(o/'payload.tar','w') as t:
 for name,p in files.items():t.add(p,arcname=name,recursive=False)
spec=importlib.util.spec_from_file_location('assembler',r/'outputs/assemble_flashable_img.py');vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
linux=r/'work/linux-validation';init=o/'fat.gz';init.unlink(missing_ok=True);vm.make_initramfs(linux,o/'guest.sh',init)
export=o/'export.raw'
with export.open('wb') as f:f.truncate(16*1024**2)
args=[linux/'qemu/qemu-system-x86_64.exe','-machine','pc,accel=tcg','-smp','2','-m','2048','-display','none','-monitor','none','-serial',f'file:{o/"VM.log"}','-nic','none','-no-reboot','-kernel',linux/'vmlinuz-virt','-initrd',init,'-append','console=ttyS0 panic=1 quiet']
for p,ro in [(r/'work/psx-switch-r49/public/rootfs.sqfs',True),(o/'payload.tar',True),(export,False)]:args+=['-drive',f'file={p},format=raw,if=virtio'+(',readonly=on'if ro else '')]
fat_export=o/'fat-export.img'
with fat_export.open('wb') as f:f.truncate(256*1024**2)
args+=['-drive',f'file={fat_export},format=raw,if=virtio']
subprocess.run(list(map(str,args)),check=True,timeout=420)
log=(o/'VM.log').read_text(encoding='utf-8',errors='replace');print(log[-5000:])
with tarfile.open(export) as t:t.extractall(o,filter='data')
passed='R51_FAT_CACHE_MIGRATION_FLUSH_DETACH_PASS' in log and 'R51_FAT_MIGRATION_EXIT=0' in log
(o/'RESULT.json').write_text(json.dumps({'passed':passed,'physical_card':False,'cached_real_catalog':True,'loop_detached_before_final_backing_file_check':True},indent=2)+'\n')
assert passed
