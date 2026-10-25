#include "platform_audio.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

#define BLOOP_SOUND_PATH "romfs:/sounds/bloop.wav"
#define POP_SOUND_PATH "romfs:/sounds/pop.wav"

typedef struct {
    Uint8 *buffer;
    Uint32 length;
} AudioClip;

static SDL_AudioDeviceID audio_device = 0;
static SDL_AudioSpec audio_spec;
static AudioClip bloop_clip = {0};
static AudioClip pop_clip = {0};
static bool audio_ready = false;

static bool load_audio_clip(const char *path, AudioClip *clip) {
    SDL_AudioSpec loaded_spec = {0};
    Uint8 *loaded_buffer = NULL;
    Uint32 loaded_length = 0;
    SDL_AudioCVT converter;

    if (!SDL_LoadWAV(path, &loaded_spec, &loaded_buffer, &loaded_length))
        return false;

    if (SDL_BuildAudioCVT(
            &converter,
            loaded_spec.format,
            loaded_spec.channels,
            loaded_spec.freq,
            audio_spec.format,
            audio_spec.channels,
            audio_spec.freq
        ) < 0) {
        SDL_FreeWAV(loaded_buffer);
        return false;
    }

    converter.len = (int)loaded_length;
    converter.buf = malloc((size_t)loaded_length * (size_t)converter.len_mult);
    if (!converter.buf) {
        SDL_FreeWAV(loaded_buffer);
        return false;
    }

    memcpy(converter.buf, loaded_buffer, loaded_length);
    SDL_FreeWAV(loaded_buffer);

    if (SDL_ConvertAudio(&converter) != 0) {
        free(converter.buf);
        return false;
    }

    clip->buffer = converter.buf;
    clip->length = (Uint32)converter.len_cvt;
    return true;
}

static void free_audio_clip(AudioClip *clip) {
    free(clip->buffer);
    clip->buffer = NULL;
    clip->length = 0;
}

static void play_audio_clip(const AudioClip *clip) {
    if (!audio_ready || !audio_device || !clip->buffer || clip->length == 0)
        return;

    SDL_ClearQueuedAudio(audio_device);
    SDL_QueueAudio(audio_device, clip->buffer, clip->length);
}

bool platform_audio_init(void) {
    SDL_AudioSpec want = {0};

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;

    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &audio_spec, 0);
    if (!audio_device)
        return false;

    if (!load_audio_clip(BLOOP_SOUND_PATH, &bloop_clip)) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
        return false;
    }

    if (!load_audio_clip(POP_SOUND_PATH, &pop_clip)) {
        free_audio_clip(&bloop_clip);
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
        return false;
    }

    SDL_PauseAudioDevice(audio_device, 0);
    audio_ready = true;
    return true;
}

void platform_audio_exit(void) {
    if (audio_device) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }

    free_audio_clip(&bloop_clip);
    free_audio_clip(&pop_clip);
    audio_ready = false;
}

void platform_audio_play_bloop(void) {
    play_audio_clip(&bloop_clip);
}

void platform_audio_play_pop(void) {
    play_audio_clip(&pop_clip);
}
