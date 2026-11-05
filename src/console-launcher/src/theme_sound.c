#include "launcher/theme_sound.h"
#include "sessiond/library.h"
#include "minimp3.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define THEME_RATE 44100
#define THEME_CHANNELS 2U
struct cl_theme_sound {
    SDL_AudioDeviceID device;
    Uint8 *music,*click;
    size_t music_length,click_length,music_at,click_at;
    SDL_AudioStream *music_stream;
    mp3dec_t decoder;
    int music_channels,music_rate;
    bool audio,muted;
};

static SDL_RWops *open_sound(const char *root,const char *name)
{
    char path[640];
    struct stat st;
    int n=snprintf(path,sizeof(path),"%s/sound/%s",root,name);
    if(n<0 || (size_t)n>=sizeof(path)) return NULL;
    int fd=cs_library_open_absolute(path,false);
    if(fd<0) return NULL;
    if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<1 || st.st_size>4*1024*1024){
        (void)close(fd);return NULL;
    }
    FILE *stream=fdopen(fd,"rb");
    if(stream==NULL){(void)close(fd);return NULL;}
    SDL_RWops *rw=SDL_RWFromFP(stream,SDL_TRUE);
    if(rw==NULL) (void)fclose(stream);
    return rw;
}
static Uint8 *stream_samples(SDL_AudioStream *stream,size_t maximum,size_t *length)
{
    if(SDL_AudioStreamFlush(stream)!=0) return NULL;
    int available=SDL_AudioStreamAvailable(stream);
    if(available<=0 || (size_t)available>maximum) return NULL;
    Uint8 *pcm=SDL_malloc((size_t)available);
    if(pcm==NULL) return NULL;
    if(SDL_AudioStreamGet(stream,pcm,available)!=available){SDL_free(pcm);return NULL;}
    *length=(size_t)available;
    return pcm;
}
/* Keep only a few PCM frames buffered. Music decoding runs with the audio
 * callback and never queues an entire track before MainUI can respond. */
