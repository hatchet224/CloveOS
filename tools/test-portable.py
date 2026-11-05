#!/usr/bin/env python3
"""Build and run the existing portable session-daemon regression suites."""
from pathlib import Path
import argparse, json, os, shlex, shutil, subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc',default=os.environ.get('CC','cc'))
parser.add_argument('--build-dir',type=Path,default=Path('build/portable-tests'))
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
source=root/'src/console-sessiond'
build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
compiler=shlex.split(args.cc)
if not compiler or not (Path(compiler[0]).is_file() or shutil.which(compiler[0])):
    raise SystemExit('Set CC to a C11 compiler, for example CC="zig cc".')
core=['fsm','power_input','json','sha256','document','metadata','hotkeys']
suites={'sessiond':['fake_bridge','commands'],'standby':[],'library_data':[]}
results=[]
env=os.environ.copy()
for key,folder in [('ZIG_GLOBAL_CACHE_DIR','zig-global'),('ZIG_LOCAL_CACHE_DIR','zig-local')]:
    cache=build/folder;cache.mkdir(exist_ok=True);env.setdefault(key,str(cache))
if os.name=='nt':
    temporary=build/'tmp';temporary.mkdir(exist_ok=True)
    env.update(TMP=str(temporary),TEMP=str(temporary),TMPDIR=str(temporary))
for name,extra in suites.items():
    executable=build/(name+('.exe' if os.name=='nt' else ''))
    command=[*compiler,'-std=c11','-Wall','-Wextra','-Werror','-UNDEBUG',
             '-I'+str(source/'include'),*[str(source/'src'/f'{p}.c') for p in core+extra],
             str(source/'tests'/f'test_{name}.c'),'-o',str(executable)]
    subprocess.run(command,check=True,cwd=root,env=env)
    subprocess.run([str(executable)],check=True,cwd=build)
    results.append({'suite':name,'passed':True})
print(json.dumps({'passed':True,'scope':'portable source tests; no physical device or emulator test','suites':results},indent=2))
