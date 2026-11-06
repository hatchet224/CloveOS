"""Prepare a file-only runtime overlay; never accesses block devices or cards."""
from pathlib import Path
import tarfile,io,json,hashlib,difflib,sys
work=Path(__file__).resolve().parent;public='--public' in sys.argv
root=work.parents[1]
source=work/('public-source' if public else 'source');upstream=root/'work/cloveos-language-r50/upstream'
build=root/'work/cloveos-language-r50'/('public-build' if public else 'build')
pm=upstream/'PortMaster-GUI-9ed29285ecbb0610b7c86a0b77d0d9d1c74efd49/PortMaster'
payload={};links={};modes={}
def put(name,data,mode=0o644):payload[name]=data;modes[name]=mode
def link(name,to):links[name]=to
def unpack(archive,prefix,strip):
 with tarfile.open(archive) as tar:
  for item in tar:
   parts=Path(item.name).parts
   if len(parts)<=strip:continue
   relative=Path(*parts[strip:]).as_posix()
   if '..' in Path(relative).parts:raise ValueError(relative)
   name=prefix+'/'+relative
   if item.isfile():put(name,tar.extractfile(item).read(),item.mode&0o777)
   elif item.issym():link(name,item.linkname)
python=next(upstream.glob('cpython-3.12.*aarch64-unknown-linux-gnu-install_only_stripped.tar.gz'))
assert hashlib.sha256(python.read_bytes()).hexdigest()=='bb4b6392670c5d3f93876996fcaa19858a4a413a0c7720144c18b0d6f1f0db2d'
unpack(python,'system/portmaster/python',1)
for receipt in json.loads((upstream/'alpine/RECEIPTS.json').read_text()):
 archive=upstream/'alpine'/Path(receipt['url']).name
 assert hashlib.sha256(archive.read_bytes()).hexdigest()==receipt['sha256']
 unpack(archive,'system/portmaster/musl',0)
for file in pm.rglob('*'):
 if not file.is_file():continue
 relative=file.relative_to(pm).as_posix()
 # Other platform payloads/architectures are unnecessary on this AArch64 port.
 if any(part.startswith('.') for part in file.relative_to(pm).parts):continue
 if any(x in relative for x in ('.armhf','.x86_64','libs.armhf/','libs.x86_64/')):continue
 if relative in {'screenshot.png','gameinfo.xml','pylibs/resources/NotoSans.tar.xz'}:continue
 if relative.split('/')[0] in {'batocera','knulli','miyoo','muos','retrodeck','spruce','trimui'}:continue
 mode=0o755 if file.suffix in {'.sh','.py'} or (file.read_bytes()[:4]==b'\x7fELF') else 0o644
 put('system/portmaster/template/'+relative,file.read_bytes(),mode)
unpack(pm/'pylibs/resources/NotoSans.tar.xz','system/portmaster/template/pylibs/resources',0)
put('system/portmaster/template/licenses/LICENSE-CloveOS-platform.txt',(work.parents[1]/'LICENSE').read_bytes())
for filename,dest in [('control.txt','control.txt'),('launch_hooks.txt','cloveos/launch_hooks.txt'),('cloveos.py','pylibs/harbourmaster/platform/cloveos.py'),('clove_renderer.py','pylibs/pugwash/clove_renderer.py')]:
 put('system/portmaster/template/'+dest,(source/'console-portmaster'/filename).read_bytes())
patches=[]
def patch(relative,before,after):
 name='system/portmaster/template/'+relative;old=payload[name].decode();assert before in old,relative
 new=old.replace(before,after);payload[name]=new.encode()
 patches.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/PortMaster/'+relative,tofile='b/PortMaster/'+relative))
patch('pylibs/harbourmaster/platform/__init__.py','from .base import (','from .cloveos import PlatformCloveOS\nfrom .base import (')
patch('pylibs/harbourmaster/platform/__init__.py',"HM_PLATFORMS = {","HM_PLATFORMS = {\n    'cloveos': PlatformCloveOS,")
patch('pylibs/pugwash/app.py','renderer = sdl2.ext.Renderer(self.window, flags=sdl2.SDL_RENDERER_ACCELERATED)',
 '''if capabilities['name'] == 'cloveos':
                from pugwash.clove_renderer import CloveRenderer
                renderer = CloveRenderer(self.window)
            else:
                renderer = sdl2.ext.Renderer(self.window, flags=sdl2.SDL_RENDERER_ACCELERATED)''')
