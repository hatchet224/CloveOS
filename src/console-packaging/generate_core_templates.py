#!/usr/bin/env python3
"""Pin protected single-file system templates from captured target core options.

Run after generate-core-identities.py: that older helper writes Gambatte-only
options. This helper replaces the global options file with all qualified
families and pins its exact digest. It never reads/writes an SD card or image.
"""
from pathlib import Path
import argparse, hashlib, json, re

SYSTEMS = {
    'GB': ('gambatte', 'gb|gbc', 8*1024*1024),
    'GBC': ('gambatte', 'gb|gbc', 8*1024*1024),
    'GBA': ('mgba', 'gba', 32*1024*1024),
    'FC': ('fceumm', 'nes', 32*1024*1024),
    'SFC': ('snes9x2005_plus', 'sfc|smc', 16*1024*1024),
    'MD': ('picodrive', 'md|gen|bin', 16*1024*1024),
    'MS': ('picodrive', 'sms', 4*1024*1024),
    'GG': ('picodrive', 'gg', 4*1024*1024),
    'ARCADE': ('fbneo', 'zip', 512*1024*1024),
    'FBNEO': ('fbneo', 'zip', 512*1024*1024),
    'NEOGEO': ('fbneo', 'zip', 512*1024*1024),
    'PCE': ('mednafen_pce_fast', 'pce|chd', 0xffffffff),
    'PICO': ('fake08', 'p8|png', 8*1024*1024),
    'N64': ('mupen64plus_next', 'z64|v64|n64', 64*1024*1024),
    'PS': ('pcsx_rearmed', 'iso|chd', 1024*1024*1024),
    'DC': ('flycast', 'chd|cdi', 0xffffffff),
    'PSP': ('ppsspp', 'iso|cso|pbp|elf', 0xffffffff),
}
def sha(data): return hashlib.sha256(data).hexdigest()
def nul(text):
    if '\0' in text: raise ValueError('NUL in identity component')
    return text.encode('utf-8') + b'\0'
def digest_file(path):
    h=hashlib.sha256()
    with path.open('rb') as source:
        while chunk:=source.read(1024*1024): h.update(chunk)
    return h.hexdigest()
def effective_config(config, options):
    h=hashlib.sha256(nul('magicx-effective-config-v1')+nul(digest_file(config)))
    for key in sorted(options,key=lambda k:k.encode('utf-8')):
        h.update(nul(key));h.update(nul(options[key]))
    return h.hexdigest()
def fbneo_config_identity(config_hash, options_hash):
    return sha(nul('magicx-frozen-default-config-v2')+nul(config_hash)+nul(options_hash))
def read_json(path):
    def unique(pairs):
        result={}
        for k,v in pairs:
            if k in result:raise ValueError('Duplicate key: '+k)
            result[k]=v
        return result
    return json.loads(path.read_text(encoding='utf-8-sig'),object_pairs_hook=unique)
