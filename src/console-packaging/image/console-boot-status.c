#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "launcher/display.h"
#include "launcher/theme.h"
#include "launcher/view.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_BYTES 8192U
#define PAD_COUNT 16U
#define SPLASH_PATH "/system/share/console/boot-splash.bmp"
struct status_video {
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  SDL_Surface *surface, *converted, *splash;
  Uint32 format;
};
struct pad {
  SDL_Joystick *handle;
  SDL_JoystickID instance;
  bool pressed[256];
};
static struct cl_assets ui_assets;
static bool ui_assets_ready;
static volatile sig_atomic_t stopping;
static void signal_stop(int signal_number) {
  (void)signal_number;
  stopping = 1;
}
static void close_video(struct status_video *v) {
  if (ui_assets_ready) { cl_assets_close(&ui_assets); ui_assets_ready = false; }
  SDL_FreeSurface(v->splash);
  SDL_FreeSurface(v->converted);
  SDL_FreeSurface(v->surface);
  if (v->texture != NULL)
    SDL_DestroyTexture(v->texture);
  if (v->renderer != NULL)
    SDL_DestroyRenderer(v->renderer);
  if (v->window != NULL)
    SDL_DestroyWindow(v->window);
  memset(v, 0, sizeof(*v));
}
static bool open_video(struct status_video *v) {
  static const Uint32 flags[] = {
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC,
      SDL_RENDERER_ACCELERATED, SDL_RENDERER_SOFTWARE};
  static const Uint32 formats[] = {SDL_PIXELFORMAT_RGB565,
                                   SDL_PIXELFORMAT_ARGB8888};
  size_t i;
  int width, height, access;
  SDL_RendererInfo info;
  memset(v, 0, sizeof(*v));
  for (int driver = 0; driver < SDL_GetNumVideoDrivers(); ++driver)
    fprintf(stderr, "boot-status available video driver: %s\n",
            SDL_GetVideoDriver(driver));
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    return false;
  (void)SDL_ShowCursor(SDL_DISABLE);
  (void)SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  /* This is the actual zero28 vendor SDL window mode used by MinUI. Diagnostic
   * software fallback never qualifies the gameplay renderer or controller. */
  v->window = SDL_CreateWindow("Console startup status",
                               SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                               CL_WIDTH, CL_HEIGHT, SDL_WINDOW_SHOWN);
  if (v->window == NULL)
    return false;
  for (i = 0U; i < sizeof(flags) / sizeof(flags[0]); ++i) {
    v->renderer = SDL_CreateRenderer(v->window, -1, flags[i]);
    if (v->renderer != NULL)
      break;
    fprintf(stderr, "boot-status renderer flags=%u unavailable: %s\n", flags[i],
            SDL_GetError());
  }
  if (v->renderer == NULL)
    return false;
  memset(&info, 0, sizeof(info));
  if (SDL_GetRendererInfo(v->renderer, &info) == 0)
    fprintf(stderr, "boot-status video=%s renderer=%s flags=%u\n",
            SDL_GetCurrentVideoDriver(),
            info.name != NULL ? info.name : "unknown", info.flags);
  for (i = 0U; i < sizeof(formats) / sizeof(formats[0]); ++i) {
    v->texture =
        SDL_CreateTexture(v->renderer, formats[i], SDL_TEXTUREACCESS_STREAMING,
                          CL_WIDTH, CL_HEIGHT);
    if (v->texture == NULL)
      continue;
    if (SDL_QueryTexture(v->texture, &v->format, &access, &width, &height) ==
            0 &&
        access == SDL_TEXTUREACCESS_STREAMING && width >= CL_WIDTH &&
        height >= CL_HEIGHT &&
        (v->format == SDL_PIXELFORMAT_RGB565 ||
         v->format == SDL_PIXELFORMAT_ARGB8888))
      break;
    SDL_DestroyTexture(v->texture);
    v->texture = NULL;
  }
  if (v->texture == NULL)
    return false;
  fprintf(stderr, "boot-status texture=%dx%d logical=%dx%d\n", width, height,
          CL_WIDTH, CL_HEIGHT);
  v->surface = SDL_CreateRGBSurfaceWithFormat(0, CL_WIDTH, CL_HEIGHT, 16,
                                              SDL_PIXELFORMAT_RGB565);
  if (v->surface == NULL)
    return false;
  if (v->format != SDL_PIXELFORMAT_RGB565)
    v->converted =
        SDL_CreateRGBSurfaceWithFormat(0, CL_WIDTH, CL_HEIGHT, 32, v->format);
  return v->format == SDL_PIXELFORMAT_RGB565 || v->converted != NULL;
}
static bool present(struct status_video *v) {
  SDL_Rect source = {0, 0, CL_WIDTH, CL_HEIGHT}, target;
  SDL_Surface *upload = v->surface;
  int width, height, rotation = 0;
  double scale, xscale, yscale;
  if (SDL_GetRendererOutputSize(v->renderer, &width, &height) != 0 ||
      width < 1 || height < 1)
    return false;
  if (height > width)
    rotation = 90; /* Reference zero28 portrait scanout convention. */
  if (v->converted != NULL) {
    if (SDL_ConvertPixels(CL_WIDTH, CL_HEIGHT, v->surface->format->format,
                          v->surface->pixels, v->surface->pitch, v->format,
                          v->converted->pixels, v->converted->pitch) != 0)
      return false;
    upload = v->converted;
  }
  xscale = (double)width / (rotation != 0 ? CL_HEIGHT : CL_WIDTH);
  yscale = (double)height / (rotation != 0 ? CL_WIDTH : CL_HEIGHT);
  scale = xscale < yscale ? xscale : yscale;
  target.w = (int)(CL_WIDTH * scale);
  target.h = (int)(CL_HEIGHT * scale);
  target.x = (width - target.w) / 2;
  target.y = (height - target.h) / 2;
  if (SDL_UpdateTexture(v->texture, &source, upload->pixels, upload->pitch) !=
          0 ||
      SDL_SetRenderDrawColor(v->renderer, 0U, 0U, 0U, 255U) != 0 ||
      SDL_RenderClear(v->renderer) != 0 ||
      SDL_RenderCopyEx(v->renderer, v->texture, &source, &target,
                       (double)rotation, NULL, SDL_FLIP_NONE) != 0)
    return false;
  SDL_RenderPresent(v->renderer);
  return true;
}
static Uint32 color(SDL_Surface *surface, uint32_t rgb) {
  return SDL_MapRGB(surface->format, (Uint8)(rgb >> 16U), (Uint8)(rgb >> 8U),
                    (Uint8)rgb);
}
static void read_theme(struct cl_theme *theme) {
  char data[65536];
  ssize_t n;
  int fd = open("/system/share/console/themes/Silky/config.json",
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return;
  n = read(fd, data, sizeof(data));
  if (n > 0 && n < (ssize_t)sizeof(data))
    (void)cl_theme_apply(theme, data, (size_t)n);
  (void)close(fd);
}
static uint32_t readable(uint32_t foreground, uint32_t background) {
  unsigned int fr = (foreground >> 16U) & 255U, fg = (foreground >> 8U) & 255U,
               fb = foreground & 255U;
  unsigned int br = (background >> 16U) & 255U, bg = (background >> 8U) & 255U,
               bb = background & 255U;
  int first = (int)(fr * 299U + fg * 587U + fb * 114U);
  int second = (int)(br * 299U + bg * 587U + bb * 114U);
  if (abs(first - second) < 96000)
    return second > 128000 ? 0x101010U : 0xffffffU;
  return foreground;
}
static void log_tail(const char *path, char lines[8][51]) {
  char data[LOG_BYTES + 1U];
  struct stat st;
  int fd;
  ssize_t n;
  size_t i, index = 0U, column = 0U;
  memset(lines, 0, 8U * 51U);
  if (path == NULL)
    return;
  fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    (void)close(fd);
    return;
  }
  n = pread(fd, data, LOG_BYTES,
            st.st_size > (off_t)LOG_BYTES ? st.st_size - (off_t)LOG_BYTES : 0);
  (void)close(fd);
  if (n <= 0)
    return;
  data[n] = '\0';
  for (i = 0U; i < (size_t)n; ++i) {
    unsigned char c = (unsigned char)data[i];
    if (c == '\r')
      continue;
    if (c == '\n' || column == 50U) {
      if (index == 7U) {
        memmove(lines[0], lines[1], 7U * 51U);
        memset(lines[7], 0, 51U);
      } else
        ++index;
      column = 0U;
      if (c == '\n')
        continue;
    }
    lines[index][column++] = (char)(c >= 32U && c < 127U ? c : '?');
  }
}
static void render(struct status_video *v, const struct cl_theme *t,
                   const char *message, const char *log, bool progress) {
  char lines[8][51], part[49];
  size_t offset = 0U, row = 0U, length = strlen(message), i;
  Uint32 text =
      color(v->surface, readable(t->fonts[CL_FONT_LIST].color, t->background));
  Uint32 panel_text =
      color(v->surface, readable(t->fonts[CL_FONT_LIST].color, t->panel));
  SDL_Rect panel = {16, 194, 608, 204};
  if (progress && v->splash != NULL) {
    for (int y = 0; y < CL_HEIGHT; ++y)
      memcpy((unsigned char *)v->surface->pixels + (size_t)y * (size_t)v->surface->pitch,
             (const unsigned char *)v->splash->pixels + (size_t)y * (size_t)v->splash->pitch,
             CL_WIDTH * sizeof(Uint16));
    return;
  }
  if (ui_assets_ready) {
    struct cl_view view = {.theme=t, .assets=&ui_assets, .battery=-1};
    cl_view_frame(v->surface, &view, "CloveOS");
    cl_view_text(v->surface, &ui_assets, CL_FONT_TITLE, 24, 84,
                 progress ? "Starting..." : "Startup needs attention", t->fonts[CL_FONT_TITLE].color, 592);
    size_t used = 0U;
    for (int y = 132; used < strlen(message) && y < 196; y += 28) {
      char line[49]; size_t count = strlen(message)-used; if (count > 48U) count = 48U;
      memcpy(line,message+used,count);line[count]='\0';used+=count;
      cl_view_text(v->surface,&ui_assets,CL_FONT_LIST,24,y,line,t->fonts[CL_FONT_LIST].color,592);
    }
    if (!progress) {
      cl_round_rect(v->surface,(SDL_Rect){16,208,608,200},8,t->panel);
      log_tail(log, lines);
      for (size_t line = 0U; line < 8U; ++line)
        cl_view_text(v->surface,&ui_assets,CL_FONT_DETAIL,28,218+(int)line*23,lines[line],t->muted,584);
    }
    cl_view_footer(v->surface,&view,progress ? "Please wait..." : "Press and release a button to retry");
    return;
  }
  (void)SDL_FillRect(v->surface, NULL, color(v->surface, t->background));
  cl_text(v->surface, 24, 22, progress ? "MINI ZERO 28" : "CONSOLE STARTUP STATUS", 3U, text, 33U);
  while (offset < length && row < 5U) {
    size_t n = length - offset;
    if (n > 48U)
      n = 48U;
    memcpy(part, message + offset, n);
    part[n] = '\0';
    cl_text(v->surface, 24, (progress ? 210 : 66) + (int)row * 22, part, 2U, text, 48U);
    offset += n;
    ++row;
  }
  if (progress) return;
  (void)SDL_FillRect(v->surface, &panel, color(v->surface, t->panel));
  log_tail(log, lines);
  for (i = 0U; i < 8U; ++i)
    cl_text(v->surface, 24, 206 + (int)i * 23, lines[i], 2U, panel_text, 50U);
  cl_text(v->surface, 24, 420, "PRESS AND RELEASE A GAMEPAD BUTTON", 2U, text,
          49U);
  cl_text(v->surface, 24, 446, "TO RETRY. NO GAME OR SAVE IS CHANGED.", 2U,
          text, 49U);
}
/* The early boot writer uses the existing scanout without creating a second
 * GPU owner, changing the video mode, or leaving a background process running.
 * It closes before the normal bootstrap renderer and themed launcher start. */
