#ifndef BW_METAL_BACKEND_H
#define BW_METAL_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BW_METAL_PIXEL_BGRA8 = 1,
    BW_METAL_PIXEL_BGRX8_ZERO = 2,
} BwMetalPixelFormat;

typedef struct {
    uint32_t guest_va, pitch, width, height, pixel_format, aa_sx, aa_sy;
} BwMetalFramebuffer;

/* Register only while the executor is idle, before start or after stop.
 * The callback runs synchronously on the executor thread after readback and
 * before the snapshot. Copy the descriptor during the call; its dimensions
 * are physical samples and its pitch is in bytes. Return zero to reject the
 * snapshot. A NULL callback or backend stop removes the registration. */
typedef int (*BwMetalFramebufferCallback)(const BwMetalFramebuffer *framebuffer);
void bw_metal_backend_set_framebuffer_callback(BwMetalFramebufferCallback callback);

/* Start after the toolkit has mapped guest memory and before the guest submits
 * GPU work. The host must set RECOMP_PB_EXEC before the toolkit starts its
 * poll thread; it is presence-based, so even "0" enables execution. Executor
 * callbacks run serially on that thread. Stop only after the thread has
 * stopped; stop does not access guest memory. Returns nonzero on success.
 * Clipped clears support single-sample BGRA8 and pitch-layout fixed-zero
 * BGRX8 at single-sample or horizontal-supersample resolution, with
 * pitch-layout Z24S8 depth/stencil textures.
 * Extended draws support a single general register-combiner stage and final
 * combiner using unsigned identity/invert mappings and scratch destinations,
 * with projective 2D, single-level BC1 textures, clamp-to-edge and linear
 * filtering on devices supporting BC compression. Projective divisors and
 * reciprocal clip W must be finite and positive. Other extended combiner or
 * texture programs, and unhandled render states, report sticky errors.
 * By default, sampling pitch-linear textures and convolution filters are
 * rejected. Set BW_METAL_QUINCUNX_APPROX=1 before start to opt into one
 * exception: a 2D pitch-linear XRGB source with both filters set to quincunx
 * convolution, clamp-to-edge addressing, and dimensions twice the width and
 * the same height as a single-sample pitch-linear XRGB destination, with
 * blending disabled. This uses the legacy CPU path's 3x3 Gaussian of bilinear
 * taps and 8-bit half-up output rounding, not verified NV2A
 * quincunx behavior. The backend logs a warning on each enabled start and
 * reports the first successful approximation draw.
 * Other convolution states remain rejected. The option is sampled at start;
 * unset it or set any value other than exactly "1" for strict rejection.
 * Quincunx hardware weights, edge rules and rounding have not been verified.
 * Exact hardware sample positions and reconstruction filtering are unverified;
 * the framebuffer consumer chooses the resolve filter.
 * Depth-tested drawing and CPU or texture access to cleared guest depth
 * surfaces remain unsupported; an incomplete clear rectangle is rejected. */
int bw_metal_backend_start(void);

/* Synthetic flat memory for focused tests; it must match the executor's base. */
int bw_metal_backend_start_for_test(void *guest_memory, size_t guest_size);

/* The first error is sticky and can be read from any thread. Its string stays
 * valid until the next start; start/stop must not race with another start. */
const char *bw_metal_backend_error(void);
void bw_metal_backend_stop(void);

#ifdef __cplusplus
}
#endif

#endif
