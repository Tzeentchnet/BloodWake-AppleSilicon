#include "apu_xaudio2.h"

#include <SDL.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

int xa2_output_channels(void);

static int check(int condition, const char *description)
{
    if (!condition)
        fprintf(stderr, "FAIL: %s\n", description);
    return condition;
}

int main(void)
{
    int16_t samples[4096 * 2];
    int ok = 1;
    for (int i = 0; i < 4096; i++) {
        samples[i * 2] = (int16_t)(i % 1024 - 512);
        samples[i * 2 + 1] = (int16_t)(512 - i % 1024);
    }

    ok &= check(xa2_output_channels() == 2, "SDL output is stereo before initialization");
    ok &= check(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0, "another SDL user initializes audio");
    ok &= check(xa2_init(), "SDL dummy audio device opens");
    if (!ok)
        return 1;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    ok &= check(SDL_WasInit(SDL_INIT_AUDIO) != 0, "backend owns an SDL audio reference");
    ok &= check(xa2_init(), "initialization is idempotent");
    ok &= check(xa2_is_active(), "device is active");
    ok &= check(xa2_output_channels() == 2, "SDL output is stereo after initialization");
    ok &= check(xa2_get_buffer_count() == 8, "configured buffer count is used");
    ok &= check(xa2_get_buffer_size() > 0, "device buffer size is valid");
    ok &= check(!xa2_submit_samples(NULL, 256), "null input is rejected");
    ok &= check(!xa2_submit_samples(samples, 0), "empty submission is rejected");
    ok &= check(!xa2_submit_samples(samples, INT_MAX), "oversized submission is rejected");
    ok &= check(xa2_submit_samples(samples, 4096), "non-silent stereo frames are accepted");
    xa2_expect_drain();
    for (int i = 0; i < 2000 && xa2_samples_played() < 4096; i++)
        SDL_Delay(1);
    ok &= check(xa2_samples_played() == 4096, "dummy device consumes all submitted frames");
    ok &= check(xa2_queued_samples() == 0, "device queue drains");
    ok &= check(xa2_underruns() == 0, "announced drain is not an underrun");

    ok &= check(xa2_submit_samples(samples, 4096), "second block is accepted");
    ok &= check(xa2_queued_samples() > 0, "queued frames are counted");
    for (int i = 0; i < 2000 && xa2_samples_played() < 8192; i++)
        SDL_Delay(1);
    ok &= check(xa2_samples_played() == 8192, "played-frame accounting advances");
    ok &= check(xa2_queued_samples() == 0, "second block drains");
    ok &= check(xa2_underruns() == 1, "unannounced drain counts once");
    (void)xa2_queued_samples();
    ok &= check(xa2_underruns() == 1, "same dry spell is not counted twice");
    xa2_shutdown();
    ok &= check(!xa2_is_active(), "device shuts down");
    ok &= check(SDL_WasInit(SDL_INIT_AUDIO) == 0, "backend releases its SDL audio reference");
    ok &= check(!xa2_submit_samples(samples, 256), "submissions fail after shutdown");
    ok &= check(xa2_output_channels() == 2, "SDL output remains stereo after shutdown");
    ok &= check(xa2_init(), "device can reopen");
    ok &= check(xa2_underruns() == 0, "underruns reset for a new instance");
    ok &= check(xa2_samples_played() == 0, "played frames reset for a new instance");
    xa2_shutdown();
    xa2_shutdown();
    return ok ? 0 : 1;
}
