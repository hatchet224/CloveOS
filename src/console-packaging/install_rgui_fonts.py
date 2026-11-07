#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch/verify licensed, pinned upstream RGUI fonts into a staged root tree.

Font payloads are separate immutable build inputs, not vendored executable or
private image data. The manifest and upstream CC BY 4.0 notice ship with source.
"""
import argparse,hashlib,json,struct,urllib.request,zlib
from pathlib import Path

def decoded_font(raw, item):
    if raw[:8]!=b'#RZIPv\x01#' or len(raw)<24:raise ValueError('Unexpected upstream font container')
    chunk_size,total=struct.unpack_from('<IQ',raw,8)
    if chunk_size!=131072 or total!=item['decoded_bytes']:raise ValueError('Unexpected decoded font size')
    result=bytearray();offset=20
    while offset<len(raw):
        if len(raw)-offset<4:raise ValueError('Truncated font chunk')
        compressed=struct.unpack_from('<I',raw,offset)[0];offset+=4
        if compressed==0 or compressed>len(raw)-offset:raise ValueError('Invalid font chunk bound')
        decoder=zlib.decompressobj()
        part=decoder.decompress(raw[offset:offset+compressed],min(chunk_size,total-len(result))+1)
        if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail or len(part)>total-len(result):
            raise ValueError('Invalid font compression stream')
        result.extend(part);offset+=compressed
    data=bytes(result)
    if len(data)!=total or hashlib.sha256(data).hexdigest()!=item['decoded_sha256']:
        raise ValueError('Decoded font failed its exact expected hash')
    return data

def main():
    p=argparse.ArgumentParser();p.add_argument('--cache',required=True,type=Path)
    p.add_argument('--root',required=True,type=Path);p.add_argument('--offline',action='store_true')
    a=p.parse_args();source=Path(__file__).resolve().parents[2]
    manifest=json.loads((source/'provenance/rgui-fonts.json').read_text())
    root=a.root.resolve();a.cache.mkdir(parents=True,exist_ok=True)
    if root==Path(root.anchor):raise ValueError('Use a staged root tree, not the host filesystem root')
    for item in manifest['files']:
        cached=a.cache/Path(item['path']).name
        if not cached.is_file():
            if a.offline:raise FileNotFoundError(cached)
            req=urllib.request.Request(item['url'],headers={'User-Agent':'CloveOS-source-build'})
            with urllib.request.urlopen(req,timeout=30) as response:data=response.read(item['bytes']+1)
            if len(data)!=item['bytes'] or hashlib.sha256(data).hexdigest()!=item['sha256']:
                raise ValueError('Upstream font input failed its exact pin: '+item['path'])
            cached.write_bytes(data)
        if cached.stat().st_size!=item['bytes'] or hashlib.sha256(cached.read_bytes()).hexdigest()!=item['sha256']:
            raise ValueError('Cached font input failed verification: '+item['path'])
        relative=(manifest['install_directory']+'/'+cached.name if item['path']!='COPYING'
                  else 'system/share/licenses/retroarch-rgui-fonts/COPYING')
        target=root/relative
        for parent in [target,*target.parents]:
            if parent==root.parent:break
            if parent.is_symlink():raise ValueError('Refusing a symlink in staged font target')
        target.parent.mkdir(parents=True,exist_ok=True)
        data=cached.read_bytes()
        if item['path']!='COPYING':data=decoded_font(data,item)
        target.write_bytes(data)
    notices=root/'system/share/licenses/retroarch-rgui-fonts'
    (notices/'NOTICE.txt').write_text(manifest['attribution']+'\n'+manifest['repository']+'\nCommit: '+manifest['commit']+
        '\nLicense: CC-BY-4.0 (COPYING). RZIP containers losslessly decoded at build time; glyph bits unchanged.\n',encoding='utf-8',newline='\n')
    print(json.dumps({'passed':True,'commit':manifest['commit'],'files':manifest['files'],'SD_access':False},indent=2))

if __name__=='__main__':main()
