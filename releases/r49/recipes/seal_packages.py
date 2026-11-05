from pathlib import Path
import hashlib,importlib.util,json,shutil,subprocess,sys,tarfile
work=Path(__file__).resolve().parent;root=work.parents[1];linux=root/'work/linux-validation'
name='public' if '--public' in sys.argv else 'private';folder=work/name
baseline=root/('work/r48-autogrow/rootfs.sqfs' if name=='public' else 'outputs/cloveos-apps-r48/CloveOS-r48-rootfs.sqfs')
expected='643a73e7cc15bc9755b9dbf635beaee0758e63e16ad2186e1e5dba846f7e63f1' if name=='public' else '6ca9ece96e83ce7e708d83d958b5c2bcb2a81a753e120dc856755ed2fc60a4b3'
def sha(file):
 with file.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
assert sha(baseline)==expected
spec=importlib.util.spec_from_file_location('assembler',root/'outputs/assemble_flashable_img.py')
vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
init=folder/'seal.gz';init.unlink(missing_ok=True);vm.make_initramfs(linux,work/'seal_guest.sh',init)
exports=folder/'seal-export.raw';audit=folder/'rootfs-audit.tar'
for file,size in [(exports,512*1024**2),(audit,768*1024**2)]:
 with file.open('wb') as target:target.truncate(size)
args=[linux/'qemu/qemu-system-x86_64.exe','-machine','pc,accel=tcg','-smp','1','-m','4096','-display','none',
 '-monitor','none','-serial',f'file:{folder/"SEAL.log"}','-nic','none','-no-reboot','-kernel',linux/'vmlinuz-virt',
 '-initrd',init,'-append','console=ttyS0 panic=1 quiet']
for file,readonly in [(baseline,True),(folder/'overlay.tar',True),(exports,False),(audit,False)]:
 args+=['-drive',f'file={file},format=raw,if=virtio'+(',readonly=on' if readonly else '')]
subprocess.run(list(map(str,args)),check=True,timeout=3600)
log=(folder/'SEAL.log').read_text(errors='replace');print(log[-1800:],flush=True)
assert 'R49_SEAL_VM_EXIT=0' in log
with tarfile.open(exports) as archive:
 for leaf in ('rootfs.sqfs','baseline-files.sha256','root-files.sha256','mksquashfs.txt'):
  entry=next(e for e in archive if e.name.removeprefix('./')==leaf)
  with archive.extractfile(entry) as source,(folder/leaf).open('wb') as target:shutil.copyfileobj(source,target)
def inventory(file):return {p:d for d,p in (line.split('  ',1) for line in file.read_text().splitlines())}
before=inventory(folder/'baseline-files.sha256');after=inventory(folder/'root-files.sha256')
overlay=json.loads((folder/'OVERLAY.json').read_text())
control={'expected-base-manifest.sha256','removed-files.txt'}
removed={'./'+p.lstrip('/') for p in overlay.get('removed_files',[])}
assert all(p.startswith(('./system/share/console/commander/','./system/share/console/themes/Silky/','./system/share/console/themes/TMNT/')) for p in removed)
allowed={'./'+p for p in overlay['files'] if p not in control}|removed
changes={p for p in before.keys()|after.keys() if before.get(p)!=after.get(p)}
assert changes<=allowed,sorted(changes-allowed)
assert all(after['./'+p]==d for p,d in overlay['files'].items() if p not in control)
assert not (removed&after.keys())
report=dict(sealed=True,publication_authorized=False,variant=name,rootfs_sha256=sha(folder/'rootfs.sqfs'),
 rootfs_bytes=(folder/'rootfs.sqfs').stat().st_size,baseline_sha256=expected,changed_files=sorted(changes),
 boot_kernel_partition_scripts_unchanged=True,personal_card_flashed=False)
(folder/'SEALED.json').write_text(json.dumps(report,indent=2)+'\n')
print(name,'SEALED_AND_VERIFIED',report['rootfs_sha256'],flush=True)