static bool splash_framebuffer(SDL_Surface *splash) {
  struct fb_fix_screeninfo fix;
  struct fb_var_screeninfo var;
  unsigned char *pixels;
  int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  bool ok = false;
  if (fd < 0) return false;
  if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
      ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
      fix.type != FB_TYPE_PACKED_PIXELS || fix.visual != FB_VISUAL_TRUECOLOR ||
      !((var.xres == 480U && var.yres == 640U) ||
        (var.xres == 640U && var.yres == 480U)) ||
      (var.bits_per_pixel != 16U && var.bits_per_pixel != 32U) ||
      var.xoffset > var.xres_virtual || var.xres > var.xres_virtual - var.xoffset ||
      var.yoffset > var.yres_virtual || var.yres > var.yres_virtual - var.yoffset ||
      (uint64_t)var.xres_virtual * (var.bits_per_pixel / 8U) > fix.line_length ||
      (uint64_t)fix.line_length * var.yres_virtual > fix.smem_len ||
      fix.smem_len == 0U || fix.smem_len > 64U * 1024U * 1024U ||
      var.red.length > 8U || var.green.length > 8U || var.blue.length > 8U ||
      var.transp.length > 8U || var.red.msb_right || var.green.msb_right ||
      var.blue.msb_right || var.transp.msb_right ||
      var.red.offset + var.red.length > var.bits_per_pixel ||
      var.green.offset + var.green.length > var.bits_per_pixel ||
      var.blue.offset + var.blue.length > var.bits_per_pixel ||
      var.transp.offset + var.transp.length > var.bits_per_pixel)
    goto done;
  pixels = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) goto done;
  for (unsigned int y = 0U; y < CL_HEIGHT; ++y) {
    const unsigned char *row = (const unsigned char *)splash->pixels +
                               (size_t)y * (size_t)splash->pitch;
    for (unsigned int x = 0U; x < CL_WIDTH; ++x) {
      Uint8 red, green, blue;
      Uint32 pixel = 0U;
      unsigned int px = var.xres == 480U ? CL_HEIGHT - 1U - y : x;
      unsigned int py = var.xres == 480U ? x : y;
      uint32_t value;
      size_t offset;
      memcpy(&pixel, row + (size_t)x * splash->format->BytesPerPixel,
             splash->format->BytesPerPixel);
      SDL_GetRGB(pixel, splash->format, &red, &green, &blue);
      value = ((uint32_t)(red >> (8U - var.red.length)) << var.red.offset) |
              ((uint32_t)(green >> (8U - var.green.length)) << var.green.offset) |
              ((uint32_t)(blue >> (8U - var.blue.length)) << var.blue.offset) |
              (((1U << var.transp.length) - 1U) << var.transp.offset);
      offset = (size_t)(py + var.yoffset) * fix.line_length +
               (size_t)(px + var.xoffset) * (var.bits_per_pixel / 8U);
      memcpy(pixels + offset, &value, var.bits_per_pixel / 8U);
    }
  }
  ok = munmap(pixels, fix.smem_len) == 0;
