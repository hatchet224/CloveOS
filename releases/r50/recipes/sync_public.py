from pathlib import Path
import shutil,json,hashlib
work=Path(__file__).resolve().parent
old=work.parents[1]/'work/psx-switch-r49/source'
source=work/'source';public=work/'public-source';changed=[]
protected={'console-supervisor/src/online.c','console-supervisor/vendor/scraper_client.h'}
for file in source.rglob('*'):
 if not file.is_file():continue
 rel=file.relative_to(source);name=rel.as_posix()
 baseline=old/rel
 if baseline.exists() and baseline.read_bytes()==file.read_bytes():continue
 if name in protected:raise RuntimeError('Private online source needs an explicit sanitized merge: '+name)
 if name.startswith('console-portmaster/vendor/'):continue
 target=public/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(file,target);changed.append(name)
# The local public build needs upstream source; publication uses pinned fetches.
for name in ['SDL_mixer','SDL2_gfx']:
 dest=public/'console-portmaster/vendor'/name
 if not dest.exists():shutil.copytree(source/'console-portmaster/vendor'/name,dest)
receipts=json.loads((work/'upstream/alpine/RECEIPTS.json').read_text(encoding='utf-8'))
pins={name:json.loads((work/'upstream'/(name+'-pin.json')).read_text(encoding='utf-8')) for name in ['PortMaster-GUI','Device-Info','SDL_mixer','SDL2_gfx']}
pins['alpine_packages']=receipts
pins['python']={'release':'20261026','version':'3.12.16','archive':'cpython-3.12.16+20261026-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz','sha256':'bb4b6392670c5d3f93876996fcaa19858a4a413a0c7720144c18b0d6f1f0db2d','source':'https://github.com/astral-sh/python-build-standalone/releases/tag/20261026'}
for tree in [source,public]:
 (tree/'console-portmaster/dependencies.lock.json').write_text(json.dumps(pins,indent=2)+'\n',encoding='utf-8')
(work/'PUBLIC_SOURCE_CHANGES.json').write_text(json.dumps(changed,indent=2)+'\n',encoding='utf-8')
print('R50_PUBLIC_ORIGINAL_SOURCE_SYNCED',len(changed))