patch('utils/pmsplash.txt','source $controlfolder/runtimes/"love_11.5"/love.txt','[ "$CFW_NAME" = CloveOS ] && return 0\nsource $controlfolder/runtimes/"love_11.5"/love.txt')
patch('pugwash','#!/usr/bin/env python3','#!/system/portmaster/python/bin/python3.12')
patch('pugwash',"PORTMASTER_VERSION = '2023-01-07-1007'","PORTMASTER_VERSION = '2026-11-02-0433'")
put('system/portmaster/template/version',b'2026-11-02-0433-clove-r51\n')
patch('pylibs/pugwash/main.py','import sys','import os\nimport sys')
patch('pylibs/pugwash/main.py',"if not harbourmaster.HM_TESTING and not config['no-check']:",
 "if not harbourmaster.HM_TESTING and not config['no-check'] and os.environ.get('CFW_NAME') != 'CloveOS':")
patch('pylibs/harbourmaster/hm_install.py','        logger.info("Installing PortMaster.zip")',
 '''        if self.platform_name == 'cloveos':
            self.callback.message_box(_("PortMaster is updated with CloveOS. Game ports can still be installed and updated here."))
            return 255
        logger.info("Installing PortMaster.zip")''')
(source/'console-portmaster/upstream.patch').write_text(''.join(patches),encoding='utf-8',newline='\n')
for name in ('bash','curl','jq'):
 exe='bin/bash' if name=='bash' else 'usr/bin/'+name
 wrapper='''#!/bin/sh
unset LD_PRELOAD
exec /system/portmaster/musl/lib/ld-musl-aarch64.so.1 --library-path /system/portmaster/musl/lib:/system/portmaster/musl/usr/lib /system/portmaster/musl/'''+exe+' "$@"\n'
 put('system/portmaster/bin/'+name,wrapper.encode(),0o755)
 if name=='bash':put('bin/bash',wrapper.encode(),0o755)
# The native helper self-binds only Roms/ports. No namespaces are available in
# the vendor kernel. The vendor /roms symlink and whole-card noexec stay intact.
cert='system/portmaster/musl/etc/ssl/certs/ca-certificates.crt'
put('system/portmaster/certs/ca-certificates.crt',payload['system/portmaster/template/exlibs/certifi/cacert.pem'])
put('etc/cloveos-release',b'CloveOS r51\n')
put('system/portmaster/libclove-port-display.so',(build/'libclove-port-display.so').read_bytes(),0o755)
gfx=build/'libSDL2_gfx-1.0.so.0'
if gfx.exists():put('system/portmaster/lib/libSDL2_gfx-1.0.so.0',gfx.read_bytes(),0o755)
mixer=next((build/'portmaster-mixer').glob('libSDL2_mixer-2.0.so.0*'))
put('system/portmaster/lib/libSDL2_mixer-2.0.so.0',mixer.read_bytes(),0o755)
tar=work/('public-portmaster-runtime.tar' if public else 'portmaster-runtime.tar')
with tarfile.open(tar,'w') as out:
 for name,data in sorted(payload.items()):
  item=tarfile.TarInfo(name);item.size=len(data);item.mode=modes.get(name,0o644);item.mtime=1793836800
  out.addfile(item,io.BytesIO(data))
 for name,target in sorted(links.items()):
  item=tarfile.TarInfo(name);item.type=tarfile.SYMTYPE;item.linkname=target;item.mode=0o755;item.mtime=1793836800
  out.addfile(item)
report={'files':len(payload),'symlinks':len(links),'bytes':tar.stat().st_size,'sha256':hashlib.sha256(tar.read_bytes()).hexdigest(),
        'gui_commit':'9ed29285ecbb0610b7c86a0b77d0d9d1c74efd49','sd_accessed':False}
(work/('PUBLIC_PORTMASTER_RUNTIME.json' if public else 'PORTMASTER_RUNTIME.json')).write_text(json.dumps(report,indent=2)+'\n');print(report)
