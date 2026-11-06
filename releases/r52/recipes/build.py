from pathlib import Path
import os,subprocess,sys
work=Path(__file__).resolve().parent;root=work.parents[1]
public='--public' in sys.argv
source=work/('public-source' if public else 'source')
build=work/('public-build' if public else 'build')
env=dict(os.environ,TEMP=str(root/'work/compiler-tmp'),TMP=str(root/'work/compiler-tmp'),
 ZIG_GLOBAL_CACHE_DIR=str(root/'work/zig-global'),ZIG_LOCAL_CACHE_DIR=str(work/('public-zig-cache' if public else 'zig-cache')))
cmake=root/'work/host-tools/cmake/data/bin/cmake.exe'
commands=[['configure',[cmake,'-S',source/'console-supervisor','-B',build,'-G','Ninja',
 '-DCMAKE_TOOLCHAIN_FILE='+str(root/'work/zig-aarch64.cmake'),
 '-DCMAKE_MAKE_PROGRAM='+str(root/'work/ninja-tools/bin/ninja.exe'),
 '-DSDL2_DIR='+str(root/'work/current-sdl-cmake'),
 '-DCONSOLE_ONLINE_PREFIX='+str(root/'work/cloveos-language-r50/public-dependencies/prefix' if public else root/'work/build-download/prefix'),
 '-DCMAKE_BUILD_TYPE=MinSizeRel','-DCMAKE_SKIP_RPATH=ON','-DBUILD_TESTING=ON','-DLAUNCHER_BUILD_TESTS=ON']],
 ['build',[cmake,'--build',build,'--parallel','4','--target',
 'console-supervisor','console-level','r49_level_runtime','r49_level_router','r51_brightness_controls','launcher_input_tests']]]
for name,args in commands:
 if public and name=='configure':
  args+=['-DCMAKE_C_FLAGS=-ffile-prefix-map='+str(root).replace('\\','/')+'=cloveos-build -fmacro-prefix-map='+str(root).replace('\\','/')+'=cloveos-build']
 log=work/(('public-' if public else '')+name+'.log')
 with log.open('w') as out:result=subprocess.run(list(map(str,args)),env=env,stdout=out,stderr=subprocess.STDOUT,timeout=1500)
 print(name,result.returncode,flush=True)
 if result.returncode:print(log.read_text()[-7000:]);result.check_returncode()
