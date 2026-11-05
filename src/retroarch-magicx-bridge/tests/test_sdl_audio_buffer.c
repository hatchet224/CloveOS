/* Exercise the production SDL driver and FIFO against SDL's dummy device.
 * This validates buffer accounting; it does not test physical speaker output. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SDL.h"
#include "audio/drivers/sdl_audio.c"

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_ERR(const char *fmt, ...) { (void)fmt; }
void RARCH_DBG(const char *fmt, ...) { (void)fmt; }

int main(void)
{
   unsigned rate = 0;
   unsigned char samples[1024], output[1024];
   size_t capacity;
   sdl_audio_t *sdl;
   assert(setenv("SDL_AUDIODRIVER", "dummy", 1) == 0);
   sdl = (sdl_audio_t *)audio_sdl.init(NULL, 48000, 128, 0, &rate);
   assert(sdl && rate == 48000);
   assert(audio_sdl.stop(sdl));
   capacity = audio_sdl.buffer_size(sdl);
   assert(capacity >= sizeof(samples) && capacity % 4 == 0);
   /* Stop first: dummy audio may consume between open and pause. Refill to
    * capacity so all subsequent assertions have a deterministic baseline. */
   audio_sdl.set_nonblock_state(sdl, true);
   memset(samples, 0x35, sizeof(samples));
   while (audio_sdl.write_avail(sdl) != 0)
      assert(audio_sdl.write(sdl, samples, sizeof(samples)) > 0);
   assert(audio_sdl.write_avail(sdl) == 0);
   assert(audio_sdl.buffer_size(sdl) == capacity);
   assert(audio_sdl.write(sdl, samples, sizeof(samples)) == 0);
   for (unsigned n = 0; n < 1000; ++n)
   {
      SDL_LockAudioDevice(sdl->speaker_device);
      sdl_audio_playback_cb(sdl, output, sizeof(output));
      SDL_UnlockAudioDevice(sdl->speaker_device);
      assert(audio_sdl.write_avail(sdl) == sizeof(output));
      assert(audio_sdl.buffer_size(sdl) == capacity);
      assert(audio_sdl.write(sdl, samples, sizeof(samples)) == sizeof(samples));
      assert(audio_sdl.write_avail(sdl) == 0);
   }
   /* Drain and force underrun: empty queue reports full writable capacity,
    * the driver emits silence, and accounting remains bounded. */
   SDL_LockAudioDevice(sdl->speaker_device);
   while (FIFO_READ_AVAIL(sdl->speaker_buffer) != 0)
      sdl_audio_playback_cb(sdl, output, sizeof(output));
   memset(output, 0xff, sizeof(output));
   sdl_audio_playback_cb(sdl, output, sizeof(output));
   SDL_UnlockAudioDevice(sdl->speaker_device);
   assert(audio_sdl.write_avail(sdl) == capacity);
   for (size_t n = 0; n < sizeof(output); ++n) assert(output[n] == 0);
   assert(audio_sdl.write_avail(NULL) == 0 && audio_sdl.buffer_size(NULL) == 0);
   assert(audio_sdl.start(sdl, false));
   assert(audio_sdl.alive(sdl));
   assert(audio_sdl.stop(sdl));
   audio_sdl.free(sdl);
   puts("SDL_AUDIO_RATE_CONTROL_BUFFER_PASS actual_sdl_dummy=1 wraparound=1000 bytes=1 silence_on_underrun=1 physical_speaker=0");
   return 0;
}
