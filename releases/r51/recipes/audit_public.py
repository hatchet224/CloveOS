from pathlib import Path
import hashlib,importlib.util,json,sys,tarfile
work=Path(__file__).resolve().parent;root=work.parents[1];folder=work/'public'
sys.path.insert(0,str(root/'work/r48-clean-card'))
spec=importlib.util.spec_from_file_location('baseline_privacy_audit',root/'work/r48-clean-card/audit.py')
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
audit.audit_archive(folder/'rootfs-audit.tar','rootfs')
expected={name:digest for digest,name in (line.split('  ',1) for line in (folder/'root-files.sha256').read_text(encoding='utf-8').splitlines())}
assert {name:digest for name,(_,digest) in audit.manifest.items()}==expected
with tarfile.open(folder/'rootfs-audit.tar') as archive:
 read=lambda name:archive.extractfile('./'+name).read()
 manifest=json.loads(read('etc/console/BINARY_MANIFEST.json'))
 for entry in manifest['artifacts']:
  if '.'+entry['path'] in audit.manifest:
   assert audit.manifest['.'+entry['path']]==(entry['bytes'],entry['sha256']),entry['path']
 registry=json.loads(read('system/share/console/onion-4.3.1-1/cores.json'))
 for entry in registry:
  assert hashlib.sha256(read(entry['ARM32'][1:])).hexdigest()==entry['SHA32']
  assert hashlib.sha256(read(entry['ARM64'][1:])).hexdigest()==entry['SHA64']
 retroarch=hashlib.sha256(read('system/bin/retroarch')).hexdigest()
 assert archive.getmember('./etc/console/retroarch-compatible.sha256').mode==0o444
 assert read('etc/console/retroarch-compatible.sha256')==(retroarch+'\n').encode()
 assert json.loads(read('etc/console/base-profile.json'))['retroarch']=='2d138567b9a4ac11b2b29fa766b568fee3592b26cba169a1099fa3934b914fe0'
rootfs_sha256=hashlib.sha256((folder/'rootfs.sqfs').read_bytes()).hexdigest()
assert rootfs_sha256==json.loads((folder/'SEALED.json').read_text(encoding='utf-8'))['rootfs_sha256']
report=dict(rootfs_sha256=rootfs_sha256,privacy_passed=not audit.failures,counts=dict(audit.counts),nested_archives=audit.archive_rows,
 failures=audit.failures,registered_hashes_match=True,no_personal_card_data_used=True,
 redistribution_license_audit_complete=False,user_publication_authorized=True,release_qualified=False,rights_cleared=False)
(folder/'PRIVACY_AUDIT.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('PUBLIC_PRIVACY_AUDIT', 'PASS' if not audit.failures else 'FAIL', 'files',audit.counts['files'],'findings',len(audit.failures),flush=True)
if audit.failures:print(json.dumps(audit.failures,indent=2))
assert not audit.failures
