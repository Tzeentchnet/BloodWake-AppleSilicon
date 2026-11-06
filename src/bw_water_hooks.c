#include <stddef.h>
#include <stdint.h>

#include "bw_draw_marker.h"
#include "bw_edition.h"
#include "bw_hook_platform.h"

extern ptrdiff_t g_xbox_mem_offset;
extern BW_HOOK_TLS uint32_t g_ecx, g_esp;

extern void sub_00037D60_gen(void);
extern void sub_00037E20_gen(void);
extern void sub_000389D0_gen(void);
extern void sub_00038EC0_gen(void);

#define WATER_MEM32(va) \
    (*(uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define WATER_MEM8(va) \
    (*(uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

#define X_RENDER_MANAGER 0x001A9C50u
#define X_SPLIT_VIEW_COUNT 0x001A9A78u
#define X_OCEAN 0x001C6990u
#define X_LIT_ON 0x0018F534u
#define X_REFLECTION_ON 0x0018F535u
#define X_FOAM_ON 0x0018F536u

#define RENDER_MANAGER_VIEW 0x2D8u
#define OCEAN_DRAW_ROOT 0x90u
#define OCEAN_TILE_COUNT 0x6F8u

#define RA_OCEAN_RENDER 0x0002E0CDu
#define RA_LIT_PASS 0x00039210u
#define RA_REFLECTION_PASS 0x00039262u
#define RA_FOAM_PASS 0x000392B4u

static int water_view(uint32_t *view)
{
    uint32_t count = WATER_MEM32(X_SPLIT_VIEW_COUNT);
    uint32_t current = WATER_MEM32(X_RENDER_MANAGER + RENDER_MANAGER_VIEW);

    if (!count || count > 4 || !current || current > count)
        return 0;
    *view = current - 1;
    return 1;
}

static int water_boundary(uint32_t return_address, uint32_t *view)
{
    return bw_edition_at_least(BW_ED_REMASTERED) &&
           g_ecx == X_OCEAN &&
           WATER_MEM32(g_esp) == return_address &&
           water_view(view);
}

static int water_pass_boundary(uint32_t return_address, uint32_t toggle,
                               int require_tiles, uint32_t *view)
{
    uint32_t draw_root = WATER_MEM32(X_OCEAN + OCEAN_DRAW_ROOT);

    return water_boundary(return_address, view) &&
           WATER_MEM8(toggle) &&
           draw_root &&
           (!require_tiles ||
            (int32_t)WATER_MEM32(X_OCEAN + OCEAN_TILE_COUNT) > 0) &&
           WATER_MEM32(g_esp + 4) == draw_root;
}

void sub_00037D60(void)
{
    uint32_t view;
    int marked = water_pass_boundary(RA_LIT_PASS, X_LIT_ON, 0, &view);

    (void)view;
    if (marked)
        bw_draw_marker_emit(BW_DRAW_MARKER_WATER_PASS_BEGIN(
            BW_DRAW_MARKER_WATER_PASS_LIT));
    sub_00037D60_gen();
    if (marked)
        bw_draw_marker_emit(BW_DRAW_MARKER_WATER_PASS_END(
            BW_DRAW_MARKER_WATER_PASS_LIT));
}

void sub_00037E20(void)
{
    uint32_t view;
    int marked = water_pass_boundary(RA_REFLECTION_PASS, X_REFLECTION_ON, 1, &view);

    (void)view;
    if (marked)
        bw_draw_marker_emit(BW_DRAW_MARKER_WATER_PASS_BEGIN(
            BW_DRAW_MARKER_WATER_PASS_REFLECTION));
    sub_00037E20_gen();
    if (marked)
        bw_draw_marker_emit(BW_DRAW_MARKER_WATER_PASS_END(
            BW_DRAW_MARKER_WATER_PASS_REFLECTION));
}

void sub_000389D0(void)
{
    uint32_t view;
    int marked = water_pass_boundary(RA_FOAM_PASS, X_FOAM_ON, 1, &view);

    (void)view;
    if (marked)
        bw_draw_marker_emit(BW_DRAW_MARKER_WATER_PASS_BEGIN(
            BW_DRAW_MARKER_WATER_PASS_FOAM));
    sub_000389D0_gen();
    if (marked)
        bw_draw_marker_emit(BW_DRAW_MARKER_WATER_PASS_END(
            BW_DRAW_MARKER_WATER_PASS_FOAM));
}

void sub_00038EC0(void)
{
    uint32_t view;
    int marked = water_boundary(RA_OCEAN_RENDER, &view);

    if (marked)
        bw_draw_marker_emit(BW_DRAW_MARKER_WATER_BEGIN(view));
    sub_00038EC0_gen();
    if (marked)
        bw_draw_marker_emit(BW_DRAW_MARKER_WATER_END(view));
}
