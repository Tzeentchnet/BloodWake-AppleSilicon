#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_gamma.h"
#include "platform/win32_compat.h"

int bw_audit_on(void) { return 0; }
void bw_audit_gamma(const uint8_t ramp[768]) { (void)ramp; }
void bw_ab_frame(uint32_t frame);
ULONGLONG GetTickCount64(void) { return 0; }

static atomic_int s_done, s_failed;

static void *write_ramps(void *unused)
{
    uint8_t ramp[768];
    (void)unused;
    for (unsigned n = 0; n < 20000; n++) {
        memset(ramp, n & 1 ? 0x11 : 0xEE, sizeof ramp);
        bw_gamma_from_title(ramp);
        bw_ab_frame(n);
    }
    atomic_store(&s_done, 1);
    return NULL;
}

static void *read_ramps(void *unused)
{
    uint8_t ramp[768];
    unsigned reads = 0;
    (void)unused;
    while (!atomic_load(&s_done) || reads < 1000) {
        if (bw_gamma_lut(ramp))
            for (unsigned i = 1; i < sizeof ramp; i++)
                if (ramp[i] != ramp[0])
                    atomic_store(&s_failed, 1);
        reads++;
    }
    return NULL;
}

static int stress(void)
{
    pthread_t writer, readers[4];
    uint8_t initial[768];
    setenv("BW_GPU_AB", "gamma", 1);
    setenv("BW_GPU_GAMMA", "1", 1);
    memset(initial, 0x11, sizeof initial);
    bw_gamma_from_title(initial);
    for (unsigned i = 0; i < 4; i++)
        if (pthread_create(&readers[i], NULL, read_ramps, NULL))
            return 1;
    if (pthread_create(&writer, NULL, write_ramps, NULL))
        return 1;
    pthread_join(writer, NULL);
    for (unsigned i = 0; i < 4; i++)
        pthread_join(readers[i], NULL);
    return atomic_load(&s_failed);
}

static int basic(int disabled)
{
    uint8_t ramp[768], out[768];
    uint32_t pixel = 0x7F112233u;

    setenv("BW_GPU_GAMMA", disabled ? "0" : "1", 1);
    if (bw_gamma_get(out) || bw_gamma_lut(out))
        return 1;
    for (unsigned i = 0; i < sizeof ramp; i++)
        ramp[i] = (uint8_t)(i & 255);
    bw_gamma_from_title(ramp);
    if (!bw_gamma_get(out) || memcmp(out, ramp, sizeof ramp) || bw_gamma_lut(out))
        return 1;
    for (unsigned i = 0; i < 256; i++) {
        ramp[i] = (uint8_t)(255 - i);
        ramp[256 + i] = (uint8_t)(i / 2);
        ramp[512 + i] = (uint8_t)i;
    }
    bw_gamma_from_title(ramp);
    if (bw_gamma_lut(out) != !disabled)
        return 1;
    bw_gamma_apply_xrgb(&pixel, 1);
    return pixel != (disabled ? 0x7F112233u : 0x7FEE1133u);
}

int main(int argc, char **argv)
{
    int result;
    if (argc != 2)
        return 2;
    if (!strcmp(argv[1], "ab")) {
        uint8_t ramp[768] = {0}, out[768];
        setenv("BW_GPU_AB", "gamma", 1);
        setenv("BW_GPU_GAMMA", "1", 1);
        bw_gamma_from_title(ramp);
        bw_ab_frame(0);
        result = !bw_gamma_lut(out);
        bw_ab_frame(1);
        result |= bw_gamma_lut(out);
        bw_ab_frame(2);
        result |= !bw_gamma_lut(out);
    } else {
        unsetenv("BW_GPU_AB");
        result = !strcmp(argv[1], "stress") ? stress()
               : basic(!strcmp(argv[1], "disabled"));
    }
    if (result)
        fprintf(stderr, "gamma %s failed\n", argv[1]);
    return result;
}
