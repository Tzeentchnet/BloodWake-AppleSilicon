#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_edition.h"
#include "../../src/gpu/bw_vfx_lod_native.h"

static bw_edition_t s_edition;
unsigned char g_bw_fix_on[BW_FIX_COUNT];
bw_edition_t bw_edition(void) { return s_edition; }

static atomic_int s_failed;

static void *lod_reader(void *arg)
{
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        float value = bw_vfx_lod_scale();
        if (value != 2.0f && value != 7.0f)
            atomic_store_explicit(&s_failed, 1, memory_order_release);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    float expected = 1.0f;
    if (argc != 2)
        return 2;
    unsetenv("BW_VFX");
    unsetenv("BW_VFX_LOD");
    s_edition = !strcmp(argv[1], "original") ? BW_ED_ORIGINAL : BW_ED_REMASTERED;
    g_bw_fix_on[BW_FIX_VFX_DRAW_DISTANCE] = 1;
    if (!strcmp(argv[1], "default"))
        expected = 2.0f;
    else if (!strcmp(argv[1], "runtime") || !strcmp(argv[1], "concurrent"))
        expected = 2.0f;
    else if (!strcmp(argv[1], "off"))
        setenv("BW_VFX", "0", 1);
    else if (!strcmp(argv[1], "fix-off"))
        g_bw_fix_on[BW_FIX_VFX_DRAW_DISTANCE] = 0;
    else if (!strcmp(argv[1], "override")) {
        g_bw_fix_on[BW_FIX_VFX_DRAW_DISTANCE] = 0;
        setenv("BW_VFX_LOD", "3.5", 1);
        expected = 3.5f;
    } else if (!strcmp(argv[1], "clamp")) {
        setenv("BW_VFX_LOD", "100", 1);
        expected = 8.0f;
    } else if (!strcmp(argv[1], "named-off"))
        setenv("BW_VFX_LOD", "off", 1);
    if (bw_vfx_lod_scale() != expected) {
        fprintf(stderr, "LOD %s: expected %.1f, got %.1f\n",
                argv[1], expected, bw_vfx_lod_scale());
        return 1;
    }
    if (!strcmp(argv[1], "runtime")) {
        char err[128];
        if (bw_vfx_lod_set_scale(7.0f, err, sizeof err) != 0 ||
            bw_vfx_lod_scale() != 7.0f ||
            bw_vfx_lod_set_scale(9.0f, err, sizeof err) == 0 ||
            !strstr(err, "1 to 8"))
            return 1;
    } else if (!strcmp(argv[1], "concurrent")) {
        pthread_t reader;
        char err[128];
        atomic_store_explicit(&s_failed, 0, memory_order_release);
        if (bw_vfx_lod_set_scale(2.0f, err, sizeof err) != 0 ||
            pthread_create(&reader, NULL, lod_reader, NULL) != 0)
            return 1;
        for (int i = 0; i < 10000; i++)
            if (bw_vfx_lod_set_scale((i & 1) ? 2.0f : 7.0f, err, sizeof err) != 0)
                return 1;
        pthread_join(reader, NULL);
        if (atomic_load_explicit(&s_failed, memory_order_acquire))
            return 1;
    }
    return 0;
}
