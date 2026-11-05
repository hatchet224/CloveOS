from pathlib import Path
import os
import shutil
import subprocess

work = Path(__file__).resolve().parent
root = work.parents[1]
ra = root / 'work/build-current-retroarch'
env = dict(os.environ, TEMP=str(root/'work/compiler-tmp'), TMP=str(root/'work/compiler-tmp'),
           ZIG_GLOBAL_CACHE_DIR=str(root/'work/zig-global'), ZIG_LOCAL_CACHE_DIR=str(work/'zig-cache'))
cmake = root/'work/host-tools/cmake/data/bin/cmake.exe'
def run(name, args, timeout=900):
    with (work/(name+'.log')).open('w') as log:
        result = subprocess.run(list(map(str,args)), env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    print(name, result.returncode, flush=True)
    if result.returncode: print((work/(name+'.log')).read_text()[-6000:], flush=True)
    result.check_returncode()
run('configure', [cmake,'-S',work/'source/console-supervisor','-B',work/'build','-DCMAKE_SKIP_RPATH=ON'])
run('production-build', [cmake,'--build',work/'build','--parallel','4','--target',
    'console-supervisor','console-launcher','GameSwitcher','console-catalog','console-sessiond','console-file-explorer',
    'five_apps_backend_tests','five_apps_controller_tests','r49_readiness_race','r49_failed_launch_recovery',
    'r49_managed_control','r49_managed_selection'])
for path in (work/'retroarch-patches').rglob('*.c'):
    shutil.copyfile(path, ra/path.relative_to(work/'retroarch-patches'))
for name in ('magicx_retroarch.c','magicx_retroarch.h','magicx_power.h'):
    backup=work/'retroarch-original/magicx'/name
    backup.parent.mkdir(parents=True,exist_ok=True)
    if not backup.exists() and (ra/'magicx'/name).exists(): shutil.copyfile(ra/'magicx'/name,backup)
    shutil.copyfile(work/'bridge'/name,ra/'magicx'/name)
for name in ('config.h','config.mk'):
    path=ra/name
    backup=work/'retroarch-original'/name
    if not backup.exists():shutil.copyfile(path,backup)
    text=path.read_text()
    if name=='config.h':
        text=text.replace('/* #undef HAVE_NETPLAYDISCOVERY */','#define HAVE_NETPLAYDISCOVERY 1')
        text=text.replace('#define HAVE_NO_NETPLAYDISCOVERY 1','/* #undef HAVE_NO_NETPLAYDISCOVERY */')
    else:
        text=text.replace('HAVE_NETPLAYDISCOVERY = 0','HAVE_NETPLAYDISCOVERY = 1')
        text=text.replace('HAVE_NO_NETPLAYDISCOVERY = 1','HAVE_NO_NETPLAYDISCOVERY = 0')
    if text!=path.read_text():path.write_text(text,newline='\n')
run('retroarch-build', [root/'work/current-build-deps/portable-shell/msys64/usr/bin/bash.exe',
                      root/'work/build-retroarch-hotkeys.sh'])
shutil.copyfile(ra/'retroarch',work/'retroarch')
print('R49_PRODUCTION_BINARIES_BUILT',flush=True)
