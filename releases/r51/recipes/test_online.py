from pathlib import Path
import importlib.util,json,subprocess,tarfile,sys
work=Path(__file__).resolve().parent;root=work.parents[1];r50=root/'work/cloveos-language-r50';out=work/'online-test';out.mkdir(exist_ok=True)
script=(r50/'test/guest.sh').read_text(encoding='utf-8')
script=script[:script.index('run /tmp/r50_language_tests\n')]
script=script.replace('es-settings.bmp es-language.bmp es-tweaks.bmp es-dialog.bmp es-fallback.bmp portmaster-home.bmp portmaster-portrait.bmp native-port-portrait.bmp native-port-result.log','R51_ONLINE.json')
script=script.replace('cp /payload/r50_language_tests /payload/launcher_preferences_tests /rootcopy/tmp/ 2>/dev/null || true','cp /payload/online_probe.py /rootcopy/tmp/')
script=script.replace('tar -xf /payload/runtime.tar -C /rootcopy','tar -xf /payload/runtime.tar -C /rootcopy\nrm -f /payload/runtime.tar')
script=script.replace('code=$?; trap - EXIT;', 'code=$?; trap - EXIT; if mountpoint -q /rootcopy/mnt/SDCARD;then tar -cf /rootcopy/tmp/portmaster-test-cache.tar -C /rootcopy/mnt/SDCARD/Roms/ports PortMaster/config wordlesdl "Wordle SDL.sh" 2>/dev/null || true;fi;')
script=script.replace('tar -cf /dev/vdc -C /rootcopy/tmp R51_ONLINE.json','tar -cf /dev/vdc -C /rootcopy/tmp portmaster-test-cache.tar R51_ONLINE.json')
start=script.index('cp /payload/qemu-aarch64-static /payload/r50_language_tests')
script=script[:start]+'''cp /payload/qemu-aarch64-static /payload/online_probe.py /rootcopy/tmp/
cp /payload/headless-sdl /rootcopy/tmp/libSDL2-2.0.so.0
chmod 0755 /rootcopy/tmp/qemu-aarch64-static
modprobe virtio_net
ln -s /proc/self/fd /dev/fd
ln -s /proc/self/fd/0 /dev/stdin
ln -s /proc/self/fd/1 /dev/stdout
ln -s /proc/self/fd/2 /dev/stderr
cat >/tmp/dhcp.sh <<'DHCP'
#!/bin/sh
if [ "$1" = bound ] || [ "$1" = renew ];then
 ifconfig "$interface" "$ip" netmask "$subnet" up
 for gateway in $router;do route add default gw "$gateway" dev "$interface";break;done
 echo 'nameserver 10.0.2.3' >/etc/resolv.conf
fi
DHCP
chmod 0755 /tmp/dhcp.sh
ifconfig eth0 up
udhcpc -i eth0 -s /tmp/dhcp.sh -n -q -t 5
rm -f /rootcopy/etc/resolv.conf
cp /etc/resolv.conf /rootcopy/etc/resolv.conf
date -u
mkdir -p /rootcopy/mnt/SDCARD/Roms/ports/PortMaster/home
cp -R /rootcopy/system/portmaster/template/. /rootcopy/mnt/SDCARD/Roms/ports/PortMaster/
if [ -f /payload/portmaster-test-cache.tar ];then tar -xf /payload/portmaster-test-cache.tar -C /rootcopy/mnt/SDCARD/Roms/ports;fi
rm -f /payload/portmaster-test-cache.tar
cp /payload/online-game-probe.so /rootcopy/tmp/
cp /payload/online_actions.py /rootcopy/tmp/
timeout -s KILL 1000 chroot /rootcopy /tmp/qemu-aarch64-static -E PYTHONHOME=/system/portmaster/python -E PYTHONDONTWRITEBYTECODE=1 -E PYTHONNOUSERSITE=1 -E SDL_VIDEODRIVER=dummy -E SDL_AUDIODRIVER=dummy -E LD_LIBRARY_PATH=/system/portmaster/lib:/tmp:/system/lib:/usr/magicx/lib:/lib:/usr/lib /system/portmaster/python/bin/python3.12 /tmp/online_probe.py
tar -cf /rootcopy/tmp/portmaster-test-cache.tar -C /rootcopy/mnt/SDCARD/Roms/ports PortMaster/config wordlesdl "Wordle SDL.sh"
tar -cf /dev/vdc -C /rootcopy/tmp portmaster-test-cache.tar R51_ONLINE.json
sync
umount /rootcopy/mnt/SDCARD
losetup -d "$loop"
sync
fsck.fat -n /fixture.img
echo R51_REAL_ONLINE_VM_COMPLETE
'''
(out/'guest.sh').write_text(script,encoding='utf-8',newline='\n')
files={'test-subreaper':r50/'test-subreaper','test-binfmt':r50/'test-binfmt',
 'qemu-aarch64-static':root/'work/qemu-user/qemu-aarch64-static',
 'console-ports':work/'build/console-ports','console-portmaster-launch':work/'source/console-supervisor/init/console-portmaster-launch',
 'online_probe.py':work/('chain_probe.py' if '--chain-only' in sys.argv else 'full_chain_probe.py'),'runtime.tar':work/'portmaster-runtime.tar',
 'online_actions.py':work/'online_actions.py',
 'online-game-probe.so':work/'online-game-probe.so',
 'headless-sdl':root/'work/public-test-r19/headless-sdl/libSDL2-2.0.so.0.2600.1'}
if (out/'portmaster-test-cache.tar').is_file():files['portmaster-test-cache.tar']=out/'portmaster-test-cache.tar'
with tarfile.open(out/'payload.tar','w') as archive:
 for name,file in files.items():archive.add(file,arcname=name,recursive=False)
spec=importlib.util.spec_from_file_location('assembler',root/'outputs/assemble_flashable_img.py');vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm);linux=root/'work/linux-validation'
init=out/'online.gz';init.unlink(missing_ok=True);vm.make_initramfs(linux,out/'guest.sh',init)
export=out/'export.raw'
with export.open('wb') as f:f.truncate(256*1024**2)
args=[linux/'qemu/qemu-system-x86_64.exe','-machine','pc,accel=tcg','-smp','2','-m','2048','-display','none','-monitor','none','-serial',f'file:{out/"VM.log"}',
 '-nic','user,model=virtio-net-pci','-no-reboot','-kernel',linux/'vmlinuz-virt','-initrd',init,'-append','console=ttyS0 panic=1 quiet']
for file,ro in [(root/'work/psx-switch-r49/public/rootfs.sqfs',True),(out/'payload.tar',True),(export,False)]:args+=['-drive',f'file={file},format=raw,if=virtio'+(',readonly=on' if ro else '')]
subprocess.run(list(map(str,args)),check=True,timeout=1100)
text=(out/'VM.log').read_text(encoding='utf-8',errors='replace');print(text[-6000:],flush=True)
assert 'R51_REAL_ONLINE_VM_COMPLETE' in text
with tarfile.open(export) as archive:archive.extractall(out,filter='data')
assert json.loads((out/'R51_ONLINE.json').read_text(encoding='utf-8'))['passed']
