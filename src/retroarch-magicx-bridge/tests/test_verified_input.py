"""Extract exact production input-binding functions into an isolated SDL component fixture."""
from pathlib import Path
import argparse,re,hashlib,json
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'magicx_retroarch.c');a=p.parse_args()
s=a.source.read_text();clean=re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',lambda m:' '*len(m.group()),s)
def function(signature):
 start=s.index(signature);at=clean.index('{',start);depth=0
 for i in range(at,len(clean)):
  if clean[i]=='{':depth+=1
  elif clean[i]=='}':
   depth-=1
   if depth==0:return s[start:i+1]+'\n'
 raise ValueError('Unterminated function '+signature)
fragments={name:function(sig) for name,sig in [('error_text','static bool error_text('),('absolute_open','static int absolute_open('),('apply_verified_input','static bool apply_verified_input(')]}
here=Path(__file__).resolve().parent;a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text((here/'verified_input_mock_prefix.h').read_text()+'\n'+''.join(fragments.values())+'\n'+(here/'verified_input_mock_main.c').read_text(),newline='\n')
a.output.with_suffix('.json').write_text(json.dumps({'production_source_sha256':hashlib.sha256(a.source.read_bytes()).hexdigest(),'exact_function_sha256':{k:hashlib.sha256(v.encode()).hexdigest() for k,v in fragments.items()},'generated_fixture_sha256':hashlib.sha256(a.output.read_bytes()).hexdigest(),'uses_real_protected_files_and_production_frontend_serializer':True,'sdl_discovery_is_mocked':True},indent=2)+'\n',newline='\n')
