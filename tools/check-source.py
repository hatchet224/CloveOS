#!/usr/bin/env python3
"""Check the source publication boundary; this is not a legal certification."""
from pathlib import Path
import json,re,sys

root=Path(__file__).resolve().parents[1]
skip={'.git','build','dist','work','outputs','__pycache__','.cache'}
forbidden={'.img','.sqfs','.zip','.7z','.exe','.dll','.so','.a','.o','.pyc',
           '.gb','.gbc','.gba','.nes','.sfc','.smc','.chd','.iso','.sav','.srm',
           '.bin','.ttf','.ttc','.bmp','.png','.jpg','.jpeg'}
key=re.compile(rb'-----BEGIN (?:OPENSSH |RSA |EC |DSA |ENCRYPTED )?PRIVATE KEY-----\s+[A-Za-z0-9+/]{40,}')
credential=re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[A-Z0-9]{16}|AIza[0-9A-Za-z_-]{30,})\b')
home=re.compile(rb'[A-Za-z]:[/\\]+Users[/\\]+[A-Za-z0-9_.-]+|/home/(?!runner(?:/|\b))[A-Za-z0-9_.-]+')
errors=[];count=0
for p in sorted(root.rglob('*')):
    rel=p.relative_to(root)
    if any(part in skip for part in rel.parts):continue
    if p.is_symlink():errors.append(f'{rel}: symlink requires explicit review');continue
    if not p.is_file():continue
    count+=1;raw=p.read_bytes()
    if p.suffix.lower() in forbidden:errors.append(f'{rel}: excluded artifact type')
    if raw.startswith((b'MZ',b'\x7fELF',b'MComprHD',b'NES\x1a')) or b'\0' in raw[:4096]:errors.append(f'{rel}: binary/game payload')
    if key.search(raw) or credential.search(raw):errors.append(f'{rel}: credential/private key pattern')
    paths_to_check=raw
    if rel.as_posix() in {'src/console-activity/vendor/sqlite3/sqlite3.c','src/console-activity/vendor/sqlite3/sqlite3.h'}:
        # SQLite's upstream API documentation uses this public example URI.
        for example in [b'fred/data.db',b'user1/config.db',b'bill/a.db-journal',b'bill/b.db-journal']:
            paths_to_check=paths_to_check.replace(b'/home/'+example,b'<sqlite-example-uri>')
    if home.search(paths_to_check):errors.append(f'{rel}: local user home path')
    if p.suffix=='.json':
        try:json.loads(raw)
        except (UnicodeDecodeError,json.JSONDecodeError):errors.append(f'{rel}: invalid JSON')
for required in ['LICENSE','LICENSE_SCOPE.md','RELEASE_POLICY.md','BUILDING.md','AGENTS.md']:
    if not (root/required).is_file():errors.append(f'missing {required}')
print(json.dumps({'passed':not errors,'files_checked':count,'findings':errors},indent=2))
sys.exit(bool(errors))
