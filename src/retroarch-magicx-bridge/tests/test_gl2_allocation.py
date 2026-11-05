"""Compile the actual RA allocation function with strict mock GL calls.
No target GPU execution: catches a lost initialization upload on immutable storage.
The pre-fix allocation helper fails the initialized immutable-storage case.
"""
from pathlib import Path
import argparse, subprocess
r=Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--retroarch-source',type=Path,default=r/'work/build-current-retroarch')
parser.add_argument('--cc',nargs='+',default=[str(r/'work/host-tools/ziglang/zig.exe'),'cc'])
parser.add_argument('--output-dir',type=Path,default=r/'work/r10-n64/gl2-tests')
args=parser.parse_args()
args.output_dir.mkdir(parents=True,exist_ok=True)
p=args.retroarch_source/'gfx/drivers/gl2.c' 
s=p.read_text();a=s.index('static void gl2_load_texture_image(');b=s.index('\nstatic bool gl2_recreate_fbo',a)
fn=s[a:b]
pre=r'''#include <stddef.h>
#include <stdio.h>
typedef unsigned GLenum; typedef int GLint; typedef int GLsizei; typedef void GLvoid;
enum gl_capability_enum {GL_CAPS_TEX_STORAGE_EXT,GL_CAPS_TEX_STORAGE,GL_CAPS_GLES3_SUPPORTED};
#define GL_BGRA_EXT 0x80e1
#define GL_TEXTURE_2D 0xde1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
static int enabled,storage_calls,upload_calls,image_calls; static const void *seen;
static int gl_check_capability(enum gl_capability_enum cap){return cap==GL_CAPS_GLES3_SUPPORTED || enabled;}
static void gl2_size_format(GLint *p){(void)p;}
static void glTexStorage2DEXT(GLenum t, GLint n, GLint f, GLsizei w, GLsizei h){(void)t;(void)n;(void)f;(void)w;(void)h;++storage_calls;}
static void glTexStorage2D(GLenum t, GLint n, GLint f, GLsizei w, GLsizei h){glTexStorage2DEXT(t,n,f,w,h);}
static void glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty,const GLvoid *p){(void)t;(void)l;(void)x;(void)y;(void)w;(void)h;(void)f;(void)ty;++upload_calls;seen=p;}
static void glTexImage2D(GLenum t, GLint l, GLint in, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty,const GLvoid *p){(void)t;(void)l;(void)in;(void)w;(void)h;(void)b;(void)f;(void)ty;++image_calls;seen=p;}
'''
post=r'''
static int check(int immutable, int initialized, int bgra){
 unsigned char pixels[64]={0}; const void *data=initialized?pixels:NULL;
 enabled=immutable;storage_calls=upload_calls=image_calls=0;seen=NULL;
 gl2_load_texture_image(GL_TEXTURE_2D,0,bgra?GL_BGRA_EXT:GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,data);
 if(immutable&&!bgra){
  if(storage_calls!=1||image_calls!=0||upload_calls!=initialized||(initialized&&seen!=data)){fprintf(stderr,"FAIL immutable initialized=%d: storage=%d upload=%d image=%d\n",initialized,storage_calls,upload_calls,image_calls);return 1;}
 }else if(image_calls!=1||storage_calls||upload_calls||(initialized&&seen!=data)){fputs("FAIL mutable initialization\n",stderr);return 1;}
 return 0;
}
int main(void){return check(1,1,0)||check(1,0,0)||check(0,1,0)||check(1,1,1);}
'''
out=args.output_dir/'gl2-allocation-fixture.c';out.write_text(pre+fn+post)
for variant,defines in [('gles2',['-DHAVE_OPENGLES','-DHAVE_OPENGLES2']),('gles3',['-DHAVE_OPENGLES']),('desktop',[])]:
 exe=args.output_dir/f'test-gl2-allocation-{variant}.exe'
 subprocess.run([*args.cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',*defines,str(out),'-o',str(exe)],cwd=r,check=True)
 result=subprocess.run([str(exe)],cwd=r)
 if result.returncode:raise SystemExit(result.returncode)
print('PASS actual gl2_load_texture_image initialized/null immutable and mutable paths, GLES2/GLES3/desktop')
