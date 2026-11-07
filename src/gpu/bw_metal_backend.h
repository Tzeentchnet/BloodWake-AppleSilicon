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

typedef enum {
    BW_METAL_WATER_RETAIL = 0,
    BW_METAL_WATER_REALISTIC = 1,
} BwMetalWaterPreset;

/* Register only while the executor is idle, before start or after stop.
 * The callback runs synchronously on the executor thread while preparing a
 * presentation descriptor; direct presentation does not first publish private
 * pixels to guest memory. Copy only the descriptor during the call; its
 * dimensions are physical samples and its pitch is in bytes. CPU-visible
 * snapshots and dumps use a separate explicit publication boundary. Return
 * zero to reject the descriptor. A NULL callback or backend stop removes the
 * registration. */
typedef int (*BwMetalFramebufferCallback)(const BwMetalFramebuffer *framebuffer);
void bw_metal_backend_set_framebuffer_callback(BwMetalFramebufferCallback callback);

/* Attach the CAMetalLayer owned by the main-thread SDL presenter before
 * starting the backend. Direct presentation keeps the completed frame on the
 * GPU; guest-memory synchronization still happens for executor snapshots and
 * framebuffer dumps. The layer pointer is opaque here so C callers do not
 * depend on Objective-C headers. Returns nonzero on success. */
int bw_metal_backend_attach_presenter(void *metal_layer, uint32_t drawable_width,
                                      uint32_t drawable_height, int vsync);
/* Main-thread only. Zero dimensions (a minimized window) are accepted without
 * changing the last drawable size. Returns zero for an invalid call. */
int bw_metal_backend_resize_presenter(uint32_t drawable_width,
                                      uint32_t drawable_height);

/* Request Metal anisotropy for filtered textures with mip chains. The value
 * must be 1, 2, 4, 8 or 16. Immutable sampler variants are selected at the
 * next executor flip; point-sampled and unmipped textures remain 1x. */
int bw_metal_backend_set_anisotropy(unsigned anisotropy);
unsigned bw_metal_backend_anisotropy(void);

/* Request the optional marked-ocean fragment pipeline. Retail is the exact
 * default and fallback. Realistic is Remastered-only and activates at the next
 * executor flip; unmarked and unsupported marked batches always use retail. */
int bw_metal_backend_set_water_preset(BwMetalWaterPreset preset);
BwMetalWaterPreset bw_metal_backend_water_preset(void);

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
 * presentation, readback or surface-ownership boundary. Full clears can share
 * the first draw encoder when the attachments match. Mutable vertex, upload,
 * feedback-resolve, and readback resources belong to one of at most three
 * in-flight submissions and are reused only after their command completes.
 * Up to 1024 decoded textures are cached by guest span and layout. Guest
 * contents are checked at most once per frame; stable entries use distributed
 * samples with staggered full verification. Render-target writeback
 * invalidates overlapping entries.
 * With BW_METAL_FRAME_TIMING=1, the backend reports monotonic wall-clock,
 * completed-command, exact command-reason, wait, submission-latency,
 * command-role, and feedback CPU statistics for rolling 300-frame windows.
 * The same opt-in emits bloodwake.metal-presentation-timing/v1 JSONL for at
 * most 512 flips. Each completed record is streamed once and correlates flip
 * stall/release, drawable acquisition, present request, command
 * commit/completion, the actual-presentation callback, and exact consecutive
 * actual-present intervals on the monotonic host clock. The current explicit
 * presenter has no display-target timestamp, so it reports
 * unavailable_explicit_presenter and null rather than inferring a refresh
 * rate. Invalid or unavailable actual times remain explicit. Stop waits at
 * most one second for owned timing callbacks, then reports any pending count.
 * With BW_METAL_PIPELINE_TIMING=1, the backend emits bounded
 * bloodwake.metal-pipeline-lifecycle/v2 JSONL as event frames complete and at
 * stop. The exact-value opt-in records at most 512 offline-library load,
 * runtime-library compile, cache-miss/reset, function-specialization, and
 * render-pipeline events; cache hits remain aggregate-only. Records contain
 * canonical-key hashes, durations, frame/command classifications, aggregate
 * cache counters, wall-frame correlation, artifact selection/fallback state,
 * and explicit errors, never descriptors or retail-derived fields.
 * BW_METAL_PIPELINE_TRACE_OUTPUT may name a bounded absolute .bwmpa path to
 * opt into capture of at most 128 successful canonical draw keys. The backend
 * writes a deterministic owner-only device-local artifact atomically at frame
 * boundaries. BW_METAL_PIPELINE_WARMUP may name such an artifact to validate
 * and preload its exact states before the first draw. A compatible preload
 * also enables a 32-entry coalescing queue for untraced specialized misses:
 * the existing exact generic extended shader renders while Metal specializes
 * and creates the requested state asynchronously, and only the serial executor
 * installs completed states. Stop waits for callbacks before releasing their
 * state. Failures remain explicit and retain the generic path. The bounded
 * bloodwake.metal-pipeline-async/v1 report contains aggregate counts and
 * timing only. Both options are unset by default. Missing, stale, corrupt,
 * invalid, or failed warm-up data emits bounded
 * bloodwake.metal-pipeline-warmup/v1 aggregate JSONL and uses the unchanged
 * synchronous runtime path. Reports never contain paths, keys, descriptors,
 * function constants, addresses, or texture identities.
 * Up to 32 exact colour-surface descriptions are cached. Ordinary target
 * switches retain each touched private generation in the current submission;
 * presentation commits them together in queue order, while eight switches
 * force a bounded fallback commit. Reactivation reuses a texture only while
 * guest bytes still match its exact synchronization snapshot. Exact tracked
 * XRGB anti-aliased quincunx feedback resolves the newest private generation
 * directly on the GPU. A detected CPU change waits, publishes, merges changed
 * 32-bit pixels, and reseeds the private texture. Other texture feedback,
 * snapshots, dumps, scale changes, captures, and CPU-visible paths explicitly
 * wait and publish.
 * Extended fragment pipelines specialize immutable texture-stage, combiner,
 * final-combiner, and alpha-test routing with Metal function constants while
 * retaining dynamic material constants and reference values.
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

