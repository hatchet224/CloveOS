"""Focused generated-card production shell diagnostic; no production source edits."""
import os,sys,subprocess,json
from pathlib import Path
pm=Path('/roms/ports/PortMaster')
def state(where):
 p=pm/'pugwash.txt'
 print('R51_LOG_STATE',where,'exists',p.exists(),'dir',p.is_dir(),'link',p.is_symlink(),flush=True)
state('before-setup')
assert subprocess.run(['/system/bin/console-portmaster-launch','setup'],timeout=60).returncode==0
state('after-setup')
Path('/tmp/r51-early-audit.py').write_text('''import os,sys,traceback
def audit(event,args):
 if event in ('os.mkdir','os.rename') and 'pugwash' in str(args):
  print('R51_EARLY_AUDIT',event,args,flush=True);traceback.print_stack()
sys.addaudithook(audit)
print('R51_LOG_STATE_PYTHON_START',os.path.isdir('/roms/ports/PortMaster/pugwash.txt'),flush=True)
''')
Path('/tmp/r51-bound-loop.py').write_text('''from pathlib import Path
from pugwash.app import PortMasterGUI
def bounded(self):
 assert self.hm and not self.hm.config['no-check'] and not self.hm.config['offline']
 assert len(self.hm.list_ports())>100 and self.hm.featured_ports()
 for _ in range(3):self.do_loop(no_delay=True)
 Path('/tmp/R51_REAL_ENTRY_PASS').write_text('production shell chain rendered online GUI\\n')
PortMasterGUI.run=bounded
''')
boot=pm/'pugwash';original=boot.read_bytes()
modified=original.replace(b'import hashlib',b"exec(open('/tmp/r51-early-audit.py').read())\nimport hashlib",1)
modified=modified.replace(b'from pugwash.main import main',b"from pugwash.main import main\nexec(open('/tmp/r51-bound-loop.py').read())",1)
boot.write_bytes(modified)
Path('/tmp/r51-trace.sh').write_text('set -x\n')
try:
 env=dict(os.environ,SDL_GAMECONTROLLERCONFIG='',PORTMASTER_CMDS='--no-check',BASH_ENV='/tmp/r51-trace.sh')
 result=subprocess.run(['/system/bin/console-portmaster-launch','manager'],env=env,timeout=420)
 print('R51_SHELL_CHAIN_EXIT',result.returncode,flush=True);state('after-manager')
 assert result.returncode==0 and Path('/tmp/R51_REAL_ENTRY_PASS').is_file()
 Path('/tmp/R51_ONLINE.json').write_text(json.dumps({'passed':True,'focused_production_chain_passed':True,'physical_handheld':False})+'\n')
finally:boot.write_bytes(original)
