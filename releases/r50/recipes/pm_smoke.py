import os,sys,ctypes
from pathlib import Path
pm=Path('/roms/ports/PortMaster')
sys.path[:0]=[str(pm/'exlibs'),str(pm/'pylibs')]
os.environ.update(CFW_NAME='CloveOS',CFW_VERSION='r50',CFW_GLIBC='2.29',
 DEVICE_NAME='MagicX Mini Zero 28',DEVICE_SLUG='mini-zero28',DEVICE_CPU='Allwinner-A133',
 DEVICE_CAPABILITIES='aarch64 640x480',DEVICE_ARCH='aarch64',DEVICE_RAM_MB='1024',
 DISPLAY_WIDTH='640',DISPLAY_HEIGHT='480',ANALOG_STICKS='2',
 HM_TOOLS_DIR='/roms/ports',HM_PORTS_DIR='/roms/ports',HM_SCRIPTS_DIR='/roms/ports',
 PYSDL2_DLL_PATH='/system/portmaster/lib:/usr/magicx/lib',HOME=str(pm/'home'),LANG='es_ES.UTF-8')
import sdl2,sdl2.ext
import harbourmaster
info=harbourmaster.device_info()
assert info['name']=='cloveos' and info['resolution']==(640,480)
print('R50_PORTMASTER_HARDWARE_PROFILE_PASS',info,flush=True)
from pugwash import lang
from pugwash.app import PortMasterGUI
from pugwash.scenes import MainMenuScene
gui=PortMasterGUI()
print('R50_PORTMASTER_GUI_CREATED',flush=True)
gui.hm=harbourmaster.HarbourMaster({'offline':True,'no-check':True})
print('R50_PORTMASTER_HARBOUR_CREATED',gui.hm.platform_name,flush=True)
assert gui.hm.platform_name=='cloveos'
assert gui.hm.install_port('/tmp/clove-native-demo.zip')==0
gui.hm.load_ports()
assert any('Clove Native Demo.sh' in value['items'] for value in gui.hm.installed_ports.values())
print('R50_PORTMASTER_ACTUAL_INSTALL_LIST_PASS',list(gui.hm.installed_ports),flush=True)
gui.scenes=[('root',[MainMenuScene(gui)])]
for i in range(3):gui.do_loop(no_delay=True)
menu=gui.scenes[0][1][0]
before=menu.tags['option_list'].selected_option()
for state in (sdl2.SDL_CONTROLLERBUTTONDOWN,sdl2.SDL_CONTROLLERBUTTONUP):
 event=sdl2.SDL_Event();event.type=state;event.cbutton.type=state
 event.cbutton.button=sdl2.SDL_CONTROLLER_BUTTON_DPAD_DOWN
 event.cbutton.state=1 if state==sdl2.SDL_CONTROLLERBUTTONDOWN else 0
 assert sdl2.SDL_PushEvent(ctypes.byref(event))==1
 gui.do_loop(no_delay=True)
assert menu.tags['option_list'].selected_option()!=before
print('R50_PORTMASTER_REAL_GUI_CONTROLLER_NAVIGATION_PASS',flush=True)
raw=gui.renderer.sdlrenderer
sdl2.SDL_SetRenderTarget(raw,gui.renderer.clove_target)
w,h=ctypes.c_int(640),ctypes.c_int(480)
image=sdl2.SDL_CreateRGBSurfaceWithFormat(0,w.value,h.value,32,sdl2.SDL_PIXELFORMAT_ARGB8888)
assert image
assert sdl2.SDL_RenderReadPixels(raw,None,sdl2.SDL_PIXELFORMAT_ARGB8888,image.contents.pixels,image.contents.pitch)==0
assert sdl2.SDL_SaveBMP(image,b'/tmp/portmaster-home.bmp')==0
sdl2.SDL_FreeSurface(image)
gui.quit()
print('R50_PORTMASTER_REAL_AARCH64_GUI_RENDER_PASS',lang.CURRENT_LANG,flush=True)
# Exercise the exact GUI presenter against the handheld's portrait dimensions.
from pugwash.clove_renderer import CloveRenderer
assert sdl2.SDL_Init(sdl2.SDL_INIT_VIDEO)==0
window=sdl2.ext.Window('Portrait fixture',size=(480,640))
renderer=CloveRenderer(window)
raw=renderer.sdlrenderer
colors=((255,0,0),(0,255,0),(0,0,255),(255,255,255))
for index,rgb in enumerate(colors):
 sdl2.SDL_SetRenderDrawColor(raw,*rgb,255)
 tile=sdl2.SDL_Rect((index%2)*320,(index//2)*240,320,240)
 assert sdl2.SDL_RenderFillRect(raw,tile)==0
renderer.present();sdl2.SDL_SetRenderTarget(raw,None)
image=sdl2.SDL_CreateRGBSurfaceWithFormat(0,480,640,32,sdl2.SDL_PIXELFORMAT_ARGB8888)
assert sdl2.SDL_RenderReadPixels(raw,None,sdl2.SDL_PIXELFORMAT_ARGB8888,image.contents.pixels,image.contents.pitch)==0
for x,y,rgb in ((360,160,colors[0]),(120,160,colors[2]),(120,480,colors[3]),(360,480,colors[1])):
 pixel=ctypes.c_uint32.from_address(image.contents.pixels+y*image.contents.pitch+x*4).value
 assert ((pixel>>16)&255,(pixel>>8)&255,pixel&255)==rgb
assert sdl2.SDL_SaveBMP(image,b'/tmp/portmaster-portrait.bmp')==0
sdl2.SDL_FreeSurface(image);renderer.destroy();window.close()
print('R50_PORTMASTER_480x640_PORTRAIT_COMPLETE_FRAME_PASS',flush=True)
