"""Reuse the previously qualified original-kernel SquashFS/hash probe."""
from pathlib import Path
import sys,json,hashlib
root=Path(__file__).resolve().parents[2]
folder=Path(__file__).resolve().parent/('public' if '--public' in sys.argv else 'private')
template=(root/'work/cloveos-r37/test_vendor_kernel.py').read_text()
old="r=Path.cwd();w=r/'work/cloveos-r37'; o=r/'outputs/cloveos-r37'; image=o/'CloveOS-r37-rootfs.sqfs'"
assert old in template
template=template.replace(old,"r=Path("+repr(root.as_posix())+");w=Path("+repr(folder.as_posix())+");o=w;image=w/'rootfs.sqfs'")
template=template.replace("candidate='--xz' in sys.argv","candidate=True")
template=template.replace("w/'candidate-files.sha'","w/'root-files.sha256'")
template=template.replace('range(12)','range(36)')
if '--public' not in sys.argv:
    runtime_probe='''
  mount -o bind /dev /new/dev
  mount -o bind /proc /new/proc
  mount -o bind /sys /new/sys
  if /bin/busybox chroot /new /bin/bash --version &&
     /bin/busybox chroot /new /bin/busybox env PYTHONDONTWRITEBYTECODE=1 PYSDL2_DLL_PATH=/system/portmaster/lib:/usr/magicx/lib LD_LIBRARY_PATH=/system/portmaster/lib:/usr/magicx/lib:/lib:/usr/lib /system/portmaster/python/bin/python3.12 -c "import sys,ssl,ctypes; sys.path.insert(0,'/system/portmaster/template/exlibs'); import sdl2.sdlmixer,sdl2.sdlttf,sdl2.sdlimage; print('R51 original-kernel Python/SSL/SDL runtime imports pass')"; then
    log R51_RUNTIME_PASS
  else log R51_RUNTIME_FAIL; fi
'''
    template=template.replace('  log MOUNT_PASS_$version','  log MOUNT_PASS_$version'+runtime_probe)
exec(compile(template,str(Path(__file__)), 'exec'))
report['boot_image_sha256']=report['kernel_sha256']
report['kernel_sha256']=hashlib.sha256((root/'work/boot-loop-r19d/vendor-kernel.Image').read_bytes()).hexdigest()
if '--public' not in sys.argv:
    report['native_python_ssl_sdl_bash_runtime_passed']='VENDOR_TEST: R51_RUNTIME_PASS' in text
    report['passed']=report['passed'] and report['native_python_ssl_sdl_bash_runtime_passed']
(folder/'VENDOR_KERNEL_XZ_TEST.json').write_text(json.dumps(report,indent=2)+'\n')
assert report['passed']
