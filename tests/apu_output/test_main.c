#include "apu_xaudio2.h"

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
    const int16_t samples[256 * 2] = {0};
    int ok = 1;

    ok &= check(xa2_output_channels() == 2, "SDL output is stereo before initialization");
    ok &= check(xa2_init(), "SDL dummy audio device opens");
    if (!ok)
        return 1;
    ok &= check(xa2_is_active(), "device is active");
    ok &= check(xa2_output_channels() == 2, "SDL output is stereo after initialization");
    ok &= check(xa2_get_buffer_count() == 8, "configured buffer count is used");
    ok &= check(xa2_get_buffer_size() > 0, "device buffer size is valid");
    ok &= check(xa2_submit_samples(samples, 256), "stereo audio period is accepted");
    xa2_shutdown();
    ok &= check(!xa2_is_active(), "device shuts down");
    ok &= check(!xa2_submit_samples(samples, 256), "submissions fail after shutdown");
    ok &= check(xa2_output_channels() == 2, "SDL output remains stereo after shutdown");
    return ok ? 0 : 1;
}