def write_json(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8',newline='\n')
def generate(config, options_dir, cores, retroarch, launch, output):
    frozen={}
    for line in config.read_text(encoding='utf-8').splitlines():
        m=re.fullmatch(r'\s*([A-Za-z0-9_]+)\s*=\s*"([^"\r\n]*)"\s*(?:#.*)?',line)
        if m:frozen[m[1]]=m[2]
    for key,value in {'core_options_path':'/etc/console/retroarch-core-options.cfg',
                      'game_specific_options':'false','global_core_options':'true',
                      'system_directory':'/mnt/SDCARD/BIOS','user_language':'0'}.items():
        if frozen.get(key)!=value:raise ValueError('Unqualified config '+key)
    if launch['bios_root']!='/mnt/SDCARD/BIOS' or launch['card_path']!='/mnt/SDCARD':
        raise ValueError('Unexpected qualified card/BIOS root')
    capture=read_json(options_dir/'CAPTURE.json')
    overrides_path=options_dir/'QUALIFIED_OVERRIDES.json'
    overrides=read_json(overrides_path)['cores'] if overrides_path.exists() else {}
    options={};combined={}
    for core in sorted({v[0] for v in SYSTEMS.values()}):
        values=read_json(options_dir/(core+'.json'))
        if not 0<=len(values)<=512 or (not values and core not in ('fake08','fbneo')):raise ValueError('Unbounded options '+core)
        for key,value in values.items():
            prefix = {'snes9x2005_plus':'snes9x_2005','mupen64plus_next':'mupen64plus','flycast':'reicast','mednafen_pce_fast':'pce_fast'}.get(core,core)
            if not re.fullmatch(r'[A-Za-z0-9_-]+',key) or not (key.startswith(prefix+'_') or key.startswith(prefix+'-')):
                raise ValueError('Unexpected option key '+key)
            if not isinstance(value,str) or not value or len(value)>512 or any(ord(c)<32 or ord(c)>126 or c in '\\"' for c in value):
                raise ValueError('Unsupported option value '+key)
            if key in combined:raise ValueError('Option collision '+key)
            combined[key]=value
        digest=digest_file(cores/(core+'_libretro.so'))
        expected=dict(capture['options'][core]['options'])
        override=overrides.get(core)
        if override is not None:
            if override['core_sha256']!=digest:
                raise ValueError('Override captured from a different compiled core: '+core)
            for key,value in override['values'].items():
                if key not in expected or value not in override['allowed_values'].get(key,[]):
                    raise ValueError('Override not advertised by the target core: '+key)
                expected[key]=value
        if expected!=values or capture['options'][core]['core_sha256']!=digest:
            raise ValueError('Captured options belong to a different compiled core: '+core)
        options[core]=values
    option_bytes=''.join(f'{k} = "{combined[k]}"\n' for k in sorted(combined)).encode('ascii')
    ra=digest_file(retroarch);cfg=digest_file(config)
    policy=('magicx-policy-v2;RASTATE1;raw-SRAM-RTC-after-unserialize;no-wallclock-adjust;fixed-single-file-media;'
            f'healthy={launch["healthy_frames"]};timeout={launch["healthy_timeout_ms"]}')
    templates={}
    seeds = read_json(output/'GENERATED_IDENTITIES.json')['records']
    bios_values = {record['identity']['bios'] for record in seeds}
    if len(bios_values) != 1: raise ValueError('Seed profiles disagree on staged BIOS inventory')
    staged_bios = next(iter(bios_values))
    for system,(core,extensions,limit) in SYSTEMS.items():
        templates[system]=dict(schema=1,system=system,extensions=extensions,
            core_path='/system/lib/libretro/'+core+'_libretro.so',config_path='/etc/console/retroarch.cfg',
            config_file_hash=cfg,options_path='/etc/console/retroarch-core-options.cfg',
            options_file_hash=sha(option_bytes),core=digest_file(cores/(core+'_libretro.so')),
            config=(fbneo_config_identity(cfg,sha(option_bytes)) if core=='fbneo' else effective_config(config,options[core])),bios=staged_bios,
            retroarch=ra,disc=sha(nul('magicx-fixed-single-file-media-v2')),policy=sha(policy.encode('ascii')),
            max_rom_bytes=limit)
    # All inputs have been validated before any build-output file is replaced.
    output.mkdir(parents=True,exist_ok=True)
    (output/'retroarch-core-options.cfg').write_bytes(option_bytes)
    for system,data in templates.items():write_json(output/'core-templates'/(system+'.json'),data)
    write_json(output/'CORE_TEMPLATE_IDENTITIES.json',dict(schema=1,templates=templates,
        core_option_counts={core:len(v) for core,v in options.items()},
        options_file_sha256=sha(option_bytes),bios_policy='fresh exact bounded BIOS inventory before publication and each new process; existing state identities unchanged',
        fc_policy='ordinary iNES only; VS/PlayChoice/NES2/mapper105 excluded',
        ps_auto_enrollment=True,endpoint_hello_verification_required=True))
    return templates
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('config','options-dir','cores','retroarch','launch','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();t=generate(a.config,a.options_dir,a.cores,a.retroarch,read_json(a.launch),a.output)
    print('Generated protected system templates: '+', '.join(t))
