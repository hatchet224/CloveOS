#!/usr/bin/env python3
"""Original emulator-only MBC3 RTC/SRAM counter cartridges for native tests."""
from pathlib import Path
import argparse

def cartridge(color=False):
 rom=bytearray(32768);rom[0x100:0x103]=bytes.fromhex('c35001');rom[0x134:0x13d]=b'CLOVE-RTC';rom[0x143]=0x80 if color else 0;rom[0x147]=0x10;rom[0x149]=2
 rom[0x14d]=(-sum(rom[0x134:0x14d])-25)&255
 code=bytearray();labels={};fix=[]
 def emit(h):code.extend(bytes.fromhex(h))
 def label(n):labels[n]=len(code)
 def jr(op,n):emit(op+'00');fix.append((len(code)-1,n))
 # Enable SRAM/RTC, halt timer and set seconds=37; return to SRAM bank0.
 emit('3e0aea00003e0cea00403e40ea00a03e08ea00403e25ea00a0afea0040')
 emit('afea00a0ea01a03e5aea02a03e91e040');label('vblank');emit('f044fe90');jr('38','vblank')
 emit('fa00a03cea00a0');jr('20','rtc');emit('fa01a03cea01a0');label('rtc')
 # Latch and read actual core RTC registers into SRAM[4] and[5].
 emit('afea00603e01ea00603e08ea0040fa00a047afea004078ea04a0')
 emit('3e0cea0040fa00a0e64047afea004078ea05a0')
 label('wait');emit('f044fe90');jr('30','wait');jr('18','vblank')
 for pos,name in fix:
  offset=labels[name]-pos-1;assert -128<=offset<=127;code[pos]=offset&255
 rom[0x150:0x150+len(code)]=code
 return bytes(rom)

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('destination',type=Path);a=p.parse_args();a.destination.mkdir(parents=True,exist_ok=False)
 for color in [False,True]:(a.destination/('counter.gbc' if color else 'counter.gb')).write_bytes(cartridge(color))
