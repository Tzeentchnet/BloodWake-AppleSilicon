#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_edition.h"

static bw_edition_t s_edition;
unsigned char g_bw_fix_on[BW_FIX_COUNT];
bw_edition_t bw_edition(void) { return s_edition; }
extern float bw_vfx_lod_scale(void);

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
    return 0;
}
