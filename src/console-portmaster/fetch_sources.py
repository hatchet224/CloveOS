#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the pinned upstream source archives; install SDL sources for CMake."""
from pathlib import Path,PurePosixPath
import argparse,hashlib,json,shutil,stat,urllib.request,zipfile

root=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cache-dir',type=Path,default=root/'source-cache')
parser.add_argument('--source-dir',type=Path,default=root)
parser.add_argument('--offline',action='store_true')
args=parser.parse_args()
cache=args.cache_dir.resolve();base=args.source_dir.resolve()
cache.mkdir(parents=True,exist_ok=True)
def host_compatibility(row,destination):
    if row['name']!='SDL_mixer':return
    # Same host-only CMake change used by the qualified Windows cross build.
    # It skips convenience symlink creation; compiled mixer code is unchanged.
    p=destination/'CMakeLists.txt';text=p.read_text(encoding='utf-8')
    before='if(SDL2MIXER_BUILD_SHARED_LIBS AND (APPLE OR (UNIX AND NOT ANDROID)))\n'
    after='if(SDL2MIXER_BUILD_SHARED_LIBS AND (APPLE OR (UNIX AND NOT ANDROID)) AND NOT CMAKE_HOST_WIN32)\n'
    if text.count(after)==2:return
    if text.count(before)!=2:raise SystemExit('Unexpected SDL_mixer CMake input')
    p.write_text(text.replace(before,after),encoding='utf-8',newline='\n')
for row in json.loads((root/'DEPENDENCIES.json').read_text()):
    archive=cache/(row['name']+'.zip')
    if not archive.exists():
        if args.offline:raise SystemExit('Missing cached archive: '+str(archive))
        request=urllib.request.Request(row['url'],headers={'User-Agent':'CloveOS-source-fetcher'})
        temporary=archive.with_suffix('.download')
        with urllib.request.urlopen(request,timeout=90) as source,temporary.open('xb') as target:
            shutil.copyfileobj(source,target)
        temporary.rename(archive)
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream,'sha256').hexdigest()!=row['sha256']:
            raise SystemExit('Source checksum mismatch: '+row['name'])
    if row['destination'] is None:
        print('Verified archive:',row['name']);continue
    destination=(base/row['destination']).resolve()
    if not destination.is_relative_to(base):raise SystemExit('Destination escaped source root')
    receipt=destination/'.clove-source-sha256'
    if destination.exists():
        if receipt.is_file() and receipt.read_text().strip()==row['sha256']:
            host_compatibility(row,destination)
            print('Already installed:',row['name']);continue
        raise SystemExit('Refusing to overwrite existing source directory: '+str(destination))
    with zipfile.ZipFile(archive) as bundle:
        members=[]
        for entry in bundle.infolist():
            path=PurePosixPath(entry.filename)
            if path.is_absolute() or '..' in path.parts or not path.parts or path.parts[0]!=row['archive_root']:
                raise SystemExit('Unsafe archive member: '+entry.filename)
            if stat.S_ISLNK(entry.external_attr>>16):
                # Unused Apple framework aliases cannot be created on ordinary
                # Windows source checkouts. The complete archive is retained.
                if len(path.parts)>1 and path.parts[1]=='Xcode':continue
                raise SystemExit('Unexpected source symlink: '+entry.filename)
            if len(path.parts)>1:members.append((entry,Path(*path.parts[1:])))
        destination.mkdir(parents=True)
        for entry,relative in members:
            target=destination/relative
            if entry.is_dir():target.mkdir(parents=True,exist_ok=True);continue
            target.parent.mkdir(parents=True,exist_ok=True)
            with bundle.open(entry) as source,target.open('xb') as output:shutil.copyfileobj(source,output)
        receipt.write_text(row['sha256']+'\n')
    host_compatibility(row,destination)
    print('Installed source:',row['name'])
