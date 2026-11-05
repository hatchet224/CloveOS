"""Reuse the same Linux filesystem tools as the test VM, without installation."""
from pathlib import Path
import gzip,hashlib,io,json,tarfile
work=Path(__file__).resolve().parent;root=work.parents[1];target=work/'linux-tools'
entries={};sources=[]
for apk in sorted((root/'work/linux-validation').glob('*.apk')):
 with tarfile.open(fileobj=io.BytesIO(gzip.decompress(apk.read_bytes())),mode='r:',ignore_zeros=True) as archive:
  for entry in archive:
   name=entry.name.removeprefix('./')
   selected=name in ('usr/bin/mksquashfs','usr/bin/unsquashfs','bin/busybox') or (
    name.startswith(('lib/','usr/lib/')) and '.so' in Path(name).name and '/' not in name.split('/',1)[1].removeprefix('lib/'))
   if not selected:continue
   assert not name.startswith('/') and '..' not in Path(name).parts
   if entry.isfile():entries[name]=archive.extractfile(entry).read()
   elif entry.issym():entries[name]=('link',entry.linkname)
   sources.append(dict(archive=apk.name,path=name,archive_sha256=hashlib.sha256(apk.read_bytes()).hexdigest()))
def resolve(name,seen=()):
 assert name not in seen,name
 data=entries[name]
 if isinstance(data,bytes):return data
 link=data[1]
 import posixpath
 next_name=posixpath.normpath(posixpath.join(posixpath.dirname(name),link))
 assert not next_name.startswith(('/','../')),next_name
 return resolve(next_name,seen+(name,))
for name in entries:
 data=resolve(name);file=target/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_bytes(data)
for name in ('usr/bin/mksquashfs','usr/bin/unsquashfs','lib/ld-musl-x86_64.so.1'):assert (target/name).is_file(),name
(target/'RECEIPTS.json').write_text(json.dumps(sources,indent=2)+'\n')
print('EXISTING_VM_LINUX_TOOLS_EXTRACTED',len(entries),flush=True)
