#!/usr/bin/env python3
"""Run the production fatal-start snapshot before noisy renderer diagnostics."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--shell',default='sh')
a=p.parse_args()
source=(Path(__file__).resolve().parents[1]/'image/console-rc.sh').read_text()
start=source.index('die() {');end=source.index('\n}\n',start)+2
function=source[start:end]
with tempfile.TemporaryDirectory(prefix='clove-startup-error-') as directory:
 root=Path(directory);log=root/'startup.log';error=root/'startup-error.log'
 log.write_text('catalog: directory exceeds bounded entries\n')
 function=function.replace('/run/console',root.as_posix())
 script='PATH=/usr/bin:/bin\n'+function+'\n(die "qualified game catalog could not be prepared") >> "'+log.as_posix()+'" 2>&1; result=$?\n[ "$result" -eq 1 ]\n'
 subprocess.run([a.shell,'-c',script],check=True)
 original=error.read_bytes()
 with log.open('a') as stream:
  stream.write('boot-status renderer initialized\nsh: setterm: not found\n')
 assert b'directory exceeds bounded entries' in original
 assert b'qualified game catalog could not be prepared' in original
 assert b'boot-status' not in original and b'setterm' not in original
 assert error.read_bytes()==original
print('STARTUP_CAUSAL_SNAPSHOT_SURVIVES_RENDERER_OUTPUT_PASS')
