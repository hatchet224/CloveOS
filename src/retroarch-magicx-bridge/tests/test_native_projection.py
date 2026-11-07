#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the production GL2 menu draw/projection code with a GL call recorder.

This verifies the actual uploaded transform and preservation of game matrices;
it substitutes GL calls, not a physical Mali GPU. Run with the pinned patched
RetroArch tree and a host C compiler. No SD card or copyrighted media is used.
"""
import argparse,hashlib,json,re,subprocess
from pathlib import Path

def function(source,name):
    match=re.search(r'(?:bool|static\s+(?:INLINE\s+)?void)\s+'+re.escape(name)+r'\s*\(',source)
    if not match:raise ValueError('Missing production function '+name)
    start=match.start();opening=source.index('{',match.end());depth=1;end=opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

def main():
    p=argparse.ArgumentParser();p.add_argument('--retroarch',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path);p.add_argument('--cc',required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    gl=(a.retroarch/'gfx/drivers/gl2.c').read_text()
    binding=(a.retroarch/'magicx/magicx_retroarch.c').read_text()
    code=r'''
#define _USE_MATH_DEFINES
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gfx/math/matrix_4x4.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define HAVE_MAGICX_BRIDGE 1
#define INLINE inline
#define GL_TEXTURE_2D 1
#define VIDEO_SHADER_STOCK_BLEND 2
#define GL_BLEND 3
#define GL_TRIANGLE_STRIP 4
#define GL2_FLAG_MENU_TEXTURE_FULLSCREEN 1U
typedef float GLfloat;
struct video_ortho {float left,right,bottom,top,znear,zfar;};
struct coordinates {const GLfloat *vertex,*tex_coord,*color;unsigned vertices;};
struct shader {void (*use)(void*,void*,int,bool);void (*set_coords)(void*,struct coordinates*);void (*set_mvp)(void*,const math_matrix_4x4*);};
typedef struct {unsigned rotation,video_width,video_height,menu_texture,flags;
float menu_texture_alpha;math_matrix_4x4 mvp,mvp_no_rot;struct shader *shader;void *shader_data;
struct coordinates coords;struct {int x,y;unsigned width,height;}vp;
const GLfloat *vertex_ptr,*white_color_ptr;struct {const GLfloat *coord;}tex_info;} gl2_t;
static struct video_ortho default_ortho={0,1,0,1,-1,1};
static const GLfloat vertexes_flipped[]={0,1,1,1,0,0,1,0};
static const GLfloat tex_coords[]={0,0,1,0,0,1,1,1};
static bool managed;
static bool magicx_retroarch_requested(void){return managed;}
static math_matrix_4x4 uploaded;static unsigned draws;
static void use(void*a,void*b,int c,bool d){(void)a;(void)b;(void)c;(void)d;}
static void coords(void*a,struct coordinates*b){(void)a;assert(b->vertices==4);}
static void mvp(void*a,const math_matrix_4x4*b){(void)a;uploaded=*b;}
static void glBindTexture(int a,unsigned b){(void)a;(void)b;}
static void glEnable(int a){(void)a;}
static void glDisable(int a){(void)a;}
static void glViewport(int a,int b,unsigned c,unsigned d){(void)a;(void)b;(void)c;(void)d;}
static void glDrawArrays(int a,int b,int c){assert(a==GL_TRIANGLE_STRIP&&b==0&&c==4);++draws;}
'''
    names=[('gl2_set_projection',gl),('magicx_retroarch_portrait_menu',binding),('gl2_clove_font_dimensions',gl),('gl2_clove_font_mvp',gl),('gl2_draw_texture',gl)]
    for name,source in names:code+='\n'+function(source,name)+'\n'
    code+=r'''
int main(void){
#ifdef _WIN32
assert(_putenv("CLOVE_DISPLAY_PANEL=magicx-mini-zero28")==0);
#else
assert(setenv("CLOVE_DISPLAY_PANEL","magicx-mini-zero28",1)==0);
#endif
struct shader shader={use,coords,mvp};gl2_t gl={0};
gl.video_width=480;gl.video_height=640;gl.menu_texture_alpha=1;gl.shader=&shader;
gl.vp.width=317;gl.vp.height=411;
gl.rotation=270;gl2_set_projection(&gl,&default_ortho,true);math_matrix_4x4 expected=gl.mvp;
for(unsigned owner=0;owner<2;++owner)for(unsigned rotation=0;rotation<4;++rotation)
for(unsigned full=0;full<2;++full){
managed=owner!=0;gl.rotation=rotation*90U;gl.flags=full?GL2_FLAG_MENU_TEXTURE_FULLSCREEN:0U;
gl2_set_projection(&gl,&default_ortho,true);math_matrix_4x4 saved=gl.mvp,plain=gl.mvp_no_rot;
unsigned before=draws;gl2_draw_texture(&gl);assert(draws==before+1U);
for(unsigned i=0;i<16;++i)assert(fabsf(uploaded.data[i]-expected.data[i])<0.00001f);
assert(gl.rotation==rotation*90U&&memcmp(&gl.mvp,&saved,sizeof(saved))==0&&memcmp(&gl.mvp_no_rot,&plain,sizeof(plain))==0);
unsigned logical_width,logical_height;gl2_clove_font_dimensions(&gl,&logical_width,&logical_height);
assert(logical_width==411&&logical_height==317);
gl2_clove_font_mvp(&gl);
for(unsigned i=0;i<16;++i)assert(fabsf(uploaded.data[i]-expected.data[i])<0.00001f);
assert(gl.rotation==rotation*90U&&memcmp(&gl.mvp,&saved,sizeof(saved))==0&&memcmp(&gl.mvp_no_rot,&plain,sizeof(plain))==0);
}
/* Refuse to add a portrait transform to a landscape non-board surface. */
managed=false;gl.video_width=640;gl.video_height=480;gl2_set_projection(&gl,&default_ortho,true);
gl2_draw_texture(&gl);assert(memcmp(&uploaded,&gl.mvp_no_rot,sizeof(uploaded))==0);
unsigned logical_width,logical_height;gl2_clove_font_dimensions(&gl,&logical_width,&logical_height);
assert(logical_width==317&&logical_height==411);gl2_clove_font_mvp(&gl);
assert(memcmp(&uploaded,&gl.mvp_no_rot,sizeof(uploaded))==0);
puts("R52_PRODUCTION_GL2_MENU_FIXED_PANEL_ALL_GAME_ROTATIONS_AND_UNCHANGED_GAME_MATRICES_PASS cases=17");return 0;}
'''
    c=a.output/'projection.c';c.write_text(code,encoding='utf-8',newline='\n')
    exe=a.output/('projection.exe' if __import__('os').name=='nt' else 'projection')
    subprocess.run([a.cc,'cc','-std=gnu99','-O2','-UNDEBUG','-I',str(a.retroarch/'libretro-common/include'),str(c),'-o',str(exe)],check=True)
    run=subprocess.run([str(exe.resolve())],check=True,capture_output=True,text=True)
    print(run.stdout.strip())
    (a.output/'RESULT.json').write_text(json.dumps(dict(passed=True,production_GL2_draw_and_projection=True,
        GL_calls_substituted=True,physical_GPU=False,game_rotations=[0,90,180,270],
        managed_and_unmanaged=True,game_matrices_preserved=True,OSD_orientation_and_logical_glyph_dimensions=True,OSD_MVP_function_sha256=hashlib.sha256(function(gl,'gl2_clove_font_mvp').encode()).hexdigest(),draw_function_sha256=hashlib.sha256(function(gl,'gl2_draw_texture').encode()).hexdigest()),indent=2)+'\n')

if __name__=='__main__':main()