static bool music_frame(struct cl_theme_sound *sound)
{
    if(sound->music_at>=sound->music_length){
        sound->music_at=0U;
        mp3dec_init(&sound->decoder);
        if(sound->music_stream!=NULL && SDL_AudioStreamFlush(sound->music_stream)!=0)
            return false;
    }
    size_t available=sound->music_length-sound->music_at;
    if(available>32768U) available=32768U;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info={0};
    int samples=mp3dec_decode_frame(&sound->decoder,sound->music+sound->music_at,
                                   (int)available,pcm,&info);
    if(info.frame_bytes<=0 || (size_t)info.frame_bytes>available){
        sound->music_at+=available;
        return true;
    }
    sound->music_at+=(size_t)info.frame_bytes;
    if(samples<=0) return true;
    if(sound->music_stream==NULL) {
        if(info.channels<1 || info.channels>2 || info.hz<8000 || info.hz>48000) return false;
        sound->music_channels=info.channels;
        sound->music_rate=info.hz;
        sound->music_stream=SDL_NewAudioStream(AUDIO_S16SYS,(Uint8)info.channels,info.hz,
                                               AUDIO_S16SYS,THEME_CHANNELS,THEME_RATE);
        if(sound->music_stream==NULL) return false;
    }
    if(info.channels!=sound->music_channels || info.hz!=sound->music_rate) return false;
    return SDL_AudioStreamPut(sound->music_stream,pcm,samples*info.channels*(int)sizeof(pcm[0]))==0;
}
static void load_music(struct cl_theme_sound *sound,SDL_RWops *rw)
{
    if(rw==NULL) return;
    sound->music=SDL_LoadFile_RW(rw,&sound->music_length,1);
    if(sound->music==NULL) return;
    if(sound->music_length>=4U && sound->music_length<=4U*1024U*1024U) {
        mp3dec_init(&sound->decoder);
        for(unsigned int attempt=0U;attempt<12U;++attempt) {
            if(!music_frame(sound)) break;
            if(sound->music_stream!=NULL) return;
        }
    }
    if(sound->music_stream!=NULL){SDL_FreeAudioStream(sound->music_stream);sound->music_stream=NULL;}
    SDL_free(sound->music);sound->music=NULL;sound->music_length=0U;
}
static Uint8 *load_click(SDL_RWops *rw,size_t *length)
{
    SDL_AudioSpec spec;
    Uint8 *raw=NULL,*pcm=NULL;
    Uint32 bytes=0U;
    if(rw==NULL || SDL_LoadWAV_RW(rw,1,&spec,&raw,&bytes)==NULL) return NULL;
    if(bytes<=256U*1024U) {
        SDL_AudioStream *stream=SDL_NewAudioStream(spec.format,spec.channels,spec.freq,
                                                  AUDIO_S16SYS,THEME_CHANNELS,THEME_RATE);
        if(stream!=NULL){
            if(SDL_AudioStreamPut(stream,raw,(int)bytes)==0)
                pcm=stream_samples(stream,THEME_RATE*THEME_CHANNELS*2U*3U,length);
            SDL_FreeAudioStream(stream);
        }
    }
    SDL_FreeWAV(raw);
    return pcm;
}
static void playback(void *context,Uint8 *output,int size)
{
    struct cl_theme_sound *sound=context;
    size_t length=(size_t)size;
    SDL_memset(output,0,length);
    if(!sound->muted && sound->music_stream!=NULL) {
        Uint8 buffer[8192];
        int wanted=size>(int)sizeof(buffer)?(int)sizeof(buffer):size;
        for(unsigned int attempt=0U;attempt<12U &&
            SDL_AudioStreamAvailable(sound->music_stream)<wanted;++attempt)
            if(!music_frame(sound)) break;
        int got=SDL_AudioStreamGet(sound->music_stream,buffer,wanted);
        if(got>0) SDL_MixAudioFormat(output,buffer,AUDIO_S16SYS,(Uint32)got,42);
    }
    if(sound->click_at<sound->click_length) {
        size_t count=sound->click_length-sound->click_at;
        if(count>length) count=length;
        SDL_MixAudioFormat(output,sound->click+sound->click_at,AUDIO_S16SYS,(Uint32)count,42);
        sound->click_at+=count;
    }
}
struct cl_theme_sound *cl_theme_sound_open(const char *theme,bool mute_music)
{
    SDL_RWops *music=open_sound(theme,"bgm.mp3"),*click=open_sound(theme,"change.wav");
    if(music==NULL && click==NULL) return NULL;
    struct cl_theme_sound *sound=calloc(1U,sizeof(*sound));
    if(sound==NULL){
        if(music!=NULL) SDL_RWclose(music);
        if(click!=NULL) SDL_RWclose(click);
        return NULL;
    }
    load_music(sound,music);
    sound->click=load_click(click,&sound->click_length);
    sound->click_at=sound->click_length;
    sound->muted=mute_music;
    if((sound->music==NULL && sound->click==NULL) || SDL_InitSubSystem(SDL_INIT_AUDIO)!=0)
        goto failed;
    sound->audio=true;
    SDL_AudioSpec spec={.freq=THEME_RATE,.format=AUDIO_S16SYS,.channels=THEME_CHANNELS,
                        .samples=1024,.callback=playback,.userdata=sound};
    sound->device=SDL_OpenAudioDevice(NULL,0,&spec,NULL,0);
    if(sound->device==0U) goto failed;
    SDL_PauseAudioDevice(sound->device,0);
    return sound;
failed:
    cl_theme_sound_close(sound);
    return NULL;
}
void cl_theme_sound_mute(struct cl_theme_sound *sound,bool mute)
{
    if(sound==NULL) return;
    SDL_LockAudioDevice(sound->device);
    sound->muted=mute;
    SDL_UnlockAudioDevice(sound->device);
}
void cl_theme_sound_click(struct cl_theme_sound *sound)
{
    if(sound==NULL || sound->click==NULL) return;
    SDL_LockAudioDevice(sound->device);
    sound->click_at=0U;
    SDL_UnlockAudioDevice(sound->device);
}
void cl_theme_sound_close(struct cl_theme_sound *sound)
{
    if(sound==NULL) return;
    if(sound->device!=0U) SDL_CloseAudioDevice(sound->device);
    if(sound->audio) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if(sound->music_stream!=NULL) SDL_FreeAudioStream(sound->music_stream);
    SDL_free(sound->music);
    SDL_free(sound->click);
    free(sound);
}
