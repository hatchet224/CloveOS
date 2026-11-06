from pathlib import Path
import zipfile,json
work=Path(__file__).resolve().parent
manifest={'version':2,'name':'clove-native-demo.zip','items':['Clove Native Demo.sh','clove-native-demo/'],
 'items_opt':None,'attr':{'title':'Clove Native Demo','desc':'Original GPL test fixture.',
 'inst':'Ready to run.','genres':['other'],'porter':['CloveOS'],'image':{},'rtr':True,
 'runtime':[],'reqs':[],'arch':['aarch64'],'exp':False,'min_glibc':'2.29'}}
script='''#!/bin/bash
source /roms/ports/PortMaster/control.txt
get_controls
sleep 60 &
cd /roms/ports/clove-native-demo
./native-demo > /tmp/native-port-result.log 2>&1
'''
with zipfile.ZipFile(work/'test/clove-native-demo.zip','w',zipfile.ZIP_DEFLATED) as z:
 z.writestr('Clove Native Demo.sh',script)
 z.writestr('clove-native-demo/port.json',json.dumps(manifest))
 z.writestr('clove-native-demo/LICENSE',(work.parents[1]/'LICENSE').read_bytes())
 z.write(work/'build/r50_native_demo','clove-native-demo/native-demo')
print('ORIGINAL_TEST_PORT_PACKAGED')
