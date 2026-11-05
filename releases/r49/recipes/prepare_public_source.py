from pathlib import Path
import json,os,shutil,subprocess
work=Path(__file__).resolve().parent;root=work.parents[1]
baseline=root/'work/cloveos-apps/source';private=work/'source';public=work/'public-source'
if not public.exists():shutil.copytree(root/'work/r48-clean-card/source',public)
protected={'console-supervisor/src/online.c','console-supervisor/src/scraper_client.h'}
changes=[]
for file in private.rglob('*'):
 if not file.is_file():continue
 relative=file.relative_to(private);before=baseline/relative
 if before.exists() and file.read_bytes()==before.read_bytes():continue
 assert relative.as_posix() not in protected,'Public privacy variant requires a deliberate merge: '+str(relative)
 target=public/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(file,target);changes.append(relative.as_posix())
for file in protected:
 assert (public/file).read_bytes()==(root/'work/r48-clean-card/source'/file).read_bytes()
(work/'PUBLIC_SOURCE_DELTA.json').write_text(json.dumps(dict(changed=changes,private_scraper_credentials_not_copied=True),indent=2)+'\n')
env=dict(os.environ,TEMP=str(root/'work/compiler-tmp'),TMP=str(root/'work/compiler-tmp'),
 ZIG_GLOBAL_CACHE_DIR=str(root/'work/zig-global'),ZIG_LOCAL_CACHE_DIR=str(work/'public-zig-cache'))
cmake=root/'work/host-tools/cmake/data/bin/cmake.exe';build=work/'public-build'
commands=[['-S',public/'console-supervisor','-B',build,'-G','Ninja',
 '-DCMAKE_MAKE_PROGRAM='+str(root/'work/ninja-tools/bin/ninja.exe'),
 '-DCMAKE_TOOLCHAIN_FILE='+str(root/'work/zig-aarch64.cmake'),
 '-DSDL2_DIR='+str(root/'work/current-sdl-cmake'),'-DCMAKE_BUILD_TYPE=MinSizeRel',
 '-DCONSOLE_ONLINE_PREFIX='+str(root/'work/build-download/prefix'),'-DCMAKE_SKIP_RPATH=ON',
 '-DCMAKE_C_FLAGS=-ffile-prefix-map='+public.as_posix()+'=cloveos-source',
 '-DACTIVITY_BUILD_TESTS=OFF','-DLAUNCHER_BUILD_TESTS=OFF','-DBUILD_TESTING=ON'],
 ['--build',build,'--parallel','4','--target','console-supervisor','console-launcher','GameSwitcher','console-sessiond','console-file-explorer','public_boot_tests','r49_netplay_menu']]
for index,args in enumerate(commands):
 with (work/f'public-build-{index}.log').open('w') as output:
  result=subprocess.run([str(cmake),*map(str,args)],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=900)
 if result.returncode:print((work/f'public-build-{index}.log').read_text()[-5000:])
 result.check_returncode()
print('PUBLIC_SOURCE_PRIVACY_VARIANT_BUILT',len(changes),'source changes; no image published',flush=True)
