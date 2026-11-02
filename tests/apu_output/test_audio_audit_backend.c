#include "apu_xaudio2.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *xa2_backend_name(void);

static int check(int condition, const char *description)
{
    if (!condition)
        fprintf(stderr, "FAIL: %s\n", description);
    return condition;
}

int main(void)
{
    int16_t samples[256][2] = {{0}};
    int ok = 1;

    setenv("RECOMP_AUDIO_HEADLESS_AUDIT", "1", 1);
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "128", 1);
    setenv("RECOMP_AUDIO_BUF_COUNT", "2", 1);
    ok &= check(SDL_WasInit(SDL_INIT_AUDIO) == 0,
                "SDL audio starts uninitialized");
    ok &= check(xa2_init(), "headless audit backend initializes");
    ok &= check(xa2_is_active(), "headless audit backend is active");
    ok &= check(strstr(xa2_backend_name(), "not audible") != NULL,
                "backend label cannot imply audible output");
    ok &= check(SDL_WasInit(SDL_INIT_AUDIO) == 0,
                "headless audit opens no SDL or CoreAudio device");
    ok &= check(xa2_get_buffer_size() == 128 && xa2_get_buffer_count() == 2,
                "headless audit uses bounded queue configuration");
    ok &= check(xa2_submit_samples(&samples[0][0], 256),
                "headless audit accepts stereo frames");
    SDL_Delay(20);
    ok &= check(xa2_queued_samples() == 0,
                "headless audit consumes at wall-clock cadence");
    ok &= check(xa2_samples_played() == 256,
                "headless audit never invents played frames");
    ok &= check(xa2_underruns() == 1,
                "headless audit reports unannounced queue underflow");

    ok &= check(xa2_submit_samples(&samples[0][0], 256),
                "headless audit accepts another period");
    xa2_expect_drain();
    SDL_Delay(20);
    ok &= check(xa2_queued_samples() == 0 && xa2_underruns() == 1,
                "announced drain is not an underflow");
    xa2_shutdown();
    ok &= check(!xa2_is_active(), "headless audit shuts down");
    ok &= check(SDL_WasInit(SDL_INIT_AUDIO) == 0,
                "headless audit shutdown leaves SDL audio untouched");

    setenv("RECOMP_AUDIO_BUF_COUNT", "invalid", 1);
    ok &= check(!xa2_init(), "invalid audit configuration fails explicitly");
    ok &= check(!xa2_is_active(), "failed audit configuration stays inactive");
    setenv("RECOMP_AUDIO_BUF_COUNT", "2", 1);
    setenv("RECOMP_AUDIO_HEADLESS_AUDIT", "invalid", 1);
    ok &= check(!xa2_init(), "invalid audit opt-in fails explicitly");
    ok &= check(!xa2_is_active(), "invalid audit opt-in stays inactive");
    return ok ? 0 : 1;
}
