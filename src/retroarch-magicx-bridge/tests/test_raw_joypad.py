from pathlib import Path
import argparse,re,subprocess,os,json,hashlib
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--zig',type=Path,required=True);p.add_argument('--output-dir',type=Path,required=True);a=p.parse_args();s=a.source.read_text();clean=re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',lambda m:' '*len(m.group()),s)
def function(sig):
 start=s.index(sig);at=clean.index('{',start);depth=0
 for i in range(at,len(clean)):
  if clean[i]=='{':depth+=1
  elif clean[i]=='}':
   depth-=1
   if depth==0:return s[start:i+1]+'\n'
 raise ValueError(sig)
start=s.index('typedef struct _sdl_joypad');stop=s.index('static const char *sdl_joypad_name',start);decl=s[start:stop]
functions={k:function(v) for k,v in [('name','static const char *sdl_joypad_name('),('button','static uint8_t sdl_pad_get_button('),('hat','static uint8_t sdl_pad_get_hat('),('axis','static int16_t sdl_pad_get_axis('),('connect','static void sdl_pad_connect('),('button_state','static int32_t sdl_joypad_button_state('),('button_query','static int32_t sdl_joypad_button('),('axis_state','static int16_t sdl_joypad_axis_state('),('state','static int16_t sdl_joypad_state(')]}
here=Path(__file__).resolve().parent;root=here.parents[2];a.output_dir.mkdir(parents=True,exist_ok=True);fixture=a.output_dir/'raw_joypad_mock.c';fixture.write_text((here/'raw_joypad_mock_prefix.h').read_text()+decl+''.join(functions.values())+(here/'raw_joypad_mock_main.c').read_text(),newline='\n');exe=a.output_dir/'raw_joypad_mock.exe'
e=os.environ.copy();e.update(TMP=str(root/'work/compiler-tmp'),TEMP=str(root/'work/compiler-tmp'),ZIG_GLOBAL_CACHE_DIR=str(root/'work/zig-global'),ZIG_LOCAL_CACHE_DIR=str(root/'work/current-zig-cache'))
subprocess.run([str(a.zig.resolve()),'cc','-target','x86_64-windows-gnu','-std=c11','-O1','-UNDEBUG','-I'+str(root/'work/current-build-deps/sdl/SDL2-2.26.1/include'),'-I'+str(root/'work/build-current-retroarch/libretro-common/include'),str(fixture.resolve()),'-o',str(exe.resolve())],env=e,check=True)
t=subprocess.run([str(exe.resolve())],text=True,capture_output=True,check=True).stdout;print(t,end='');(a.output_dir/'RAW_JOYPAD_TESTS.log').write_text(t,newline='\n');(a.output_dir/'RAW_JOYPAD_TESTS.json').write_text(json.dumps({'passed':True,'exact_pinned_pad_functions_with_mock_sdl':True,'physical_controller_tested':False,'managed_raw_indices':True,'four_raw_axes_both_directions':True,'dpad_and_menu21_state_reads':True,'unmanaged_gamecontroller_unchanged':True,'source_sha256':hashlib.sha256(a.source.read_bytes()).hexdigest(),'function_sha256':{k:hashlib.sha256(v.encode()).hexdigest() for k,v in functions.items()}},indent=2)+'\n',newline='\n')
