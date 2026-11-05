"""Seal using cached Alpine tools on the existing WSL Linux kernel."""
from pathlib import Path
import hashlib,json,subprocess,sys,uuid
work=Path(__file__).resolve().parent;root=work.parents[1]
name='public' if '--public' in sys.argv else 'private';folder=work/name
baseline=root/('work/r48-autogrow/rootfs.sqfs' if name=='public' else 'outputs/cloveos-apps-r48/CloveOS-r48-rootfs.sqfs')
expected='643a73e7cc15bc9755b9dbf635beaee0758e63e16ad2186e1e5dba846f7e63f1' if name=='public' else '6ca9ece96e83ce7e708d83d958b5c2bcb2a81a753e120dc856755ed2fc60a4b3'
def sha(file):
 with file.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def linux(path):
 path=path.resolve();assert path.drive.upper()=='C:'
 return '/mnt/host/c/'+path.as_posix()[3:]
assert sha(baseline)==expected
args=['wsl.exe','-d','docker-desktop','--exec','/bin/sh',linux(work/'seal_native.sh'),
 '/tmp/cloveos-r49-seal-'+name+'-'+uuid.uuid4().hex,linux(baseline),linux(folder/'overlay.tar'),linux(folder),linux(work/'linux-tools')]
with (folder/'NATIVE_SEAL.log').open('w') as output:
 result=subprocess.run(args,stdout=output,stderr=subprocess.STDOUT,timeout=1200)
result.check_returncode()
def inventory(file):return {p:d for d,p in (line.split('  ',1) for line in file.read_text().splitlines())}
before=inventory(folder/'baseline-files.sha256');after=inventory(folder/'root-files.sha256')
overlay=json.loads((folder/'OVERLAY.json').read_text())
control={'expected-base-manifest.sha256','removed-files.txt'}
allowed={'./'+p for p in overlay['files'] if p not in control}|{'.'+p for p in overlay['removed_files']}
changes={p for p in before.keys()|after.keys() if before.get(p)!=after.get(p)}
assert changes<=allowed,sorted(changes-allowed)
assert all(after['./'+p]==d for p,d in overlay['files'].items() if p not in control)
assert all('.'+p not in after for p in overlay['removed_files'])
report=dict(sealed=True,user_publication_authorized=True,release_qualified=False,rights_cleared=False,
 variant=name,rootfs_sha256=sha(folder/'rootfs.sqfs'),rootfs_bytes=(folder/'rootfs.sqfs').stat().st_size,
 baseline_sha256=expected,changed_files=sorted(changes),boot_kernel_partition_scripts_unchanged=True,
 personal_card_flashed=False,packager='Cached Alpine squashfs-tools 4.6.1 via existing docker-desktop WSL; exact tool receipts in linux-tools/RECEIPTS.json')
(folder/'SEALED.json').write_text(json.dumps(report,indent=2)+'\n')
print(name,'NATIVE_SEALED_AND_VERIFIED',report['rootfs_sha256'],flush=True)
