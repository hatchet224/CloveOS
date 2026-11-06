from pathlib import Path
import json,shutil,hashlib
w=Path(__file__).resolve().parent;r=w.parents[1];old=r/'work/brightness-wake-r51';changes=[]
for f in (w/'source').rglob('*'):
 if not f.is_file():continue
 rel=f.relative_to(w/'source');before=old/'source'/rel
 if before.is_file() and before.read_bytes()==f.read_bytes():continue
 dst=w/'public-source'/rel;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,dst)
 changes.append({'path':rel.as_posix(),'sha256':hashlib.sha256(f.read_bytes()).hexdigest()})
(w/'SOURCE_CHANGES.json').write_text(json.dumps(changes,indent=2)+'\n',encoding='utf-8')
s=(old/'build.py').read_text(encoding='utf-8')
start=s.index(" 'console-launcher','GameSwitcher'");end=s.index(']]]',start)
s=s[:start]+" 'console-supervisor','console-level','r49_level_runtime','r49_level_router','r51_brightness_controls','launcher_input_tests'"+s[end:]
(w/'build.py').write_text(s,encoding='utf-8')
# Reproduce the old helper's brightness-specific failure using the inverse
# light ordering reported by the user, without editing R51 or claiming optics.
test=(old/'source/console-packaging/tests/test_levels.c').read_text(encoding='utf-8')
test=test.replace('#include "../image/console-level.c"','#include "'+(old/'source/console-packaging/image/console-level.c').as_posix()+'"')
start=test.index('  /* User-facing controls must follow');end=test.index('  audio=mmap',start)
test=test[:start]+'''  mock_brightness=128;enabled=1;
  int before_light=255-mock_brightness;
  assert(brightness(1)==0 && mock_brightness==160);
  assert(255-mock_brightness<before_light);
  assert(brightness_menu_level(160U)>brightness_menu_level(128U));
  puts("R51_POSITIVE_REGISTER_DIMMING_WITH_RISING_METER_REPRO panel_model_from_user_report=1");
  return 0;
}
'''
(w/'test').mkdir(exist_ok=True);(w/'test/r51-repro.c').write_text(test,encoding='utf-8')
status=json.loads((w/'STATUS.json').read_text());status.update(stage='R52 brightness-only adapter and meter correction implemented; targeted builds/tests starting',
 physical_settings_right_dimmed=True,code_changes=[x['path'] for x in changes],logical_sound_mapping_changed=False,
 frontend_brightness_sign_changed=False,saved_raw_brightness_reinterpreted=False,standby_code_changed=False,
 diagnostic_change='Supervisor records volume/brightness candidate EV_KEY codes, MENU state and active process IDs without changing key routing.')
(w/'STATUS.json').write_text(json.dumps(status,indent=2)+'\n')
print('R52_NARROW_SOURCE_PREPARED',len(changes))
