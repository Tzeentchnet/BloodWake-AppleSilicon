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
    BW_DRAW_MARKER_OP_OCEAN_DRAW = 4,
    BW_DRAW_MARKER_OP_WATER_EVENT = 5
};

enum {
    BW_DRAW_MARKER_WATER_VIEW_BEGIN = 1,
    BW_DRAW_MARKER_WATER_LIT_DRAW = 2,
    BW_DRAW_MARKER_WATER_REFLECTION_DRAW = 3,
    BW_DRAW_MARKER_WATER_FOAM_DRAW = 4,
    BW_DRAW_MARKER_WATER_VIEW_END = 5,
    BW_DRAW_MARKER_WATER_PASS_RUN_BEGIN = 6,
    BW_DRAW_MARKER_WATER_PASS_RUN_END = 7
};

enum {
    BW_DRAW_MARKER_WATER_PASS_LIT = 1,
    BW_DRAW_MARKER_WATER_PASS_REFLECTION = 2,
    BW_DRAW_MARKER_WATER_PASS_FOAM = 3
};

#define BW_DRAW_MARKER_OCEAN_LEGACY_VERSION 1u
#define BW_DRAW_MARKER_OCEAN_DRAW_LEGACY \
    BW_DRAW_MARKER_MAKE(BW_DRAW_MARKER_OP_OCEAN_DRAW, \
                        BW_DRAW_MARKER_OCEAN_LEGACY_VERSION)

/* Water-event argument: version[7:5], event[4:2], view/payload[1:0].
 * Draw events inherit the active view and require a zero payload. Pass-run
 * events carry BW_DRAW_MARKER_WATER_PASS_* and classify every draw until the
 * matching run end. */
#define BW_DRAW_MARKER_WATER_VERSION 1u
#define BW_DRAW_MARKER_WATER_ARG_VERSION(version, event, payload) \
    ((((uint32_t)(version) & 7u) << 5) | \
     (((uint32_t)(event) & 7u) << 2) | ((uint32_t)(payload) & 3u))
#define BW_DRAW_MARKER_WATER_ARG(event, payload) \
    BW_DRAW_MARKER_WATER_ARG_VERSION(BW_DRAW_MARKER_WATER_VERSION, event, payload)
#define BW_DRAW_MARKER_WATER_MAKE(event, payload) \
    BW_DRAW_MARKER_MAKE(BW_DRAW_MARKER_OP_WATER_EVENT, \
                        BW_DRAW_MARKER_WATER_ARG(event, payload))
#define BW_DRAW_MARKER_WATER_BEGIN(view) \
    BW_DRAW_MARKER_WATER_MAKE(BW_DRAW_MARKER_WATER_VIEW_BEGIN, view)
#define BW_DRAW_MARKER_WATER_LIT \
    BW_DRAW_MARKER_WATER_MAKE(BW_DRAW_MARKER_WATER_LIT_DRAW, 0)
#define BW_DRAW_MARKER_WATER_REFLECTION \
    BW_DRAW_MARKER_WATER_MAKE(BW_DRAW_MARKER_WATER_REFLECTION_DRAW, 0)
#define BW_DRAW_MARKER_WATER_FOAM \
    BW_DRAW_MARKER_WATER_MAKE(BW_DRAW_MARKER_WATER_FOAM_DRAW, 0)
#define BW_DRAW_MARKER_WATER_END(view) \
    BW_DRAW_MARKER_WATER_MAKE(BW_DRAW_MARKER_WATER_VIEW_END, view)
#define BW_DRAW_MARKER_WATER_PASS_BEGIN(pass) \
    BW_DRAW_MARKER_WATER_MAKE(BW_DRAW_MARKER_WATER_PASS_RUN_BEGIN, pass)
#define BW_DRAW_MARKER_WATER_PASS_END(pass) \
    BW_DRAW_MARKER_WATER_MAKE(BW_DRAW_MARKER_WATER_PASS_RUN_END, pass)

/* The validated Ocean_FillVertices boundary currently identifies pass 1. */
#define BW_DRAW_MARKER_OCEAN_DRAW BW_DRAW_MARKER_WATER_LIT

/* Appends one tagged NOP through D3D and preserves the guest call state. */
void bw_draw_marker_emit(uint32_t param);

#endif
