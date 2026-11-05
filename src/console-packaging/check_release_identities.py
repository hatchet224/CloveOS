"""Check protected profile/template identities against extracted image hashes.

Use the sha256sum manifest from the extracted, rebuilt SquashFS, not workspace
source files. A complete packaged catalog execution is a separate release gate.
"""
from pathlib import Path
import argparse,hashlib,json

IDENTITY=('core','config','bios','retroarch','disc','policy')
def check(config,hashes):
    config=Path(config);templates={}
    expected_ra=hashes['./system/bin/retroarch']
    compatibility=hashes.get('./etc/console/retroarch-compatible.sha256')
    if compatibility is not None:
        record=(expected_ra+'\n').encode('ascii')
        if hashlib.sha256(record).hexdigest()!=compatibility:
            raise ValueError('RetroArch compatibility record does not authenticate the packaged executable')
        expected_ra='2d138567b9a4ac11b2b29fa766b568fee3592b26cba169a1099fa3934b914fe0'
    for p in sorted((config/'core-templates').glob('*.json')):
        packaged=hashes['./etc/console/core-templates/'+p.name]
        if hashlib.sha256(p.read_bytes()).hexdigest()!=packaged:
            raise ValueError('template bytes differ from package: '+p.name)
        j=json.loads(p.read_text());system=j['system']
        if p.stem!=system:raise ValueError('template filename/system differs: '+str(p))
        if j['retroarch']!=expected_ra:
            raise ValueError('template '+system+' RetroArch checkpoint identity differs')
        for key,path in [('core',j['core_path']),('config_file_hash',j['config_path']),('options_file_hash',j['options_path'])]:
            if j[key]!=hashes['.'+path]:raise ValueError('template '+system+' '+key+' differs from packaged bytes')
        if Path(j['core_path']).name=='fbneo_libretro.so':
            frozen=('magicx-frozen-default-config-v2',j['config_file_hash'],j['options_file_hash'],'')
            expected=hashlib.sha256('\0'.join(frozen).encode('ascii')).hexdigest()
            if j['config']!=expected:raise ValueError('template '+system+' has stale FBNeo configuration identity')
        templates[system]=j
    if not templates:raise ValueError('no system templates')
    count=0
    for p in sorted((config/'catalog').glob('*.json')):
        entry=json.loads(p.read_text());profile=json.loads((config/'profiles'/p.name).read_text())
        if p.stem!=profile['game']:raise ValueError('bundled game/profile filename differs')
        t=templates[entry['system']]
        if entry['core_path']!=t['core_path'] or entry['config_path']!=t['config_path']:raise ValueError('bundled core/config path differs')
        for key in IDENTITY:
            if profile[key]!=t[key]:raise ValueError('bundled profile '+p.name+' '+key+' disagrees with '+entry['system']+' template')
        count+=1
    base=json.loads((config/'base-profile.json').read_text())
    if base['retroarch'] not in ('0'*64,expected_ra):raise ValueError('base profile has stale RetroArch identity')
    return dict(passed=True,system_templates=len(templates),bundled_game_profiles=count,packaged_binary_and_config_hashes_checked=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--config',type=Path,required=True);p.add_argument('--hashes',type=Path,required=True);a=p.parse_args()
    hashes={line.split('  ',1)[1]:line.split('  ',1)[0] for line in a.hashes.read_text().splitlines()}
    print(json.dumps(check(a.config,hashes),indent=2))
