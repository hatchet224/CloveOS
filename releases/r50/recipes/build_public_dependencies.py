"""Rebuild static TLS dependencies with anonymous compiler diagnostic paths."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess
work=Path(__file__).resolve().parent;root=work.parents[1]
build=work/'public-dependencies';prefix=build/'prefix'
if not prefix.exists():shutil.copytree(root/'work/build-download/prefix',prefix)
env=dict(os.environ,TEMP=str(root/'work/compiler-tmp'),TMP=str(root/'work/compiler-tmp'),
 ZIG_GLOBAL_CACHE_DIR=str(root/'work/zig-global'),ZIG_LOCAL_CACHE_DIR=str(work/'public-zig-cache'))
cmake=root/'work/host-tools/cmake/data/bin/cmake.exe'
flags='-ffile-prefix-map='+root.as_posix()+'=cloveos-build -fmacro-prefix-map='+root.as_posix()+'=cloveos-build'
commands=[['-S',root/'work/download-source/mbedtls/mbedtls-3.6.7','-B',build/'mbedtls','-G','Ninja',
 '-DCMAKE_MAKE_PROGRAM='+str(root/'work/ninja-tools/bin/ninja.exe'),
 '-DCMAKE_TOOLCHAIN_FILE='+str(root/'work/zig-aarch64.cmake'),'-DCMAKE_BUILD_TYPE=MinSizeRel',
 '-DCMAKE_POLICY_VERSION_MINIMUM=3.5','-DENABLE_PROGRAMS=OFF','-DENABLE_TESTING=OFF',
 '-DUSE_SHARED_MBEDTLS_LIBRARY=OFF','-DGEN_FILES=OFF','-DCMAKE_INSTALL_PREFIX='+str(prefix),'-DCMAKE_C_FLAGS='+flags],
 ['--build',build/'mbedtls','--parallel','3'],['--install',build/'mbedtls']]
with (work/'public-dependencies.log').open('w',encoding='utf-8') as log:
 for args in commands:subprocess.run([str(cmake),*map(str,args)],env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=900)
libs={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (prefix/'lib').glob('libmbed*.a')}
(work/'public-dependencies.json').write_text(json.dumps({'built':True,'compiler_path_maps':True,'libraries':libs},indent=2)+'\n',encoding='utf-8')
print('PUBLIC_TLS_DEPENDENCIES_REBUILT_WITH_COMPILER_PATH_MAPS',flush=True)
