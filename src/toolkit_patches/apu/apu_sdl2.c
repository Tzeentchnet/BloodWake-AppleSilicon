/*
 * SDL2 implementation of the toolkit's xa2_* audio-output contract.
 *
 * The APU submits 48 kHz signed 16-bit stereo frames. SDL copies them into
 * its queue and feeds the actual host device; the APU paces against the
 * queued frame count. SDL_AUDIODRIVER=dummy is an explicit, silent test
 * device, not an automatic fallback when the host device fails.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

#if !defined(_WIN32)

#include <SDL.h>
#include <limits.h>
#include <stdatomic.h>
#include <time.h>

#define SDLA_SAMPLE_RATE 48000
#define SDLA_CHANNELS 2
#define SDLA_BUF_SAMPLES_DEFAULT 512
#define SDLA_NUM_BUFS_DEFAULT 4
#define SDLA_BUF_SAMPLES_MIN 128
#define SDLA_BUF_SAMPLES_MAX 4096
#define SDLA_NUM_BUFS_MIN 2
#define SDLA_NUM_BUFS_MAX 8
#define SDLA_FRAME_BYTES (SDLA_CHANNELS * sizeof(int16_t))

static SDL_AudioDeviceID g_dev;
static int g_buf_samples = SDLA_BUF_SAMPLES_DEFAULT;
static int g_num_bufs = SDLA_NUM_BUFS_DEFAULT;
static int g_initialized;
static int g_audio_owned;
static int g_frames_written;
static _Atomic unsigned g_underruns;
static _Atomic uint64_t g_samples_queued;
static int g_dry;
static int64_t g_underrun_logged_ms;

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    char *end;
    long v;
    if (!e || !*e)
        return def;
    v = strtol(e, &end, 10);
    if (end == e)
        return def;
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return (int)v;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int xa2_init(void)
{
    SDL_AudioSpec want, have;
    const char *driver;

    if (g_initialized)
        return 1;
    g_buf_samples = env_int("RECOMP_AUDIO_BUF_SAMPLES", SDLA_BUF_SAMPLES_DEFAULT,
                            SDLA_BUF_SAMPLES_MIN, SDLA_BUF_SAMPLES_MAX);
    g_num_bufs = env_int("RECOMP_AUDIO_BUF_COUNT", SDLA_NUM_BUFS_DEFAULT,
                         SDLA_NUM_BUFS_MIN, SDLA_NUM_BUFS_MAX);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    /* Hold our own SDL audio reference even if another subsystem user has
     * already initialized it; release exactly that reference on shutdown. */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[SDL2] audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    g_audio_owned = 1;
    SDL_zero(want);
    want.freq = SDLA_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = SDLA_CHANNELS;
    want.samples = (Uint16)g_buf_samples;
    want.callback = NULL;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        fprintf(stderr, "[SDL2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        goto fail;
    }
    if (have.freq != want.freq || have.format != want.format ||
        have.channels != want.channels || have.samples == 0) {
        fprintf(stderr, "[SDL2] device returned an incompatible audio format\n");
        goto fail;
    }
    if (have.samples != g_buf_samples) {
        fprintf(stderr, "[SDL2] device buffer is %u samples (asked %d)\n",
                (unsigned)have.samples, g_buf_samples);
        g_buf_samples = have.samples > SDLA_BUF_SAMPLES_MAX
                      ? SDLA_BUF_SAMPLES_MAX : have.samples;
    }

    g_frames_written = 0;
    atomic_store_explicit(&g_samples_queued, 0, memory_order_relaxed);
    atomic_store_explicit(&g_underruns, 0, memory_order_relaxed);
    g_underrun_logged_ms = 0;
    g_dry = 0;
    SDL_PauseAudioDevice(g_dev, 0);
    if (SDL_GetAudioDeviceStatus(g_dev) != SDL_AUDIO_PLAYING) {
        fprintf(stderr, "[SDL2] audio device could not start\n");
        goto fail;
    }
    g_initialized = 1;
    driver = SDL_GetCurrentAudioDriver();
    fprintf(stderr, "[SDL2] audio initialized: driver %s (%d Hz stereo 16-bit, "
                    "%d x %d-sample buffers, %.1f ms queued max)%s\n",
            driver ? driver : "?", SDLA_SAMPLE_RATE, g_num_bufs, g_buf_samples,
            1000.0 * g_num_bufs * g_buf_samples / SDLA_SAMPLE_RATE,
            driver && !strcmp(driver, "dummy") ? " [silent: explicit dummy driver]" : "");
    return 1;

fail:
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_audio_owned = 0;
    return 0;
}

void xa2_shutdown(void)
{
    if (g_dev) {
        SDL_PauseAudioDevice(g_dev, 1);
        SDL_ClearQueuedAudio(g_dev);
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (g_audio_owned) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_owned = 0;
    }
    if (g_initialized)
        fprintf(stderr, "[SDL2] shut down (%d periods written, %u underruns)\n",
                g_frames_written, atomic_load_explicit(&g_underruns, memory_order_relaxed));
    g_initialized = 0;
}

int xa2_is_active(void)
{
    return g_initialized;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    if (!g_initialized || !g_dev || !samples || num_samples <= 0 ||
        (uint64_t)num_samples * SDLA_FRAME_BYTES > UINT32_MAX)
        return 0;
    if (SDL_QueueAudio(g_dev, samples, (Uint32)((size_t)num_samples * SDLA_FRAME_BYTES)) != 0) {
        fprintf(stderr, "[SDL2] SDL_QueueAudio failed: %s\n", SDL_GetError());
        return 0;
    }
    g_frames_written++;
    atomic_fetch_add_explicit(&g_samples_queued, (uint64_t)num_samples, memory_order_relaxed);
    return 1;
}

int xa2_get_buffer_size(void)
{
    return g_buf_samples;
}

int xa2_get_buffer_count(void)
{
    return g_num_bufs;
}

int xa2_queued_samples(void)
{
    Uint32 bytes;
    unsigned underruns;
    if (!g_initialized || !g_dev)
        return 0;
    bytes = SDL_GetQueuedAudioSize(g_dev);
    if (bytes == 0 && g_frames_written > 0) {
        if (!g_dry) {
            int64_t now = now_ms();
            g_dry = 1;
            underruns = atomic_fetch_add_explicit(&g_underruns, 1, memory_order_relaxed) + 1;
            if (now - g_underrun_logged_ms >= 1000) {
                g_underrun_logged_ms = now;
                fprintf(stderr, "[SDL2] underrun x%u\n", underruns);
            }
        }
    } else {
        g_dry = 0;
    }
    return (int)(bytes / SDLA_FRAME_BYTES);
}

void xa2_expect_drain(void)
{
    g_dry = 1;
}

unsigned xa2_underruns(void)
{
    return atomic_load_explicit(&g_underruns, memory_order_relaxed);
}

uint64_t xa2_samples_played(void)
{
    uint64_t queued, submitted;
    if (!g_initialized || !g_dev)
        return 0;
    submitted = atomic_load_explicit(&g_samples_queued, memory_order_relaxed);
    queued = SDL_GetQueuedAudioSize(g_dev) / SDLA_FRAME_BYTES;
    return submitted > queued ? submitted - queued : 0;
}
#endif
