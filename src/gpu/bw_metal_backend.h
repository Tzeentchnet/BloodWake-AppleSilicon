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
 * Extended draws support up to eight general register-combiner stages and a final
 * combiner using the DirectX reference's constants, final special registers,
 * input and general-stage output mappings, dot products, mux, blue-to-alpha
 * and scratch destinations, plus up to four independent projective 2D,
 * BC1, BC2, BC3 or swizzled R5G6B5, A8R8G8B8 or X8R8G8B8 texture stages
 * with up to 12 mip levels, or pitch-linear A8R8G8B8 and X8R8G8B8 stages,
 * independently selected wrap, mirror or clamp-to-edge addressing, and
 * point, linear, nearest-mip or trilinear filtering. The executor's perspective-correct
 * fog factor and fog colour are available as NV2A combiner input 3.
 * Projective 2D, pass-through, and the title's dot-product diffuse/specular
 * reflection texture-stage programs are supported. Reflection stages accept
 * normalized cube textures and use explicit Direct3D-compatible face
 * selection over six Metal 2D-array slices.
 * The BC formats require device BC-compression support. Mipped filtering
 * requires zero LOD bias and a clamp covering
 * the full uploaded chain.
 * Inactive stages produce (0, 0, 0, 1). Active-stage projective divisors must
 * be finite; zero is interpreted as one, matching the upstream DX11 path.
 * A projected texture-coordinate vector with a non-finite component is
 * sampled at zero.
 * Reciprocal clip W must be finite and positive. Other extended combiner or
 * texture programs, and unhandled render states, report sticky errors.
 * Filled-polygon depth bias follows the upstream DX11 scale factor and
 * integer-rounded constant-unit behavior.
 * By default, convolution filters are rejected. Set
 * BW_METAL_QUINCUNX_APPROX=1 before start to opt into one exception: a 2D
 * pitch-linear XRGB source with both filters set to quincunx
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
 * Draws and clears are recorded into one pending command buffer until a
 * presentation, readback or surface-ownership boundary. Draws with matching
 * colour and depth attachments also share a render encoder. Vertex data is
 * suballocated from a shared upload arena that is reused only after the
 * pending command buffer completes.
 * Up to 1024 decoded textures are cached by guest span and layout. Guest
 * contents are checked at most once per frame; stable entries use distributed
 * samples with staggered full verification. Render-target writeback
 * invalidates overlapping entries.
 * With BW_GPU_AUDIT=1, the backend reports monotonic wall-clock statistics for
 * a rolling 300-frame window every 100 flips.
 * Up to 32 exact colour-surface descriptions are cached. Switching targets
 * after GPU writes synchronously copies the completed old target to guest
 * memory, then reseeds the cached new target from guest memory. This permits
 * ordered render-target sampling, including the title's AA resolve, without
 * assuming that an inactive cached texture still owns authoritative data.
 * Depth-tested and stencil-tested draws use pitch-layout Z24S8 surfaces,
 * including all NV2A comparison functions, depth writes, stencil masks and
 * keep, zero, replace, clamp, invert and wrap operations.
 * Sampling a guest-address span overlapping the active colour surface is
 * rejected, including before its first draw. These span checks do not prove
 * physical alias identity or synchronize guest CPU access. CPU or texture
 * access to GPU-owned depth surfaces remains unsupported; an incomplete
 * clear rectangle is rejected. */
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
