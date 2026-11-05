int main(void)
{
   unsigned char pixels[320U * 240U * 4U];
   SDL_Renderer renderer = {0};
   sdl2_video_t video = {&renderer};
   SDL_Texture texture = {256, 256, SDL_TEXTUREACCESS_STREAMING, 0,
      SDL_PIXELFORMAT_RGB565};
   SDL_Rect source = {0, 0, 160, 144}, destination = {53, 0, 533, 480};
   int before;
   memset(pixels, 0x5a, sizeof(pixels));

   assert(sdl2_window_flags(true, true, true) == SDL_WINDOW_SHOWN);
   assert(sdl2_window_flags(true, true, false) == SDL_WINDOW_SHOWN);
   assert(sdl2_window_flags(true, false, false) == SDL_WINDOW_SHOWN);
   assert(sdl2_window_flags(false, true, true) == SDL_WINDOW_FULLSCREEN_DESKTOP);
   assert(sdl2_window_flags(false, true, false) == SDL_WINDOW_FULLSCREEN);
   assert(sdl2_window_flags(false, false, false) == SDL_WINDOW_RESIZABLE);

   buffer_bytes = 160U * 144U * 2U;
   assert(sdl2_moss_upload(&texture, pixels, 160, 144, 320, false, false));
   assert(last_upload.w == 160 && last_upload.h == 144);
   before = uploads;
   assert(!sdl2_moss_upload(&texture, pixels, 160, 144, 318, false, false));
   assert(!sdl2_moss_upload(&texture, pixels, 160, 144, UINT_MAX, false, false));
   assert(!sdl2_moss_upload(&texture, pixels, 0, 144, 320, false, false));
   assert(!sdl2_moss_upload(NULL, pixels, 160, 144, 320, false, false));
   assert(!sdl2_moss_upload(&texture, NULL, 160, 144, 320, false, false));
   texture.w = 128;
   assert(!sdl2_moss_upload(&texture, pixels, 160, 144, 320, false, false));
   texture.w = 256; texture.h = 128;
   assert(!sdl2_moss_upload(&texture, pixels, 160, 144, 320, false, false));
   texture.h = 256; texture.format = SDL_PIXELFORMAT_ARGB8888;
   assert(!sdl2_moss_upload(&texture, pixels, 160, 144, 320, false, false));
   texture.format = SDL_PIXELFORMAT_RGB565; texture.access = 0;
   assert(!sdl2_moss_upload(&texture, pixels, 160, 144, 320, false, false));
   texture.access = SDL_TEXTUREACCESS_STREAMING; texture.error = -1;
   assert(!sdl2_moss_upload(&texture, pixels, 160, 144, 320, false, false));
   texture.error = 0; texture.w = 32768;
   assert(!sdl2_moss_upload(&texture, pixels, 160, 144, 320, false, false));
   texture.w = 256;
   assert(uploads == before);

   assert(sdl2_moss_copy(&video, &texture, &source, &destination, 0));
   assert(last_source.w == 160 && last_source.h == 144);
   assert(last_destination.x == 53 && last_destination.y == 0);
   assert(last_rotation == 0);
   output_w = 480; output_h = 640;
   assert(sdl2_moss_copy(&video, &texture, &source, &destination, 0));
   assert(last_destination.x == -27 && last_destination.y == 80);
   assert(last_destination.w == 533 && last_destination.h == 480);
   assert(last_rotation == 90);
   assert(sdl2_moss_copy(&video, &texture, &source, &destination, 270));
   assert(last_rotation == 360);
   before = copies;
   assert(!sdl2_moss_copy(&video, &texture, NULL, &destination, 0));
   output_w = 0;
   assert(!sdl2_moss_copy(&video, &texture, &source, &destination, 0));
   output_w = 480; renderer.error = -1;
   assert(!sdl2_moss_copy(&video, &texture, &source, &destination, 0));
   renderer.error = 0;
   assert(copies == before);

   /* Padded menu allocation: logical320x240, backing512x256. */
   texture.w = 512; texture.h = 256; texture.format = SDL_PIXELFORMAT_RGBA4444;
   buffer_bytes = 320U * 240U * 2U;
   assert(sdl2_moss_upload(&texture, pixels, 320, 240, 640, false, true));
   assert(last_upload.w == 320 && last_upload.h == 240);
   /* Core ARGB8888 keeps its own pitch and 32-bit pixel format. */
   texture.format = SDL_PIXELFORMAT_ARGB8888;
   buffer_bytes = 160U * 144U * 4U;
   assert(sdl2_moss_upload(&texture, pixels, 160, 144, 640, true, false));
   update_error = -1;
   assert(!sdl2_moss_upload(&texture, pixels, 160, 144, 640, true, false));
   puts("MOSS_SDL2_WINDOW_NPOT_PITCH_ROTATION_MOCK_PASS");
   return 0;
}
