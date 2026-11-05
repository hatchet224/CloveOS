#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fb.h>
#include <string.h>
static struct fb_fix_screeninfo fixture_fix;
static struct fb_var_screeninfo fixture_var;
static int fixture_fd;
static int fixture_open(const char *path,int flags,...);
static int fixture_ioctl(int fd,unsigned long request,...);
int boot_status_test_main(int argc,char **argv);
#define open fixture_open
#define ioctl fixture_ioctl
#define main boot_status_test_main
#include "../image/console-boot-status.c"
#undef open
#undef ioctl
#undef main
#ifdef NDEBUG
#error Assertions required
#endif
static int fixture_open(const char *path,int flags,...) {
    if(!strcmp(path,SPLASH_PATH))return open(path,flags);
    assert(!strcmp(path,"/dev/fb0"));
    fixture_fd=open("/tmp/splash-fb",flags);return fixture_fd;
}
static int fixture_ioctl(int fd,unsigned long request,...) {
    va_list args;void *target;assert(fd==fixture_fd);va_start(args,request);target=va_arg(args,void*);va_end(args);
    if(request==FBIOGET_FSCREENINFO)memcpy(target,&fixture_fix,sizeof(fixture_fix));
    else {assert(request==FBIOGET_VSCREENINFO);memcpy(target,&fixture_var,sizeof(fixture_var));}return 0;
}
int main(void) {
    SDL_Surface *letterbox=SDL_CreateRGBSurfaceWithFormat(0,640,480,16,SDL_PIXELFORMAT_RGB565);assert(letterbox);
    assert(fill_splash(letterbox)==letterbox); /* all black is valid */
    for(unsigned y=61;y<419;++y)for(unsigned x=0;x<640;++x)
      ((Uint16 *)((unsigned char *)letterbox->pixels+(size_t)y*(size_t)letterbox->pitch))[x]=x<80 || x>560?0xf800:0x001f;
    SDL_Surface *covered=fill_splash(letterbox);assert(covered);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<640;++x)assert(splash_pixel(covered,x,y)==0x001f);
    SDL_FreeSurface(covered);
    SDL_Surface *logo=load_splash();assert(logo);
    for(unsigned y=0;y<480;y+=479){unsigned ink=0;for(unsigned x=0;x<640;++x)if(splash_pixel(logo,x,y)!=0)++ink;assert(ink>600);}
    assert(SDL_SaveBMP(logo,"/tmp/fullscreen-splash.bmp")==0);SDL_FreeSurface(logo);
    puts("SPLASH_COVER_NO_BARS_NO_STRETCH_ACTUAL_ASSET_PASS");
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,640,480,32,SDL_PIXELFORMAT_ARGB8888);
    assert(surface);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<640;++x)
      ((Uint32*)((unsigned char*)surface->pixels+(size_t)y*(size_t)surface->pitch))[x]=SDL_MapRGB(surface->format,(Uint8)x,(Uint8)y,(Uint8)(x+y));
    for(unsigned mode=0;mode<2;++mode) {
      memset(&fixture_var,0,sizeof(fixture_var));memset(&fixture_fix,0,sizeof(fixture_fix));
      fixture_var.xres=mode?640:480;fixture_var.yres=mode?480:640;
      fixture_var.xres_virtual=fixture_var.xres+8;fixture_var.yres_virtual=fixture_var.yres*2;
      fixture_var.yoffset=fixture_var.yres;fixture_var.bits_per_pixel=mode?16:32;
      fixture_var.red=(struct fb_bitfield){mode?11:16,mode?5:8,0};
      fixture_var.green=(struct fb_bitfield){mode?5:8,mode?6:8,0};fixture_var.blue=(struct fb_bitfield){0,mode?5:8,0};
      fixture_var.transp=(struct fb_bitfield){mode?0:24,mode?0:8,0};
      fixture_fix.line_length=fixture_var.xres_virtual*(fixture_var.bits_per_pixel/8);
      fixture_fix.smem_len=fixture_fix.line_length*fixture_var.yres_virtual;
      fixture_fix.type=FB_TYPE_PACKED_PIXELS;fixture_fix.visual=FB_VISUAL_TRUECOLOR;
      unsigned char *data=malloc(fixture_fix.smem_len);assert(data);memset(data,0xa5,fixture_fix.smem_len);
      int fd=open("/tmp/splash-fb",O_CREAT|O_TRUNC|O_RDWR,0600);assert(fd>=0);assert(write(fd,data,fixture_fix.smem_len)==(ssize_t)fixture_fix.smem_len);close(fd);
      assert(splash_framebuffer(surface));fd=open("/tmp/splash-fb",O_RDONLY);assert(read(fd,data,fixture_fix.smem_len)==(ssize_t)fixture_fix.smem_len);close(fd);
      for(unsigned y=0;y<fixture_var.yres_virtual;++y)for(unsigned x=0;x<fixture_var.xres_virtual;++x) {
        unsigned bpp=fixture_var.bits_per_pixel/8;size_t offset=(size_t)y*fixture_fix.line_length+(size_t)x*bpp;
        if(y<fixture_var.yoffset||x>=fixture_var.xres) {for(unsigned b=0;b<bpp;++b)assert(data[offset+b]==0xa5);continue;}
        unsigned sx=mode?x:y-fixture_var.yoffset,sy=mode?y-fixture_var.yoffset:479-x;
        uint32_t expected=mode?((sx&255)>>3)<<11|((sy&255)>>2)<<5|(((sx+sy)&255)>>3):0xff000000U|((sx&255)<<16)|((sy&255)<<8)|((sx+sy)&255),actual=0;
        memcpy(&actual,data+offset,bpp);assert(actual==expected);
      }
      fixture_var.bits_per_pixel=24;assert(!splash_framebuffer(surface));
      fixture_var.bits_per_pixel=mode?16:32;fixture_var.yoffset=fixture_var.yres_virtual;assert(!splash_framebuffer(surface));
      free(data);
    }
    unlink("/tmp/splash-fb");SDL_FreeSurface(surface);puts("SPLASH_FRAMEBUFFER_PASS portrait_rotation=90 landscape=1 rgb565=1 argb8888=1 stride_offsets=1 bounds=1");return 0;
}