#if defined(BW_METAL_TESTING)
typedef struct {
    int enabled;
    size_t event_count, event_capacity;
    uint64_t events_dropped;
    uint64_t cache_hits, cache_misses, whole_cache_resets;
    unsigned cache_entry_high_water;
    int library_artifact_available, library_artifact_loaded;
    uint64_t library_load_attempts, library_load_timed;
    uint64_t library_fallbacks, library_unavailable, library_rejections;
    uint64_t library_attempts, library_timed;
    uint64_t specialization_attempts, specialization_timed;
    uint64_t pipeline_attempts, pipeline_timed;
    uint64_t timing_unavailable, timing_non_monotonic;
    uint64_t wall_timing_unavailable, wall_timing_non_monotonic;
    uint64_t allocation_failures, metal_failures;
    uint64_t wall_frame_intervals, wall_frame_max_ns;
    uint32_t wall_frame_max_frame;
    size_t events_on_wall_max;
} BwMetalPipelineDiagnosticSnapshot;

typedef struct {
    int load_enabled, trace_enabled, identity_ready;
    int load_compatible, runtime_fallback;
    int load_artifact_status, load_payload_status, trace_write_status;
    size_t load_entries, preloaded_entries, trace_entries;
    uint64_t trace_entries_dropped;
    uint64_t trace_write_attempts, trace_write_successes;
    uint64_t trace_write_failures;
} BwMetalPipelineWarmupSnapshot;

typedef struct {
    int requested, enabled, initialization_failed;
    size_t entries, pending, ready, failed, capacity;
    uint64_t scheduled, coalesced_misses, generic_fallback_draws;
    uint64_t installed, function_failures, pipeline_failures;
    uint64_t allocation_failures, capacity_fallbacks;
    uint64_t function_timed, function_total_ns, function_max_ns;
    uint64_t pipeline_timed, pipeline_total_ns, pipeline_max_ns;
    uint64_t timing_unavailable, timing_non_monotonic;
} BwMetalPipelineAsyncSnapshot;

typedef struct {
    int enabled, initialization_failed;
    size_t event_count, event_capacity;
    uint64_t events_seen, events_dropped;
    uint64_t flip_releases, drawables_acquired, drawables_unavailable;
    uint64_t present_requests, commands_completed, commands_failed;
    uint64_t actual_presented, actual_unavailable, actual_invalid;
    uint64_t actual_interval_count, actual_interval_min_ns;
    uint64_t actual_interval_max_ns, ordering_failures;
    uint64_t callback_timeouts, allocation_failures;
} BwMetalPresentationTimingSnapshot;

void bw_metal_test_pipeline_diagnostic_snapshot(
    BwMetalPipelineDiagnosticSnapshot *snapshot);
void bw_metal_test_pipeline_warmup_snapshot(
    BwMetalPipelineWarmupSnapshot *snapshot);
void bw_metal_test_pipeline_async_snapshot(
    BwMetalPipelineAsyncSnapshot *snapshot);
void bw_metal_test_presentation_timing_snapshot(
    BwMetalPresentationTimingSnapshot *snapshot);
size_t bw_metal_test_timing_sample_capacity(void);
void bw_metal_test_timing_percentile_report(void);
void bw_metal_test_pipeline_diagnostic_clear(void);
void bw_metal_test_pipeline_diagnostic_inject_cache_miss(uint64_t key_hash,
                                                         uint32_t frame);
void bw_metal_test_fail_next_pipeline_allocation(void);
void bw_metal_test_fail_warmup_pipeline(void);
void bw_metal_test_hold_async_pipeline_install(int hold);
void bw_metal_test_drain_async_pipelines(void);
void bw_metal_test_async_pipeline_failures(int allocation,
                                           int function,
                                           int pipeline);
void bw_metal_test_fail_presentation_record_allocation(void);
void bw_metal_test_presentation_inject(uint32_t frame, uint64_t base_ns,
                                       int presented);
void bw_metal_test_presentation_schedule_callbacks(uint32_t frame,
                                                   uint64_t base_ns);
void bw_metal_test_shader_library_failures(int offline_library,
                                           int source_library);
const char *bw_metal_test_shader_source_sha256(void);
int bw_metal_test_pipeline_diagnostic_format_event(char *buffer, size_t size);
int bw_metal_test_pipeline_diagnostic_format_summary(char *buffer, size_t size);
int bw_metal_test_pipeline_async_format_summary(char *buffer, size_t size,
                                                int final);
int bw_metal_test_presentation_format_event(size_t index,
                                            uint64_t previous_actual_ns,
                                            char *buffer, size_t size);
int bw_metal_test_presentation_format_summary(char *buffer, size_t size);
#endif

#ifdef __cplusplus
}
#endif

#endif
