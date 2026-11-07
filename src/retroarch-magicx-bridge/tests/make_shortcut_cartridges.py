#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate original test-only GB/GBC SRAM counters; no commercial game data."""
from pathlib import Path
import argparse

def cartridge(color):
    rom=bytearray(32768)
    rom[0x100:0x103]=bytes.fromhex('c35001')
    rom[0x134:0x13d]=b'CLOVE-TGB'
    rom[0x143]=0x80 if color else 0
    rom[0x147]=3
    rom[0x149]=2
    rom[0x14d]=(-sum(rom[0x134:0x14d])-25)&255
    code=bytearray();labels={};fix=[]
    def emit(h):code.extend(bytes.fromhex(h))
    def label(n):labels[n]=len(code)
    def jr(op,n):emit(op+'00');fix.append((len(code)-1,n))
    # Enable SRAM, initialize the two-byte frame counter and sentinel, enable
    # LCD, increment once per VBlank, and wait for the next visible interval.
    emit('3e0aea0000afea00a0ea01a03e5aea02a03e91e040')
    label('vblank');emit('f044fe90');jr('38','vblank')
    emit('fa00a03cea00a0');jr('20','wait');emit('fa01a03cea01a0')
    label('wait');emit('f044fe90');jr('30','wait');jr('18','vblank')
    for pos,n in fix:code[pos]=(labels[n]-pos-1)&255
    rom[0x150:0x150+len(code)]=code
    return bytes(rom)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    for color,ext in [(False,'gb'),(True,'gbc')]:
        (args.output/('counter.'+ext)).write_bytes(cartridge(color))
