#!/usr/bin/env python3
"""Download checksum-pinned public compatibility-core sources, never firmware."""
from pathlib import Path
import hashlib,json,urllib.request

root=Path(__file__).resolve().parent
destination=root/'upstream';destination.mkdir(exist_ok=True)
for row in json.loads((root/'DOWNLOADS.json').read_text()):
    name=row['filename']
    if Path(name).name!=name or not name.endswith('.tar.gz'):
        raise SystemExit('Invalid source archive filename')
    target=destination/name
    if target.exists():
        if hashlib.sha256(target.read_bytes()).hexdigest()!=row['sha256']:
            raise SystemExit(f'Existing source archive differs: {name}')
        print('Verified',name);continue
    temporary=target.with_suffix(target.suffix+'.partial')
    created=False
    try:
        request=urllib.request.Request(row['url'],headers={'User-Agent':'CloveOS-source-builder'})
        digest=hashlib.sha256()
        with urllib.request.urlopen(request,timeout=90) as response,temporary.open('xb') as output:
            created=True
            while data:=response.read(1024*1024):
                digest.update(data);output.write(data)
        if digest.hexdigest()!=row['sha256']:
            raise SystemExit(f'Source checksum mismatch: {name}')
        temporary.replace(target)
        created=False
    except BaseException:
        if created:temporary.unlink(missing_ok=True)
        raise
    print('Fetched and verified',name)
