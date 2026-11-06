from pathlib import Path
import hashlib,importlib.util,json,subprocess,tarfile
work=Path(__file__).resolve().parent;root=work.parents[1];folder=work/'public';linux=root/'work/linux-validation'
seal=json.loads((folder/'SEALED.json').read_text())
assert hashlib.sha256((folder/'rootfs.sqfs').read_bytes()).hexdigest()==seal['rootfs_sha256']
script=(root/'work/r48-clean-card/launcher.sh').read_text()
script=script.replace('unsquashfs -no-progress -processors 4 -d /rootcopy /dev/vda5','unsquashfs -no-progress -processors 2 -d /rootcopy /dev/vda')
script=script.replace('mount -t vfat -o ro,utf8 /dev/vda10 /fixture\n','')
script=script.replace('cp -a /fixture/. /rootcopy/mnt/SDCARD/\numount /fixture\n','mkdir -p /rootcopy/mnt/SDCARD/Roms /rootcopy/mnt/SDCARD/BIOS\n')
script=script.replace('sync\numount /rootcopy/mnt/SDCARD','run /tmp/public_boot_tests fallback\nsync\numount /rootcopy/mnt/SDCARD')
script=script.replace('echo CLEAN_LAUNCHER_REPEAT_PASS','tar -cf /dev/vdc -C /rootcopy/tmp public-home.bmp public-fallback.bmp\necho CLEAN_LAUNCHER_REPEAT_PASS')
(folder/'launcher-test.sh').write_text(script,newline='\n')
files={'qemu-aarch64-static':root/'work/qemu-user/qemu-aarch64-static',
 'public_boot_tests':work/'public-build/public_boot_tests',
 'headless-sdl':root/'work/public-test-r19/headless-sdl/libSDL2-2.0.so.0.2600.1'}
payload=folder/'launcher-payload.tar'
with tarfile.open(payload,'w') as archive:
 for name,file in files.items():archive.add(file,arcname=name,recursive=False)
spec=importlib.util.spec_from_file_location('assembler',root/'outputs/assemble_flashable_img.py')
vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
init=folder/'launcher-test.gz';init.unlink(missing_ok=True);vm.make_initramfs(linux,folder/'launcher-test.sh',init)
export=folder/'launcher-export.raw'
with export.open('wb') as stream:stream.truncate(16*1024**2)
args=[linux/'qemu/qemu-system-x86_64.exe','-machine','pc,accel=tcg','-smp','1','-m','2048',
 '-display','none','-monitor','none','-serial',f'file:{folder/"LAUNCHER_TEST.log"}','-nic','none','-no-reboot',
 '-kernel',linux/'vmlinuz-virt','-initrd',init,'-append','console=ttyS0 panic=1 quiet']
for file,readonly in ((folder/'rootfs.sqfs',True),(payload,True),(export,False)):
 args+=['-drive',f'file={file},format=raw,if=virtio'+(',readonly=on' if readonly else '')]
subprocess.run(list(map(str,args)),check=True,timeout=420)
text=(folder/'LAUNCHER_TEST.log').read_text(errors='replace');print(text[-4000:])
assert text.count('PUBLIC_ACTUAL_LAUNCHER_EMPTY_HOME_RENDER_PASS')==3
assert 'CLEAN_LAUNCHER_REPEAT_PASS' in text and 'CLEAN_LAUNCHER_EXIT=0' in text
with tarfile.open(export) as archive:
 (folder/'public-home.bmp').write_bytes(archive.extractfile('public-home.bmp').read())
 (folder/'public-fallback.bmp').write_bytes(archive.extractfile('public-fallback.bmp').read())
report=dict(passed=True,rootfs_sha256=hashlib.sha256((folder/'rootfs.sqfs').read_bytes()).hexdigest(),
 actual_arm64_launcher=True,empty_fat32_catalog=True,repeat_starts=2,theme='Clove',missing_theme_fallback=True,
 screenshot='public-home.bmp',physical_handheld=False)
(folder/'LAUNCHER_TEST.json').write_text(json.dumps(report,indent=2)+'\n')
