#!/usr/bin/env python3
"""Extract native save-error notification/return functions; the removed custom menu is not recreated."""
from pathlib import Path
import argparse,hashlib,json
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();s=a.source.read_text();root=Path(__file__).resolve().parent
fragments={}
for signature in ['static void native_notice(', 'static bool ra_quick_menu(',
                  'static void recovery_status(void)\n{','static void recovery_command(',
                  'static void native_error_exit(', 'static bool native_notice_action(',
                  'bool magicx_retroarch_menu_finish_requested(']:
 start=s.index(signature);at=s.index('{',start);depth=0
 for end in range(at,len(s)):
  if s[end]=='{':depth+=1
  elif s[end]=='}':
   depth-=1
   if depth==0:break
 else:raise ValueError(signature)
 fragments[signature]=s[start:end+1]+'\n'
a.output.write_text((root/'recovery_panel_mock_prefix.c').read_text()+''.join(fragments.values())+(root/'recovery_panel_mock_main.c').read_text(),newline='\n')
a.output.with_suffix('.json').write_text(json.dumps(dict(production_source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),function_sha256={k:hashlib.sha256(v.encode()).hexdigest() for k,v in fragments.items()},sdl_rgui_native_callbacks_mocked=True,real_protected_seqpacket_commands=True),indent=2)+'\n',newline='\n')