done:
  (void)close(fd);
  return ok;
}
static uint32_t bmp_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
         ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}
static Uint16 splash_pixel(const SDL_Surface *surface, unsigned int x, unsigned int y) {
  return ((const Uint16 *)((const unsigned char *)surface->pixels +
                           (size_t)y * (size_t)surface->pitch))[x];
}
static Uint16 splash_blend(Uint16 a, Uint16 b, Uint16 c, Uint16 d,
                          unsigned int fx, unsigned int fy) {
  Uint16 result = 0U;
  const unsigned int shifts[] = {0U, 5U, 11U}, masks[] = {31U, 63U, 31U};
  for (unsigned int channel = 0U; channel < 3U; ++channel) {
    unsigned int shift = shifts[channel], mask = masks[channel];
    unsigned int upper = ((a >> shift) & mask) * (256U-fx) + ((b >> shift) & mask) * fx;
    unsigned int lower = ((c >> shift) & mask) * (256U-fx) + ((d >> shift) & mask) * fx;
    unsigned int value = (upper * (256U-fy) + lower * fy + 32768U) >> 16U;
    result = (Uint16)(result | (Uint16)(value << shift));
  }
  return result;
}
static SDL_Surface *fill_splash(SDL_Surface *source) {
  /* Earlier assets contained black letterboxing. Treat only fully black
   * outer rows/columns as padding, then use a centred cover crop. Both early
   * framebuffer output and the later SDL splash use this same canvas. */
  unsigned int left=CL_WIDTH, top=CL_HEIGHT, right=0U, bottom=0U;
  for (unsigned int y=0U;y<CL_HEIGHT;++y) for (unsigned int x=0U;x<CL_WIDTH;++x) {
    if (splash_pixel(source,x,y)==0U) continue;
    if(x<left)left=x;if(x>right)right=x;if(y<top)top=y;if(y>bottom)bottom=y;
  }
  if(left>right || top>bottom)return source;
  unsigned int width=right-left+1U,height=bottom-top+1U;
  if(width*CL_HEIGHT>height*CL_WIDTH){unsigned int crop=height*CL_WIDTH/CL_HEIGHT;left+=(width-crop)/2U;width=crop;}
  else {unsigned int crop=width*CL_HEIGHT/CL_WIDTH;top+=(height-crop)/2U;height=crop;}
  if(width==CL_WIDTH && height==CL_HEIGHT)return source;
  if(width==0U || height==0U)return source;
  SDL_Surface *filled=SDL_CreateRGBSurfaceWithFormat(0,CL_WIDTH,CL_HEIGHT,16,SDL_PIXELFORMAT_RGB565);
  if(filled==NULL)return source;
  for(unsigned int y=0U;y<CL_HEIGHT;++y){
    Uint16 *target=(Uint16 *)((unsigned char *)filled->pixels+(size_t)y*(size_t)filled->pitch);
    int yp=(int)(((2U*y+1U)*height*256U)/(2U*CL_HEIGHT))-128;
    if(yp<0)yp=0;if((unsigned int)yp>(height-1U)*256U)yp=(int)((height-1U)*256U);
    unsigned int sy=top+(unsigned int)yp/256U,sy2=sy+1U<top+height?sy+1U:sy;
    for(unsigned int x=0U;x<CL_WIDTH;++x){
      int xp=(int)(((2U*x+1U)*width*256U)/(2U*CL_WIDTH))-128;
      if(xp<0)xp=0;if((unsigned int)xp>(width-1U)*256U)xp=(int)((width-1U)*256U);
      unsigned int sx=left+(unsigned int)xp/256U,sx2=sx+1U<left+width?sx+1U:sx;
      target[x]=splash_blend(splash_pixel(source,sx,sy),splash_pixel(source,sx2,sy),
          splash_pixel(source,sx,sy2),splash_pixel(source,sx2,sy2),(unsigned int)xp%256U,(unsigned int)yp%256U);
    }
  }
  SDL_FreeSurface(source);return filled;
}
static SDL_Surface *load_splash(void) {
  /* The build emits one bounded, uncompressed 24-bit BMP. Decode directly to
   * the existing RGB565 canvas; avoid vendor SDL's optimized cross-format
   * blitters and any GPU dependency during early boot. */
  unsigned char header[54], row[CL_WIDTH * 3U];
  struct stat st;
  SDL_Surface *result = NULL;
  int fd = open(SPLASH_PATH, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return NULL;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      pread(fd, header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
      header[0] != 'B' || header[1] != 'M' || bmp_u32(header + 14U) != 40U ||
      bmp_u32(header + 18U) != CL_WIDTH || bmp_u32(header + 22U) != CL_HEIGHT ||
      header[26] != 1U || header[27] != 0U || header[28] != 24U || header[29] != 0U ||
      bmp_u32(header + 30U) != 0U || bmp_u32(header + 10U) != sizeof(header) ||
      st.st_size != (off_t)(sizeof(header) + sizeof(row) * CL_HEIGHT)) goto done;
  result = SDL_CreateRGBSurfaceWithFormat(0, CL_WIDTH, CL_HEIGHT, 16, SDL_PIXELFORMAT_RGB565);
  if (result == NULL) goto done;
  for (unsigned int y = 0U; y < CL_HEIGHT; ++y) {
    Uint16 *target = (Uint16 *)((unsigned char *)result->pixels + (size_t)y * (size_t)result->pitch);
    off_t offset = (off_t)(sizeof(header) + (CL_HEIGHT - 1U - y) * sizeof(row));
    if (pread(fd, row, sizeof(row), offset) != (ssize_t)sizeof(row)) {
      SDL_FreeSurface(result); result = NULL; break;
    }
    for (unsigned int x = 0U; x < CL_WIDTH; ++x)
      target[x] = (Uint16)(((uint32_t)(row[x * 3U + 2U] >> 3U) << 11U) |
                          ((uint32_t)(row[x * 3U + 1U] >> 2U) << 5U) |
                          (row[x * 3U] >> 3U));
  }
done:
  (void)close(fd);
  return result!=NULL?fill_splash(result):NULL;
}
static void open_pad(struct pad pads[PAD_COUNT], int device) {
  SDL_Joystick *handle = SDL_JoystickOpen(device);
  size_t i;
  SDL_JoystickID instance;
  if (handle == NULL)
    return;
  instance = SDL_JoystickInstanceID(handle);
  for (i = 0U; i < PAD_COUNT; ++i)
    if (pads[i].handle != NULL && pads[i].instance == instance) {
      SDL_JoystickClose(handle);
      return;
    }
  for (i = 0U; i < PAD_COUNT; ++i)
    if (pads[i].handle == NULL) {
      pads[i].handle = handle;
      pads[i].instance = instance;
      return;
    }
  SDL_JoystickClose(handle);
}
int main(int argc, char **argv) {
  const char *message = "Startup could not continue.", *log = NULL;
  bool once = false, surface_test = false, progress = false;
  bool splash = false, framebuffer = false;
  const char *surface_output = NULL;
  struct status_video video;
  struct cl_theme theme;
  struct pad pads[PAD_COUNT];
  struct sigaction action;
  Uint32 next_draw = 0U;
  int i, result = 1;
  memset(pads, 0, sizeof(pads));
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--message") == 0 && i + 1 < argc)
      message = argv[++i];
    else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc &&
             argv[i + 1][0] == '/')
      log = argv[++i];
    else if (strcmp(argv[i], "--test-once") == 0)
      once = true;
    else if (strcmp(argv[i], "--test-surface") == 0)
      surface_test = true;
    else if (strcmp(argv[i], "--progress") == 0)
      progress = true;
    else if (strcmp(argv[i], "--splash") == 0) {
      splash = true;
      progress = true;
    } else if (strcmp(argv[i], "--framebuffer-splash") == 0) {
      splash = true;
      framebuffer = true;
    } else if (strcmp(argv[i], "--surface-output") == 0 && i + 1 < argc && argv[i + 1][0] == '/')
      surface_output = argv[++i];
    else if (strcmp(argv[i], "--help") == 0) {
      puts("console-boot-status --message TEXT [--log ABS_PATH] [--test-once | "
           "--test-surface] [--progress | --splash | --framebuffer-splash] "
           "[--surface-output ABS_PATH]");
      return 0;
    } else {
      fprintf(stderr, "Invalid boot-status argument\n");
      return 2;
    }
  }
  if (framebuffer) {
    if (SDL_Init(0U) != 0) return 1;
    SDL_Surface *image = load_splash();
    bool rendered = image != NULL && splash_framebuffer(image);
    SDL_FreeSurface(image);
    SDL_Quit();
    fprintf(stderr, "EARLY_SPLASH_RENDERED=%u\n", rendered ? 1U : 0U);
    return rendered ? 0 : 1;
  }
  memset(&action, 0, sizeof(action));
  action.sa_handler = signal_stop;
  (void)sigemptyset(&action.sa_mask);
  (void)sigaction(SIGTERM, &action, NULL);
  (void)sigaction(SIGINT, &action, NULL);
  cl_theme_default(&theme);
  /* Silky supplies white text over image assets. The diagnostic must remain
   * legible even when those image/font files are unavailable. */
  theme.background = 0x171a21U;
  theme.panel = 0x282d37U;
  theme.fonts[CL_FONT_LIST].color = 0xffffffU;
  read_theme(&theme);
  (void)cl_theme_read(&theme, "/system/share/console/themes/Silky/moss.override.json");
  if (!progress && SDL_Init(0U) == 0) {
    ui_assets_ready = cl_assets_open(&ui_assets, &theme, "/system/share/console/themes/Silky");
    if (!ui_assets_ready) cl_assets_close(&ui_assets);
  }
  if (surface_test) {
    size_t ink = 0U;
    Uint32 text_color;
    if (SDL_Init(0U) != 0) return 1;
    memset(&video, 0, sizeof(video));
    video.surface = SDL_CreateRGBSurfaceWithFormat(0, CL_WIDTH, CL_HEIGHT, 16,
                                                   SDL_PIXELFORMAT_RGB565);
    if (video.surface == NULL) {
      fprintf(stderr, "Diagnostic surface allocation failed: %s\n",
              SDL_GetError());
      return 1;
    }
    if (splash) video.splash = load_splash();
    if (splash && video.splash == NULL) {
      close_video(&video);
      return 1;
    }
    render(&video, &theme, message, log, progress);
    if (surface_output != NULL && SDL_SaveBMP(video.surface, surface_output) != 0) {
      close_video(&video);
      return 1;
    }
    text_color = color(video.surface, readable(theme.fonts[CL_FONT_LIST].color,
                                               theme.background));
    for (int y = 0; y < CL_HEIGHT; ++y) {
      const Uint16 *row =
          (const Uint16 *)((const unsigned char *)video.surface->pixels +
                           (size_t)y * (size_t)video.surface->pitch);
      for (int x = 0; x < CL_WIDTH; ++x)
        if (row[x] == (Uint16)text_color)
          ++ink;
    }
    printf("BOOT_STATUS_SURFACE_RENDERED=1 text_pixels=%zu\n", ink);
    close_video(&video);
    SDL_Quit();
    return splash || ink > 100U ? 0 : 1;
  }
  if (!open_video(&video)) {
    fprintf(stderr, "Cannot display startup diagnostic: %s\n", SDL_GetError());
    close_video(&video);
    SDL_Quit();
    return 1;
  }
  if (splash) {
    video.splash = load_splash();
    if (video.splash == NULL)
      fprintf(stderr, "Startup splash unavailable; displaying progress text\n");
  }
  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0)
    fprintf(stderr, "Diagnostic gamepad unavailable: %s\n", SDL_GetError());
  else
    for (i = 0; i < SDL_NumJoysticks(); ++i)
      open_pad(pads, i);
  while (!stopping) {
    SDL_Event event;
    Uint32 now = SDL_GetTicks();
    while (SDL_PollEvent(&event) != 0) {
      size_t p;
      if (event.type == SDL_JOYDEVICEADDED)
        open_pad(pads, event.jdevice.which);
      else if (event.type == SDL_JOYDEVICEREMOVED)
        for (p = 0U; p < PAD_COUNT; ++p) {
          if (pads[p].handle != NULL &&
              pads[p].instance == event.jdevice.which) {
            SDL_JoystickClose(pads[p].handle);
            memset(&pads[p], 0, sizeof(pads[p]));
          }
        }
      else if (!progress && (event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP))
        for (p = 0U; p < PAD_COUNT; ++p) {
          if (pads[p].handle != NULL &&
              pads[p].instance == event.jbutton.which) {
            if (event.type == SDL_JOYBUTTONDOWN)
              pads[p].pressed[event.jbutton.button] = true;
            else if (pads[p].pressed[event.jbutton.button]) {
              result = 0;
              stopping = 1;
            }
          }
        }
      /* Keyboard, mouse and touch never activate a retry or an action. */
    }
    if ((Sint32)(now - next_draw) >= 0) {
      render(&video, &theme, message, log, progress);
      if (!present(&video)) {
        fprintf(stderr, "Startup diagnostic presentation failed: %s\n",
                SDL_GetError());
        break;
      }
      if (once) {
        puts("BOOT_STATUS_RENDERED=1");
        result = 0;
        break;
      }
      next_draw = now + 500U;
    }
    SDL_Delay(16U);
  }
  for (i = 0; i < (int)PAD_COUNT; ++i)
    if (pads[i].handle != NULL)
      SDL_JoystickClose(pads[i].handle);
  close_video(&video);
  SDL_Quit();
  return result;
}
