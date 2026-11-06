"""Actual target-runtime HTTPS catalog/install probe on a generated card only."""
import hashlib,json,os,sys,time,subprocess,shutil
from pathlib import Path
pm=Path('/roms/ports/PortMaster');runtime=Path('/system/portmaster')
sys.path[:0]=[str(pm/'exlibs'),str(pm/'pylibs')]
os.environ.update(CFW_NAME='CloveOS',CFW_VERSION='r51',CFW_GLIBC='2.29',
 DEVICE_NAME='MagicX Mini Zero 28',DEVICE_SLUG='mini-zero28',DEVICE_CPU='Allwinner-A133',
 DEVICE_CAPABILITIES='aarch64 640x480 analogue dpad',DEVICE_ARCH='aarch64',DEVICE_RAM_MB='1024',
 DISPLAY_WIDTH='640',DISPLAY_HEIGHT='480',ANALOG_STICKS='2',
 HM_TOOLS_DIR='/roms/ports',HM_PORTS_DIR='/roms/ports',HM_SCRIPTS_DIR='/roms/ports',
 PYSDL2_DLL_PATH='/system/portmaster/lib:/usr/magicx/lib',HOME=str(pm/'home'),LANG='en_US.UTF-8',
 SSL_CERT_FILE=str(runtime/'certs/ca-certificates.crt'),REQUESTS_CA_BUNDLE=str(runtime/'certs/ca-certificates.crt'))
import requests
real_get=requests.get;calls=[]
def get(url,*args,**kwargs):
 assert kwargs.get('verify',True) is not False
 r=real_get(url,*args,**kwargs)
 calls.append({'url':str(url),'status':r.status_code})
 print('R51_REAL_HTTPS',r.status_code,url,flush=True)
 return r
requests.get=get
import harbourmaster
from pugwash.util import get_ip_address
assert get_ip_address(), 'Actual IP route required'
with harbourmaster.make_temp_directory() as temporary:
 hm=harbourmaster.HarbourMaster({'offline':False,'no-check':False},temp_dir=temporary)
 assert hm.platform_name=='cloveos' and not hm.config['offline']
 ports=hm.list_ports();featured=hm.featured_ports()
 assert len(ports)>100, len(ports)
 assert featured, 'Real Featured response must be usable'
 print('R51_REAL_CATALOG_AND_FEATURED_PASS',len(ports),len(featured),flush=True)
 info=hm.port_info('wordlesdl.zip');assert info and info['attr']['rtr']
 assert hm.install_port('wordlesdl.zip')==0
 hm.load_ports();assert (Path('/roms/ports')/'Wordle SDL.sh').is_file()
 assert Path('/roms/ports/wordlesdl/wordle').is_file()
 assert any('Wordle SDL.sh' in row['items'] for row in hm.installed_ports.values())
 print('R51_REAL_FREE_PORT_DOWNLOAD_INSTALL_PASS wordlesdl.zip',flush=True)
 native_env=dict(os.environ,SDL_RENDER_DRIVER='software',
  LD_PRELOAD='/tmp/online-game-probe.so:/system/portmaster/libclove-port-display.so')
 # This observer probes the downloaded binary independently of noexec FAT.
 # The production executable bind mount/launcher is tested separately.
 shutil.copyfile('/roms/ports/wordlesdl/wordle','/tmp/downloaded-wordle')
 os.chmod('/tmp/downloaded-wordle',0o700)
 native=subprocess.run(['/tmp/downloaded-wordle'],cwd='/roms/ports/wordlesdl',env=native_env,timeout=90)
 assert native.returncode==0 and Path('/tmp/R51_WORDLE_RENDER_PASS').is_file()
 print('R51_REAL_DOWNLOADED_NATIVE_WORDLE_RENDER_EXIT_PASS',flush=True)
 # Exercise the complete production shell -> hook -> Python entry chain.
 # A test-only sitecustomize bounds the interactive loop, without changing
 # argument parsing, networking, config loading, catalog or Featured fetches.
 assert subprocess.run(['/system/bin/console-portmaster-launch','setup'],timeout=60).returncode==0
 observe=Path('/tmp/r51-entry-observer.py')
 observe.write_text('''from pathlib import Path
import sys,traceback
def audit(event,args):
 if event=='os.mkdir' and 'pugwash.txt' in str(args):
  print('R51_LOG_PATH_MKDIR_AUDIT',args,flush=True);traceback.print_stack()
sys.addaudithook(audit)
from pugwash.app import PortMasterGUI
print('R51_LOG_PATH_BEFORE_ENTRY',Path('/roms/ports/PortMaster/pugwash.txt').exists(),Path('/roms/ports/PortMaster/pugwash.txt').is_dir(),flush=True)
def bounded(self):
 assert self.hm and not self.hm.config['no-check'] and not self.hm.config['offline']
 assert len(self.hm.list_ports())>100 and self.hm.featured_ports()
 for _ in range(3):self.do_loop(no_delay=True)
 Path('/tmp/R51_REAL_ENTRY_PASS').write_text('production shell/hook/main online GUI rendered\\n')
PortMasterGUI.run=bounded
''')
 boot=pm/'pugwash';original_boot=boot.read_bytes()
 needle=b'from pugwash.main import main'
 assert needle in original_boot
 boot.write_bytes(original_boot.replace(needle,b"exec(open('/tmp/r51-entry-observer.py').read())\n"+needle))
 try:
  wrapper_env=dict(os.environ,SDL_GAMECONTROLLERCONFIG='',PORTMASTER_CMDS='--no-check',PM_RUN_ONCE='Y')
  assert subprocess.run(['/system/bin/console-portmaster-launch','manager'],env=wrapper_env,timeout=420).returncode==0
 finally:observe.unlink(missing_ok=True);boot.write_bytes(original_boot)
 assert Path('/tmp/R51_REAL_ENTRY_PASS').is_file()
 print('R51_PRODUCTION_SHELL_HOOK_PYTHON_ENTRY_ONLINE_PASS',flush=True)
 report={'passed':True,'network_mocked':False,'tls_verification_enabled':True,'catalog_ports':len(ports),
 'featured_groups':len(featured),'ready_to_run_port_downloaded_and_installed':'wordlesdl.zip',
 'real_gui_with_online_data_rendered':True,'downloaded_native_port_rendered_and_exited':True,
 'production_shell_hook_python_entrypoint_online':True,'requests':calls,
 'ca_bundle_sha256':hashlib.sha256((runtime/'certs/ca-certificates.crt').read_bytes()).hexdigest(),
 'physical_handheld':False}
 Path('/tmp/R51_ONLINE.json').write_text(json.dumps(report,indent=2)+'\n')
 print('R51_REAL_ONLINE_GUI_PASS',flush=True)
