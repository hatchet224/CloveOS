"""Rebuild the six isolated compatibility variants from reviewed source archives.

Run from the source-review kit on Linux with Python 3.12+, make, patch, and
CC/CXX pointing to the qualified AArch64 compiler wrappers from the base kit.
This creates no card image and downloads nothing.
"""
from pathlib import Path,PurePosixPath
import hashlib,json,os,subprocess,tarfile
kit=Path(__file__).resolve().parent;out=kit/'compatibility-build'
assert not out.exists(),'Use a new empty compatibility-build directory.'
out.mkdir()
for name in ('CC','CXX'):assert os.environ.get(name),name+' must name the qualified target compiler'
def unpack(archive,destination,expected):
 assert hashlib.sha256(archive.read_bytes()).hexdigest()==expected,archive
 destination.mkdir(parents=True,exist_ok=True)
 with tarfile.open(archive) as source:
  members=[]
  for member in source:
   components=PurePosixPath(member.name).parts[1:]
   if not components:continue
   assert '..' not in components and not member.name.startswith('/')
   member.name='/'.join(components);members.append(member)
  source.extractall(destination,members=members,filter='data')
rows=json.loads((kit/'REVIEW.json').read_text())['cores']
for row in rows:
 name=row['name'];directory=out/name
 unpack(kit/'upstream'/(name+'.tar.gz'),directory,row['source']['sha256'])
 subprocess.run(['patch','-p1','--batch','-i',str(kit/'patches'/(name+'.patch'))],cwd=directory,check=True)
 if name=='picodrive':
  for receipt in json.loads((kit/'netplay/PICO_SUBMODULE_RECEIPTS.json').read_text()):
   filename=receipt['repository'].replace('/','-')+'-'+receipt['commit']+'.tar.gz'
   unpack(kit/'upstream'/filename,directory/receipt['path'],receipt['sha256'])
 leaf='Makefile.libretro' if name in ('fceumm','picodrive') else 'Makefile'
 args=['make','-f',leaf,'-j4','platform='+('aarch64' if name=='picodrive' else 'unix'),
       'CC='+os.environ['CC'],'CXX='+os.environ['CXX']]
 if name=='picodrive':args+=['GIT_VERSION=688c90d5','DONT_COMPILE_IN_ZLIB=0']
 if name=='fba-neogeo-2024':args+=['GIT_VERSION=" unknown"']
 subprocess.run(args,cwd=directory,check=True)
print('COMPATIBILITY_VARIANTS_BUILT; preserve each upstream license and qualification limits')
