from pathlib import Path
import hashlib,io,json,re,shutil,tarfile,getpass
work=Path(__file__).resolve().parent;root=work.parents[1]
net=work/'onion-netplay'
account=getpass.getuser().encode('utf-8')
account_pattern=re.escape(account)
scrubbed_account=(b'build'+b'_'*len(account))[:len(account)]
def sha(data):return hashlib.sha256(data).hexdigest()
def as_json(value):return (json.dumps(value,indent=2)+'\n').encode()
canonical='2d138567b9a4ac11b2b29fa766b568fee3592b26cba169a1099fa3934b914fe0'
private_manifest=(root/'work/cloveos-apps/BINARY_MANIFEST.json').read_bytes()
with tarfile.open(root/'work/r48-autogrow/rootfs-audit.tar') as archive:
 commander_resources=['/'+entry.name.removeprefix('./') for entry in archive
  if entry.isfile() and entry.name.startswith('./system/share/console/commander/')]
 public_theme_resources=['/'+entry.name.removeprefix('./') for entry in archive
  if entry.isfile() and entry.name.startswith(('./system/share/console/themes/Silky/','./system/share/console/themes/TMNT/'))]
 public_ui=json.load(archive.extractfile('./etc/console/ui.json'))
 public_files={entry.name.removeprefix('./'):archive.extractfile(entry).read() for entry in archive
  if entry.isfile() and (entry.name.startswith(('./etc/console/core-templates/','./etc/console/core-variants/')) or
  entry.name in ('./etc/console/base-profile.json','./etc/console/CORE_TEMPLATE_IDENTITIES.json','./etc/console/BINARY_MANIFEST.json'))}
for public in (False,True):
 name='public' if public else 'private';folder=work/name;folder.mkdir(exist_ok=True)
 build=work/('public-build' if public else 'build')
 originals=public_files['etc/console/BINARY_MANIFEST.json'] if public else private_manifest
 mods={file.relative_to(net/'staging').as_posix():file.read_bytes() for file in (net/'staging').rglob('*') if file.is_file()}
 for program in ('console-launcher','GameSwitcher','console-supervisor','console-standby','console-level'):
  mods['system/bin/'+program]=(build/program).read_bytes()
 mods['system/bin/console-sessiond']=(build/'launcher/sessiond/console-sessiond').read_bytes()
 mods['system/bin/console-file-explorer']=(build/'file-explorer/console-file-explorer').read_bytes()
 mods['system/bin/retroarch']=(work/'retroarch').read_bytes()
 changes=[]
 if public:
  public_ui['theme_path']='/system/share/console/themes/Clove'
  mods['etc/console/ui.json']=as_json(public_ui)
  theme=work/'public-source/console-launcher/themes/Clove'
  for file in theme.rglob('*'):
   if file.is_file():mods['system/share/console/themes/Clove/'+file.relative_to(theme).as_posix()]=file.read_bytes()
  for path,data in list(mods.items()):
   if not data.startswith(b'\x7fELF'):continue
   for match in re.finditer(account_pattern,data,re.I):
    assert data[match.start()-1:match.start()] in (b'/',b'\\') and data[match.end():match.end()+1] in (b'/',b'\\'),path
   edited=re.sub(account_pattern,scrubbed_account,data,flags=re.I)
   if edited!=data:changes.append(path);mods[path]=edited
  for path,data in public_files.items():
   if path=='etc/console/BINARY_MANIFEST.json':continue
   value=json.loads(data)
   if path.endswith('CORE_TEMPLATE_IDENTITIES.json'):
    for entry in value['templates'].values():entry['retroarch']=canonical
   else:value['retroarch']=canonical
   mods[path]=as_json(value)
 # Exact current executable hash is mapped to the existing compatible state
 # ABI. This is the pre-existing, root-owned compatibility mechanism, not a
 # wildcard or relaxed arbitrary-binary check.
 mods['etc/console/retroarch-compatible.sha256']=(sha(mods['system/bin/retroarch'])+'\n').encode()
 registry=json.loads(mods['system/share/console/onion-4.3.1-1/cores.json'])
 for row in registry:
  row['SHA32']=sha(mods[row['ARM32'][1:]]);row['SHA64']=sha(mods[row['ARM64'][1:]])
 mods['system/share/console/onion-4.3.1-1/cores.json']=as_json(registry)
 manifest=json.loads(originals);entries={entry['path']:entry for entry in manifest['artifacts']}
 # Original GPL explorer has no dependency on the older commander artwork/fonts.
 removed=sorted(set(commander_resources)|{path for path in entries if path.startswith('/system/share/console/commander/')})
 if public:removed=sorted(set(removed)|set(public_theme_resources))
 for path in removed:entries.pop(path,None)
 for path,data in mods.items():entries['/'+path]=dict(path='/'+path,bytes=len(data),sha256=sha(data))
 manifest['artifacts']=[entries[path] for path in sorted(entries)]
 mods['etc/console/BINARY_MANIFEST.json']=as_json(manifest)
 mods['etc/console/BUILD_ID']=b'20261105-cloveos-r49\n'
 mods['expected-base-manifest.sha256']=(sha(originals)+'\n').encode()
 mods['removed-files.txt']=(''.join(path.lstrip('/')+'\n' for path in sorted(removed))).encode()
 with tarfile.open(folder/'overlay.tar','w') as archive:
  for path,data in mods.items():
   info=tarfile.TarInfo(path);info.size=len(data)
   info.mode=0o444 if path=='etc/console/retroarch-compatible.sha256' else (0o755 if path.startswith('system/bin/') or data.startswith(b'\x7fELF') else 0o644)
   archive.addfile(info,io.BytesIO(data))
 (folder/'OVERLAY.json').write_bytes(as_json(dict(publication_authorized=False,personal_path_scrubs=changes,
  retroarch_sha256=sha(mods['system/bin/retroarch']),base_manifest_sha256=sha(originals),
  removed_files=removed,files={path:sha(data) for path,data in mods.items()})))
 print(name,'OVERLAY_PREPARED',len(mods),'files',flush=True)
