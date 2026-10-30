/*
 * bw_widescreen.h - Remastered widescreen (D-015, D-016): one place that
 * decides the picture's shape. Design: docs/remaster/widescreen.md (agent
 * R3, "option C": Hor+ by an anamorphic projection); built by D5.
 *
 *   k = picture aspect / (4/3)     16:9 -> 1.3333, 21:9 (64:27) -> 1.7778
 *
 * k == 1 means off. Original never sets it (D-007): every hook and back-end
 * path tests bw_ws_k() > 1 first and is otherwise exactly the old code.
 *
 * The scheme:
 *   - the game keeps its 640x480 logical screen, viewports, HUD and maths;
 *   - each gameplay view's projection is k times wider horizontally (left and
 *     right planes x k: Hor+, the vertical field is unchanged), and the
 *     game's frustum culling and world grid are widened to match;
 *   - the back end renders the device's full-screen targets k times wider on
 *     the host. 3D, still placed in the guest's 640-wide units, stretches
 *     over the wider surface, which restores its proportions;
 *   - 2D (the overlay layers, movies) is pillarboxed inside its own view
 *     instead, so HUD art, text and movies keep their shape. Markers in the
 *     pushbuffer (an NV097 NO_OPERATION with a tagged parameter, written
 *     through the game's own D3D) tell the back end where 2D starts and ends.
 *
 * Switches (every edition but Original, which ignores them):
 *   BW_FIX=-WIDESCREEN     4:3 as Original (the registry line in bw_fixes.def)
 *   BW_ASPECT=16:9 | 21:9 | 64:27 | 1.7778 | 4:3 | display (default: the
 *                          display's own aspect; 16:9 if it can't be read).
 *                          Capped at 2.39 (21:9 monitors); wider is pillarboxed.
 * The current Metal backend lacks widened targets and marker handling, so
 * macOS explicitly keeps 4:3 until those renderer capabilities are present.
 */
#ifndef BW_WIDESCREEN_H
#define BW_WIDESCREEN_H

#include <stdint.h>

/* After bw_edition_init(). main.c calls it once the boot knows whether it is
 * a gameplay boot (launch data "SPLASH" mode 1) or the title and menus; the
 * host window (bw_host.c), which runs no game, calls it with 0 to size its
 * window. Logs one "[WS]" line when log is set. On macOS an unavailable
 * widescreen request is always reported, even when log is 0. */
void  bw_ws_init(int gameplay_boot, int log);

/* The picture's aspect (width / height) the window and the present use. 4/3
 * when widescreen is off. The same for every boot of a session, so the window
 * keeps its shape; a 4:3 boot (the menus) is pillarboxed inside it. */
float bw_ws_aspect(void);

/* The horizontal factor for this boot's gameplay views and device targets.
 * 1.0 = off (Original, -WIDESCREEN, 4:3, and the title/menu boots). */
float bw_ws_k(void);

/* ---- in-stream markers ---------------------------------------------------
 * NV097 NO_OPERATION (method 0x0100, header 0x00040100) with a parameter whose
 * top 16 bits are BW_WS_MARK_TAG. D3D's own software methods carry small
 * values (0x300..0x324 region), so the tag can't collide. The carried
 * executor hands such a NOP to the handler the back end registers
 * (bw_set_nop_marker_handler) and drops it; with no handler (Original) the
 * game never writes one. */
#define BW_WS_NOP_HEADER   0x00040100u      /* count 1, subchannel 0, method 0x100 */
#define BW_WS_MARK_TAG     0xB1D3u
#define BW_WS_MARK(op, arg) ((BW_WS_MARK_TAG << 16) | ((uint32_t)(op) << 8) | ((uint32_t)(arg) & 0xFFu))
enum {
    BW_WS_OP_2D_BEGIN = 1,      /* arg = what: BW_WS_WHAT_* */
    BW_WS_OP_2D_END   = 2,
    BW_WS_OP_3D_43    = 3,      /* arg = 1: this view's 3D is 4:3 (pillarboxed) */
};
#define BW_WS_WHAT_OVERLAY 0x20u   /* Overlay_FlushBatches: the HUD, menus, text */
#define BW_WS_WHAT_MOVIE 0x40u     /* Movie_DrawQuad: a Bink frame */

#endif
