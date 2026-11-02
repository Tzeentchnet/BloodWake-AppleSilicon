#include <pthread.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_edition.h"
#include "bw_vfx_lod_native.h"

static pthread_once_t s_lod_once = PTHREAD_ONCE_INIT;
static atomic_uint s_lod_bits;

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float bits_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void init_lod(void)
{
    const char *e;
    float lod;

    atomic_store_explicit(&s_lod_bits, float_bits(1.0f), memory_order_release);
    if (!bw_edition_at_least(BW_ED_REMASTERED))
        return;
    e = getenv("BW_VFX");
    if (e && e[0] == '0')
        return;
    lod = bw_fix_enabled(BW_FIX_VFX_DRAW_DISTANCE) ? 2.0f : 0.0f;
    e = getenv("BW_VFX_LOD");
    if (e && *e) {
        if (e[0] == 'o' || e[0] == 'O')
            lod = e[1] == 'n' || e[1] == 'N' ? 2.0f : 0.0f;
        else
            lod = (float)atof(e);
    }
    if (lod > 8.0f)
        lod = 8.0f;
    if (lod > 1.0f)
        atomic_store_explicit(&s_lod_bits, float_bits(lod), memory_order_release);
}

float bw_vfx_lod_scale(void)
{
    pthread_once(&s_lod_once, init_lod);
    return bits_float(atomic_load_explicit(&s_lod_bits, memory_order_acquire));
}

int bw_vfx_lod_set_scale(float scale, char *err, size_t errlen)
{
    pthread_once(&s_lod_once, init_lod);
    if (err && errlen)
        err[0] = 0;
    if (!isfinite(scale) || scale < 1.0f || scale > 8.0f) {
        if (err && errlen) {
            snprintf(err, errlen, "LOD scale must be a finite number from 1 to 8");
            err[errlen - 1] = 0;
        }
        return -1;
    }
    if (!bw_edition_at_least(BW_ED_REMASTERED) && scale != 1.0f) {
        if (err && errlen) {
            snprintf(err, errlen, "adjustable LOD is available only in Remastered");
            err[errlen - 1] = 0;
        }
        return -1;
    }
    atomic_store_explicit(&s_lod_bits, float_bits(scale), memory_order_release);
    return 0;
}
