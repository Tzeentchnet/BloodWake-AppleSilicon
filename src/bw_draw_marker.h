/*
 * In-stream draw markers written through the title's own D3D push buffer.
 * They cross the game/executor thread boundary in command order; no host-side
 * flag is shared between those threads.
 */
#ifndef BW_DRAW_MARKER_H
#define BW_DRAW_MARKER_H

#include <stdint.h>

#define BW_DRAW_MARKER_NOP_HEADER 0x00040100u
#define BW_DRAW_MARKER_TAG        0xB1D3u
#define BW_DRAW_MARKER_MAKE(op, arg) \
    ((BW_DRAW_MARKER_TAG << 16) | ((uint32_t)(op) << 8) | ((uint32_t)(arg) & 0xFFu))

enum {
    BW_DRAW_MARKER_OP_2D_BEGIN = 1,
    BW_DRAW_MARKER_OP_2D_END = 2,
    BW_DRAW_MARKER_OP_3D_43 = 3,
    BW_DRAW_MARKER_OP_OCEAN_DRAW = 4
};

#define BW_DRAW_MARKER_OCEAN_VERSION 1u
#define BW_DRAW_MARKER_OCEAN_DRAW \
    BW_DRAW_MARKER_MAKE(BW_DRAW_MARKER_OP_OCEAN_DRAW, BW_DRAW_MARKER_OCEAN_VERSION)

/* Appends one tagged NOP through D3D and preserves the guest call state. */
void bw_draw_marker_emit(uint32_t param);

#endif
