#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build isolated normal-play TGB Dual profiles from the pinned rootfs inputs.

Shared Gambatte configuration, options and profiles are never modified. The
existing bridge hashes the dedicated configuration and the core's one advertised
single-player option, keeping checkpoints separate from other cores and Netplay.
"""
from pathlib import Path
import argparse,hashlib,json
CORE='/system/lib/libretro/onion-4.3.1-1/tgbdual_libretro.so'
CORE_SHA='e2495af9a5be23045fc74981173f06b9856c3182d696651fa026999348134f45'
CONFIG='/etc/console/core-configs/tgbdual-single.cfg'
OPTIONS='/etc/console/core-options/tgbdual-single.cfg'
def sha(data):return hashlib.sha256(data).hexdigest()
def nul(text):return text.encode('ascii')+b'\0'
def generate(root,output):
    assert sha((root/CORE.lstrip('/')).read_bytes())==CORE_SHA,'Unqualified TGB Dual artifact'
    original=(root/'etc/console/retroarch.cfg').read_bytes()
    old=b'core_options_path = "/etc/console/retroarch-core-options.cfg"'
    assert original.count(old)==1
    cfg=original.replace(old,('core_options_path = "'+OPTIONS+'"').encode('ascii'))
    options=b'tgbdual_gblink_enable = "disabled"\n'
    for name,data in [(CONFIG,cfg),(OPTIONS,options)]:
        p=output/name.lstrip('/');p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
    identity=sha(nul('magicx-effective-config-v1')+nul(sha(cfg))+
                 nul('tgbdual_gblink_enable')+nul('disabled'))
    for system in ['GB','GBC']:
        source=json.loads((root/f'etc/console/core-templates/{system}.json').read_text(encoding='utf-8'))
        source.update(core_path=CORE,core=CORE_SHA,config_path=CONFIG,config_file_hash=sha(cfg),
                      options_path=OPTIONS,options_file_hash=sha(options),config=identity)
        p=output/f'etc/console/core-variants/{system}/tgbdual.json';p.parent.mkdir(parents=True,exist_ok=True)
        p.write_text(json.dumps(source,indent=2)+'\n',encoding='utf-8',newline='\n')
    return dict(core_sha256=CORE_SHA,configuration_sha256=sha(cfg),options_sha256=sha(options),
                effective_configuration_identity=identity,link_enabled=False,shared_files_modified=False)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path);a=p.parse_args();print(json.dumps(generate(a.root,a.output),indent=2))
