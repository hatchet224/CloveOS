from pathlib import Path
import hashlib,json,subprocess,sys,uuid
work=Path(__file__).resolve().parent;root=work.parents[1]
variant='public' if '--public' in sys.argv else 'private';folder=work/variant
baseline=root/'work/psx-switch-r49'/variant/'rootfs.sqfs'
expected={'private':'8ba030a46b7773fce2945a37db9cf03ba515c90ff3e1cb1fd42fae9c768ef956','public':'0f5c94d209951e3bb2db179a79df70fbeb28ff29d9bc7de2daa0d74f2eafc4b4'}[variant]
def sha(file):
 with file.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def linux(path):
 path=path.resolve();assert path.drive.upper()=='C:'
 return '/mnt/host/c/'+path.as_posix()[3:]
assert sha(baseline)==expected
template=(root/'work/psx-switch-r49/seal_native.sh').read_text(encoding='utf-8')
template=template.replace('cloveos-r49-seal-','cloveos-r50-seal-').replace('20261105-cloveos-r49','20261105-cloveos-r50').replace('R49_NATIVE_SEAL_EXIT','R50_NATIVE_SEAL_EXIT')
(work/'seal_native.sh').write_text(template,encoding='utf-8',newline='\n')
args=['wsl.exe','-d','docker-desktop','--exec','/bin/sh',linux(work/'seal_native.sh'),
 '/tmp/cloveos-r50-seal-'+variant+'-'+uuid.uuid4().hex,linux(baseline),linux(folder/'overlay.tar'),linux(folder),linux(root/'work/psx-switch-r49/linux-tools')]
with (folder/'NATIVE_SEAL.log').open('w',encoding='utf-8') as output:
 result=subprocess.run(args,stdout=output,stderr=subprocess.STDOUT,timeout=1200)
result.check_returncode()
def inventory(file):return {p:d for d,p in (line.split('  ',1) for line in file.read_text(encoding='utf-8').splitlines())}
before=inventory(folder/'baseline-files.sha256');after=inventory(folder/'root-files.sha256')
overlay=json.loads((folder/'OVERLAY.json').read_text(encoding='utf-8'))
controls={'expected-base-manifest.sha256','removed-files.txt'}
allowed={'./'+p for p in overlay['files'] if p not in controls}
changes={p for p in before.keys()|after.keys() if before.get(p)!=after.get(p)}
assert changes<=allowed,sorted(changes-allowed)
assert all(after['./'+p]==d for p,d in overlay['files'].items() if p not in controls)
report={'sealed':True,'variant':variant,'rootfs_sha256':sha(folder/'rootfs.sqfs'),'rootfs_bytes':(folder/'rootfs.sqfs').stat().st_size,
 'baseline_sha256':expected,'changed_files':sorted(changes),'boot_kernel_partition_scripts_unchanged':True,
 'retroarch_and_emulator_cores_unchanged':True,'personal_card_flashed':False,'rights_cleared':False,
 'release_qualified':False,'source_first_review_required':True}
(folder/'SEALED.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(variant,'R50_SEALED_AND_ALL_FILES_VERIFIED',report['rootfs_sha256'],flush=True)
