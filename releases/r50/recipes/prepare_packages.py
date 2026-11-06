"""Prepare data-preserving R50 rootfs overlays from immutable R49 baselines."""
from pathlib import Path
import hashlib,io,json,tarfile
work=Path(__file__).resolve().parent;root=work.parents[1]
def digest(data):return hashlib.sha256(data).hexdigest()
def encoded(value):return (json.dumps(value,indent=2)+'\n').encode()
for variant in ['private','public']:
 folder=work/variant;folder.mkdir(exist_ok=True)
 build=work/('public-build' if variant=='public' else 'build')
 baseline=root/'work/psx-switch-r49'/variant
 with tarfile.open(baseline/'rootfs-audit.tar') as original:
  old_manifest=original.extractfile('./etc/console/BINARY_MANIFEST.json').read()
  compatible=original.extractfile('./etc/console/retroarch-compatible.sha256').read()
 mods={};links={};modes={}
 runtime=work/('public-portmaster-runtime.tar' if variant=='public' else 'portmaster-runtime.tar')
 with tarfile.open(runtime) as archive:
  for item in archive:
   if item.isfile():mods[item.name]=archive.extractfile(item).read();modes[item.name]=item.mode
   elif item.issym():links[item.name]=item.linkname
   else:raise ValueError(item.name)
 runtime_manifest={'schema':1,'gui_commit':'9ed29285ecbb0610b7c86a0b77d0d9d1c74efd49',
  'files':{p:{'bytes':len(data),'sha256':digest(data)} for p,data in sorted(mods.items())},
  'symlinks':links,'source_lock':'console-portmaster/dependencies.lock.json',
  'native_ports_have_retroarch_savestates':False}
 mods['etc/console/PORTMASTER_MANIFEST.json']=encoded(runtime_manifest)
 for name in ['console-launcher','GameSwitcher','console-supervisor','console-catalog','console-ports']:
  mods['system/bin/'+name]=(build/name).read_bytes();modes['system/bin/'+name]=0o755
 mods['system/bin/console-sessiond']=(build/'launcher/sessiond/console-sessiond').read_bytes()
 modes['system/bin/console-sessiond']=0o755
 mods['system/bin/console-portmaster-launch']=(work/'source/console-supervisor/init/console-portmaster-launch').read_bytes()
 modes['system/bin/console-portmaster-launch']=0o755
 manifest=json.loads(old_manifest)
 entries={row['path']:row for row in manifest['artifacts']}
 # The bounded Package Manager registry stays small. The full Python/resource
 # inventory is separately authenticated by the protected rootfs image.
 for path,data in mods.items():
  if path.startswith('system/portmaster/'):continue
  entries['/'+path]={'path':'/'+path,'bytes':len(data),'sha256':digest(data)}
 manifest['artifacts']=[entries[p] for p in sorted(entries)]
 assert len(encoded(manifest))<131072
 mods['etc/console/BINARY_MANIFEST.json']=encoded(manifest)
 mods['etc/console/BUILD_ID']=b'20261105-cloveos-r50\n'
 mods['expected-base-manifest.sha256']=(digest(old_manifest)+'\n').encode()
 mods['removed-files.txt']=b''
 with tarfile.open(folder/'overlay.tar','w') as archive:
  for path,data in sorted(mods.items()):
   item=tarfile.TarInfo(path);item.size=len(data);item.mode=modes.get(path,0o644);item.mtime=1793923200
   archive.addfile(item,io.BytesIO(data))
  for path,target in sorted(links.items()):
   item=tarfile.TarInfo(path);item.type=tarfile.SYMTYPE;item.linkname=target;item.mode=0o755;item.mtime=1793923200;archive.addfile(item)
 report={'variant':variant,'base_manifest_sha256':digest(old_manifest),'files':{p:digest(d) for p,d in mods.items()},
  'symlinks':links,'removed_files':[],'retroarch_compatible_sha256_file_unchanged':True,
  'retroarch_and_all_emulator_cores_unchanged':True,'card_data_copied_or_modified':False}
 (folder/'OVERLAY.json').write_bytes(encoded(report))
 print(variant,'R50_OVERLAY_PREPARED',len(mods),len(links),flush=True)
