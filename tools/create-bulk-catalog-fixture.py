#!/usr/bin/env python3
"""Generate original test cartridges and PC-copy layouts in a NEW directory.
No commercial ROM, BIOS or personal data is used. The cartridge skips the
hardware boot-logo requirement and is intended for emulator tests only.
"""
from pathlib import Path
import argparse, hashlib

def counter_rom():
    rom=bytearray(32768)
    rom[0x100:0x103]=bytes.fromhex('c35001')
    rom[0x134:0x13d]=b'CLOVE-TGB';rom[0x147]=3;rom[0x149]=2
    rom[0x14d]=(-sum(rom[0x134:0x14d])-25)&255
    code=bytes.fromhex('3e0aea0000afea00a0ea01a03e5aea02a03e91e040f044fe9038fafa00a03cea00a02007fa01a03cea01a0f044fe9030fa18e2')
    rom[0x150:0x150+len(code)]=code
    assert hashlib.sha256(rom).hexdigest()=='575553aa3488340159c313ef17b7c4599fb6d73365c320f619b61705b518d13f'
    return bytes(rom)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('destination',type=Path)
    p.add_argument('--additions',type=int,default=1024,choices=range(4097),metavar='0..4096')
    a=p.parse_args();a.destination.mkdir(parents=True,exist_ok=False)
    for kind in ['base','additions','mixed']:(a.destination/kind/'Roms/GB').mkdir(parents=True)
    base=counter_rom();(a.destination/'base/Roms/GB/A-counter.gb').write_bytes(base)
    for n in range(a.additions):
        rom=bytearray(base);rom[0x300:0x304]=(n+1).to_bytes(4,'little')
        (a.destination/('additions/Roms/GB/B%04d.gb'%n)).write_bytes(rom)
    mixed=a.destination/'mixed/Roms/GB'
    for n in range(4097):(mixed/('artwork-%d.txt'%n)).write_text('')
    (mixed/('L'*150+'.gb')).write_bytes(base)
    (mixed/'Caf\u00e9.gb').write_bytes(base)
    print('Generated original same-card test layouts; copy only to disposable test media.')

if __name__=='__main__':main()
