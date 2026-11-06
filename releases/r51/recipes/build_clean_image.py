"""Create an internal clean candidate; this does not authorize redistribution."""
from pathlib import Path
import ctypes,hashlib,json,msvcrt,os,struct
from ctypes import wintypes
work=Path(__file__).resolve().parent;root=work.parents[1];folder=work/'public'
assert json.loads((folder/'PRIVACY_AUDIT.json').read_text(encoding='utf-8'))['privacy_passed']
seal=json.loads((folder/'SEALED.json').read_text(encoding='utf-8'));payload=folder/'rootfs.sqfs'
def sha(file):
 with file.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
assert sha(payload)==seal['rootfs_sha256']
base=root/'work/r48-autogrow/CloveOS-r48-autogrow-blank-card.img'
assert sha(base)=='907c0139c41426411deee669348a11f5c7cfc9d4e14717ccefb080598771b074'
offset=34897920;span=600*1024**2;size=base.stat().st_size
assert size==4*1024**3 and 0<payload.stat().st_size<span and offset+span<1024**3
image=folder/'CloveOS-r51-candidate.img'
with base.open('rb') as incoming,image.open('xb') as output:
 ctl=ctypes.WinDLL('kernel32',use_last_error=True).DeviceIoControl
 ctl.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.LPVOID,wintypes.DWORD,wintypes.LPVOID,wintypes.DWORD,ctypes.POINTER(wintypes.DWORD),wintypes.LPVOID]
 ctl.restype=wintypes.BOOL;returned=wintypes.DWORD()
 assert ctl(msvcrt.get_osfhandle(output.fileno()),0x900c4,None,0,None,0,ctypes.byref(returned),None)
 output.truncate(size)
 # Copy only the unchanged regions; all discarded old rootfs bytes are zero.
 for start,end in ((0,offset),(offset+span,size)):
  incoming.seek(start);output.seek(start)
  while incoming.tell()<end:
   block=incoming.read(min(1024**2,end-incoming.tell()));assert block
   if any(block):output.write(block)
   else:output.seek(len(block),1)
 output.seek(offset)
 with payload.open('rb') as source:
  while block:=source.read(1024**2):output.write(block)
 output.flush();os.fsync(output.fileno())
with base.open('rb') as previous,image.open('rb') as actual:
 for start,end in ((0,offset),(offset+span,size)):
  previous.seek(start);actual.seek(start)
  while previous.tell()<end:
   count=min(1024**2,end-previous.tell());assert previous.read(count)==actual.read(count)
 actual.seek(offset);assert actual.read(payload.stat().st_size)==payload.read_bytes()
 remaining=span-payload.stat().st_size
 while remaining:
  block=actual.read(min(remaining,1024**2));assert block and not any(block);remaining-=len(block)
 actual.seek(1024**3);boot=actual.read(512)
 assert boot[71:82]==b'CLOVE_DATA ' and boot[82:90]==b'FAT32   '
report=dict(built=True,user_publication_authorized=True,release_qualified=False,rights_cleared=False,redistribution_audit_complete=False,
 image_path=image.name,image_bytes=size,image_sha256=sha(image),rootfs_sha256=seal['rootfs_sha256'],
 all_bytes_outside_rootfs_unchanged=True,old_rootfs_padding_zero=True,
 games_bios_artwork_and_personal_settings_absent=True,baseline_clean_audit='r48 SHARE_COPY_VERIFIED.json',
 expansion_and_boot_firmware_unchanged=True)
(folder/'CLEAN_IMAGE.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('INTERNAL_CLEAN_IMAGE_VERIFIED',report['image_sha256'],flush=True)
