#include <pthread.h>
#include <stdlib.h>

#include "bw_edition.h"

static pthread_once_t s_lod_once = PTHREAD_ONCE_INIT;
static float s_lod = 1.0f;

static void init_lod(void)
{
    const char *e;
    float lod;

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
        s_lod = lod;
}

float bw_vfx_lod_scale(void)
{
    pthread_once(&s_lod_once, init_lod);
    return s_lod;
}
