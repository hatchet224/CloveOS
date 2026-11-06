#!/usr/bin/env python3
"""Create synthetic enrollment inputs in a new, isolated test directory."""
from pathlib import Path
import argparse, json

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('destination',type=Path)
args=parser.parse_args()
args.destination.mkdir(parents=True,exist_ok=False)
fixtures={'GB':('gb',b'gb'), 'GBC':('gbc',b'gbc'), 'GBA':('gba',b'gba'),
 'FC':('nes',b'NES\x1a'+b'\x00'*12), 'SFC':('sfc',b'sfc'),
 'MD':('md',b'md'), 'GG':('gg',b'gg'), 'N64':('z64',b'n64'),
 'PS':('iso',b'psx'), 'DC':('cdi',b'dc'), 'PSP':('iso',b'psp'),
 'ARCADE':('zip',b'arcade'), 'NEOGEO':('zip',b'neogeo'), 'PCE':('pce',b'pce')}
for system,(extension,header) in fixtures.items():
    folder=args.destination/'Roms'/system
    folder.mkdir(parents=True)
    data=bytearray(1024)
    data[:len(header)]=header
    if system=='MD': data[256:260]=b'SEGA'
    (folder/f'Generated {system} fixture.{extension}').write_bytes(data)
    emulator=args.destination/'Emu'/system
    emulator.mkdir(parents=True)
    (emulator/'config.json').write_text(json.dumps({'label':system,
        'rompath':'../../Roms/'+system,'extlist':extension}),encoding='utf-8')
(args.destination/'BIOS').mkdir()
print('Created 14 synthetic catalog inputs. These are not playable games.')
