#ifndef BW_FRAMEBUFFER_METAL_H
#define BW_FRAMEBUFFER_METAL_H

#include "../../gpu/bw_metal_backend.h"

/* Enable descriptor matching before starting the Metal executor. */
void bw_sdl_framebuffer_expect_metal(void);

/* Metal calls this synchronously before a possible present on the same thread.
 * The presenter copies the descriptor; a later descriptor replaces an unused
 * one, and an active present consumes it exactly once. */
int bw_sdl_framebuffer_prepare_metal(const BwMetalFramebuffer *framebuffer);

#endif
