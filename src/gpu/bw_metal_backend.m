#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>

#include <float.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "bw_metal_backend.h"
#include "bw_backend_sync.h"
#include "bw_gamma.h"
#include "bw_metal_shader_data.inc"
#include "../bw_edition.h"
#include "d3d/d3d8_swizzle.h"
#include "../toolkit_patches/bw_runtime_lifecycle.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xbox_GetMemoryBase(void);
extern size_t xbox_GetMappedSize(void);
extern int bw_ab_off(const char *fix);
extern void bw_ab_frame(uint32_t frame);
extern uint64_t xbox_HostNowNs(void);

#define METAL_MAX_DIMENSION 16384u
#define METAL_MAX_PIPELINES 128u
#define METAL_MAX_DEPTH_STATES 128u
#define METAL_MAX_COLOR_SURFACES 32u
#define METAL_MAX_DEPTH_SURFACES 32u
#define METAL_MAX_FRAME_SAMPLES 16384u
#define METAL_MAX_IN_FLIGHT 3u
#define METAL_MAX_TARGET_SWITCHES_PER_SUBMISSION 8u
#define METAL_INITIAL_VERTEX_UPLOAD (16u * 1024u * 1024u)
#define METAL_COLOR_XRGB_ZERO 0x4u
#define METAL_COLOR_ARGB8 0x8u
#define METAL_PIPELINE_EVENT_CAPACITY 512u
#define METAL_PIPELINE_SUMMARY_CAPACITY 64u
#define METAL_PIPELINE_REPORT_LINE_MAX 2048u
#define METAL_PIPELINE_SCHEMA "bloodwake.metal-pipeline-lifecycle/v2"
#if defined(BW_METAL_TESTING)
#define METAL_WATER_BENCHMARK_WARMUP_FRAMES 1u
#define METAL_WATER_BENCHMARK_SAMPLE_FRAMES 2u
#else
#define METAL_WATER_BENCHMARK_WARMUP_FRAMES 10u
#define METAL_WATER_BENCHMARK_SAMPLE_FRAMES 50u
#endif
#define METAL_WATER_BENCHMARK_BLOCKS 4u
#define METAL_WATER_BENCHMARK_SAMPLES_PER_PRESET \
    (METAL_WATER_BENCHMARK_SAMPLE_FRAMES * 2u)
#define METAL_WATER_CAPTURE_IMAGES 4u

enum {
    METAL_COMMAND_REASON_RENDER = 1u << 0,
    METAL_COMMAND_REASON_TARGET_SWITCH = 1u << 1,
    METAL_COMMAND_REASON_SURFACE_UPLOAD = 1u << 2,
    METAL_COMMAND_REASON_SURFACE_READBACK = 1u << 3,
    METAL_COMMAND_REASON_FEEDBACK = 1u << 4,
    METAL_COMMAND_REASON_PRESENT = 1u << 5,
    METAL_COMMAND_REASON_DEPTH_COPY = 1u << 6,
};

typedef struct {
    float position[4];
    float color[4];
    float texcoord[4];
} MetalVertex;

typedef struct {
    float surface_texture[4];
    float depth[4];
    float alpha[4];
} MetalUniforms;

typedef struct {
    uint32_t width, height, aa_sx, aa_sy;
} MetalResolveUniforms;

typedef struct {
    uint32_t source_width, source_height, destination_width, destination_height;
} MetalScaleUniforms;

typedef struct {
    uint32_t gamma_enabled, padding[3];
} MetalPresentUniforms;

typedef struct {
    MetalUniforms draw;
    float fog_color[4];
    uint32_t combiner[8][4];
    uint32_t factors[8][4];
    uint32_t final[4];
    uint32_t texture[4];
    uint32_t stages[4];
    uint32_t dot[4];
} MetalExtendedUniforms;

typedef struct {
    uint32_t textured, color_mask, fixed_alpha_zero;
    uint32_t blend_enable, blend_src, blend_dst, blend_eq;
    uint32_t depth_attachment;
    uint32_t specialized;
    uint32_t texture[4], stages[4], dot[4];
    uint32_t combiner[8][4], final[2], alpha[2];
} MetalPipelineKey;

typedef struct {
    uint32_t texture[4], stages[4], dot[4];
    uint32_t combiner[8][4], final[2], alpha[2];
} MetalExtendedSpecialization;

enum {
    METAL_SHADER_COLOR,
    METAL_SHADER_TEXTURE,
    METAL_SHADER_EXTENDED,
    METAL_SHADER_REALISTIC_WATER,
    METAL_SHADER_DEPTH_ONLY,
};

typedef enum {
    METAL_PIPELINE_EVENT_LIBRARY_LOAD,
    METAL_PIPELINE_EVENT_LIBRARY_COMPILE,
    METAL_PIPELINE_EVENT_FUNCTION_SPECIALIZATION,
    METAL_PIPELINE_EVENT_PIPELINE_CREATE,
    METAL_PIPELINE_EVENT_CACHE_MISS,
    METAL_PIPELINE_EVENT_CACHE_RESET,
} MetalPipelineEventKind;

typedef enum {
    METAL_PIPELINE_ROLE_STARTUP,
    METAL_PIPELINE_ROLE_RENDER,
} MetalPipelineCommandRole;

typedef enum {
    METAL_PIPELINE_STATUS_OK,
    METAL_PIPELINE_STATUS_ARTIFACT_UNAVAILABLE,
    METAL_PIPELINE_STATUS_ARTIFACT_REJECTED,
    METAL_PIPELINE_STATUS_ALLOCATION_FAILED,
    METAL_PIPELINE_STATUS_METAL_FAILED,
} MetalPipelineStatus;

typedef enum {
    METAL_PIPELINE_TIMING_NOT_APPLICABLE,
    METAL_PIPELINE_TIMING_PENDING,
    METAL_PIPELINE_TIMING_OK,
    METAL_PIPELINE_TIMING_UNAVAILABLE,
    METAL_PIPELINE_TIMING_NON_MONOTONIC,
    METAL_PIPELINE_TIMING_NOT_COMPLETED,
} MetalPipelineTimingStatus;

typedef struct {
    uint64_t start_ns;
    MetalPipelineTimingStatus status;
} MetalPipelineTimer;

typedef struct {
    uint64_t key_hash, duration_ns, wall_frame_ns;
    uint32_t frame;
    MetalPipelineEventKind kind;
    MetalPipelineCommandRole role;
    MetalPipelineStatus status;
    MetalPipelineTimingStatus timing_status, wall_timing_status;
} MetalPipelineEvent;

typedef struct {
    uint64_t attempts, timed, total_ns, max_ns;
} MetalPipelineTimingAggregate;

typedef struct {
    int enabled, final_reported, summary_dirty;
    MetalPipelineEvent events[METAL_PIPELINE_EVENT_CAPACITY];
    size_t event_count, events_reported;
    unsigned summaries_reported;
    uint64_t events_dropped;
    uint64_t cache_hits, cache_misses, whole_cache_resets;
    unsigned cache_entry_high_water;
    MetalPipelineTimingAggregate library_load, library, specialization, pipeline;
    uint64_t library_fallbacks, library_unavailable, library_rejections;
    int library_artifact_loaded;
    uint64_t timing_unavailable, timing_non_monotonic;
    uint64_t wall_timing_unavailable, wall_timing_non_monotonic;
    uint64_t allocation_failures, metal_failures;
    uint64_t wall_frame_intervals, wall_frame_max_ns;
    uint32_t wall_frame_max_frame;
} MetalPipelineDiagnostic;

typedef struct {
    uint32_t depth_test, depth_func, depth_write, stencil_enable;
    uint32_t stencil_func, stencil_func_mask, stencil_write_mask;
    uint32_t stencil_fail, stencil_zfail, stencil_zpass;
} MetalDepthStateKey;

typedef struct {
    uint64_t start, end;
} MetalGuestRegion;

typedef struct {
    uint32_t color_va, width, height, pitch;
    uint32_t physical_width, physical_height;
    uint32_t format, bytes_per_pixel, aa_sx, aa_sy, scale_epoch;
} MetalColorSurfaceKey;

@interface BWMetalColorSurface : NSObject {
@public
    id<MTLTexture> texture;
    NSMutableData *guest_sync;
    Nv2aSurface surface;
    uint32_t format;
    size_t row;
    int sync_valid;
    int private_dirty;
    uint64_t submitted_generation;
    uint64_t completed_generation;
    uint64_t published_generation;
}
@end
@implementation BWMetalColorSurface
@end

@interface BWMetalDepthSurface : NSObject {
@public
    id<MTLTexture> texture;
    uint32_t va, width, height, guest_width, guest_height, pitch, scale_epoch;
    uint64_t end;
    unsigned initialized;
}
@end
@implementation BWMetalDepthSurface
@end

typedef struct {
    uint32_t offset, width, height, pitch;
    uint32_t color, levels, cube;
} MetalTextureKey;

@interface BWMetalTextureEntry : NSObject {
@public
    id<MTLTexture> texture;
    uint64_t hash, sparse, end;
    uint32_t offset, frame, last_use, stable;
}
@end
@implementation BWMetalTextureEntry
@end

@interface BWMetalSubmission : NSObject {
@public
    id<MTLCommandBuffer> command;
    id<MTLBuffer> vertex_upload;
    size_t vertex_upload_capacity;
    size_t vertex_upload_offset;
    id<MTLTexture> surface_upload;
    id<MTLTexture> surface_readback;
    id<MTLTexture> feedback_resolve;
    BWMetalColorSurface *__strong color_entries[METAL_MAX_COLOR_SURFACES];
    uint64_t color_generations[METAL_MAX_COLOR_SURFACES];
    size_t color_entry_count;
    unsigned target_switches;
    CFTimeInterval commit_time;
    uint32_t frame;
    uint32_t reason_mask;
    int water_command;
    int presented;
#if defined(BW_METAL_TESTING)
    int test_failure;
#endif
}
@end
@implementation BWMetalSubmission
@end

static uintptr_t s_guest;
static MetalGuestRegion s_guest_regions[2];
static char s_error[512];
static atomic_int s_error_ready;
static int s_registered;
static int s_dirty;
static int s_quincunx_approx;
static int s_quincunx_reported;
static float s_render_scale = 1.0f;
static float s_render_fixed_scale = 1.0f;
static float s_render_ssaa = 1.0f;
static unsigned s_render_scale_epoch = 1;
static int s_render_scale_auto;
static int s_render_guest_aa;
static uint32_t s_frame;
static uint32_t s_current_format;
static Nv2aSurface s_current;
static BwMetalFramebufferCallback s_framebuffer_callback;
static CAMetalLayer *s_present_layer;
static atomic_uint s_present_width;
static atomic_uint s_present_height;
static id<MTLTexture> s_present_surface;
static BwMetalFramebuffer s_present_framebuffer;
static BWMetalColorSurface *s_surface_entry;
static id<MTLDevice> s_device;
static id<MTLCommandQueue> s_queue;
static id<MTLCommandBuffer> s_pending_command;
static BWMetalSubmission *s_encoding_submission;
static NSMutableArray<BWMetalSubmission *> *s_inflight_submissions;
static NSMutableArray<BWMetalSubmission *> *s_free_submissions;
static id<MTLLibrary> s_shader_library;
static id<MTLRenderCommandEncoder> s_draw_encoder;
static id<MTLTexture> s_draw_color;
static id<MTLTexture> s_draw_depth;
static uint64_t s_command_submissions;
static uint64_t s_draw_encoder_creations;
static uint64_t s_vertex_upload_allocations;
static id<MTLFunction> s_vertex_function;
static id<MTLFunction> s_color_function;
static id<MTLFunction> s_texture_function;
static id<MTLFunction> s_extended_vertex_function;
static id<MTLFunction> s_extended_fragment_function;
static id<MTLFunction> s_realistic_water_fragment_function;
static id<MTLFunction> s_clear_vertex_function;
static id<MTLFunction> s_clear_color_function;
static id<MTLFunction> s_clear_depth_function;
static id<MTLFunction> s_present_vertex_function;
static id<MTLFunction> s_present_resolve_function;
static id<MTLFunction> s_present_fragment_function;
static id<MTLFunction> s_scale_function;
static id<MTLFunction> s_box_function;
static id<MTLRenderPipelineState> s_present_resolve_pipeline;
static id<MTLRenderPipelineState> s_present_pipeline;
static id<MTLRenderPipelineState> s_scale_pipeline;
static id<MTLRenderPipelineState> s_box_pipeline;
static id<MTLSamplerState> s_present_sampler;
static uint64_t s_surface_seed_count;
static uint64_t s_surface_cache_reuses;
static uint64_t s_present_cache_reuses;
static uint64_t s_present_guest_reseeds;
static id<MTLTexture> s_present_resolve;
static id<MTLTexture> s_present_gamma;
static uint8_t s_present_gamma_bytes[1024];
static int s_present_gamma_valid;
static uint64_t s_presented_frames;
static uint64_t s_dropped_drawables;
static uint64_t s_surface_generation;
static uint64_t s_async_display_frames;
static uint64_t s_guest_publications;
static uint64_t s_guest_publication_bytes;
static uint64_t s_queue_saturation_waits;
static uint64_t s_queue_saturation_wait_ns;
static unsigned s_queue_depth_high_water;
static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *s_pipelines;
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *s_clear_pipelines;
static NSMutableDictionary<NSData *, BWMetalColorSurface *> *s_color_surfaces;
static NSMutableDictionary<NSData *, BWMetalTextureEntry *> *s_textures;
static NSMutableDictionary<NSNumber *, BWMetalDepthSurface *> *s_depth_surfaces;
static NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *s_depth_states;
static id<MTLDepthStencilState> s_clear_depth_states[4];
static id<MTLSamplerState> s_sampler;
static id<MTLSamplerState> s_samplers[5][3][3][2][2][3];
static atomic_uint s_requested_anisotropy;
static unsigned s_active_anisotropy = 1;
static atomic_uint s_requested_water_preset;
static unsigned s_active_water_preset;
static uint64_t s_realistic_water_batches;
static uint64_t s_realistic_water_fallback_batches;
static int s_realistic_water_reported;
static int s_realistic_water_fallback_reported;
static int s_water_reflect_single_enabled;
static int s_water_reflect_single_failed;
static int s_water_reflect_single_reported;
static int s_water_reflect_active;
static int s_water_reflect_primed;
static uint8_t s_water_reflect_view;
static BWMetalDepthSurface *s_water_reflect_depth;
static BWMetalDepthSurface *s_water_reflect_source;
static id<MTLDepthStencilState> s_water_reflect_prime_state;
static id<MTLDepthStencilState> s_water_reflect_test_state;
static uint64_t s_water_reflect_copies;
static uint64_t s_water_reflect_prime_draws;
static uint64_t s_water_reflect_filtered_draws;
#if defined(BW_METAL_TESTING)
static int s_water_reflect_test_failure;
static int s_test_async_display;
static int s_test_defer_retirement;
static int s_test_fail_next_submission;
static int s_test_fail_next_pipeline_allocation;
static int s_test_fail_offline_library;
static int s_test_fail_source_library;
#endif
static id<MTLTexture> s_surface;
static id<MTLTexture> s_inactive_texture;
static id<MTLTexture> s_inactive_cube_texture;
static uint64_t s_texture_uploads;
static uint64_t s_frame_times[METAL_MAX_FRAME_SAMPLES];
static size_t s_frame_time_count;
static uint64_t s_gpu_command_times[METAL_MAX_FRAME_SAMPLES];
static size_t s_gpu_command_time_count;
static uint64_t s_command_wait_times[METAL_MAX_FRAME_SAMPLES];
static size_t s_command_wait_time_count;
static uint64_t s_submission_latency_times[METAL_MAX_FRAME_SAMPLES];
static size_t s_submission_latency_time_count;
static uint64_t s_frame_gpu_command_ns;
static uint64_t s_frame_command_wait_ns;
static int s_frame_gpu_command_valid;
static uint64_t s_present_command_gpu_ns;
static uint64_t s_offscreen_command_gpu_ns;
static uint64_t s_present_command_count;
static uint64_t s_offscreen_command_count;
typedef struct {
    uint32_t color_va, width, height;
    int presented;
    uint64_t nanoseconds, count;
} MetalCommandRoleSurface;
static MetalCommandRoleSurface s_command_role_surfaces[8];
typedef struct {
    uint32_t reason_mask;
    uint64_t nanoseconds, count;
} MetalCommandReasonTiming;
static MetalCommandReasonTiming s_command_reason_timings[16];
static uint64_t s_command_reason_timing_overflows;
static uint64_t s_feedback_checks;
static uint64_t s_gpu_private_feedback_resolves;
static uint64_t s_feedback_fallbacks;
static uint64_t s_feedback_publications;
static uint64_t s_feedback_publication_ns;
static uint64_t s_quincunx_preparations;
static uint64_t s_quincunx_preparation_bytes;
static uint64_t s_quincunx_preparation_ns;
static uint64_t s_gpu_command_timestamp_failures;
static uint64_t s_last_flip_ns;
static int s_frame_timing_enabled;
static MetalPipelineDiagnostic s_pipeline_diagnostic;
static int s_water_benchmark_enabled;
static int s_water_benchmark_started;
static int s_water_benchmark_complete;
static int s_water_benchmark_failed;
static unsigned s_water_benchmark_block;
static unsigned s_water_benchmark_frames;
static int s_water_benchmark_sampling;
static uint64_t s_water_benchmark_samples[2][METAL_WATER_BENCHMARK_SAMPLES_PER_PRESET];
static size_t s_water_benchmark_sample_count[2];
static unsigned s_frame_supported_water_batches;
static uint64_t s_frame_water_gpu_ns;
static int s_frame_water_gpu_valid;
static int s_frame_water_gpu_invalid;
static int s_pending_supported_water;
static int s_water_capture_enabled;
static int s_water_capture_complete;
static int s_water_capture_trigger_active;
static unsigned s_water_capture_supported_frames;
static unsigned s_water_capture_images;
static BwMetalWaterPreset s_water_capture_preset;
static char s_water_capture_trigger[PATH_MAX];

static int flush_commands(void);
static int commit_commands_async(void);
static int retire_completed_submissions(void);
static int wait_for_all_submissions(void);
static id<MTLCommandBuffer> pending_command(void);
static void set_draw_state(id<MTLRenderCommandEncoder> encoder,
                           const Nv2aRenderState *state);
static int preserve_depth_surfaces(void);
static int publish_all_dirty_color_surfaces(void);
static int metal_fail(const char *format, ...);

static uint64_t pipeline_key_hash(uint64_t domain, const void *bytes, size_t length)
{
    const uint64_t offset = UINT64_C(14695981039346656037);
    const uint64_t prime = UINT64_C(1099511628211);
    const uint8_t *data = bytes;
    uint64_t hash = offset;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (domain >> shift) & 0xFFu;
        hash *= prime;
    }
    for (size_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= prime;
    }
    return hash;
}

static void pipeline_diagnostic_reset(int enabled)
{
    memset(&s_pipeline_diagnostic, 0, sizeof s_pipeline_diagnostic);
    s_pipeline_diagnostic.enabled = enabled != 0;
    s_pipeline_diagnostic.summary_dirty = enabled != 0;
}

static const char *pipeline_event_kind_name(MetalPipelineEventKind kind)
{
    switch (kind) {
    case METAL_PIPELINE_EVENT_LIBRARY_LOAD: return "library_load";
    case METAL_PIPELINE_EVENT_LIBRARY_COMPILE: return "library_compile";
    case METAL_PIPELINE_EVENT_FUNCTION_SPECIALIZATION:
        return "function_specialization";
    case METAL_PIPELINE_EVENT_PIPELINE_CREATE: return "pipeline_create";
    case METAL_PIPELINE_EVENT_CACHE_MISS: return "cache_miss";
    case METAL_PIPELINE_EVENT_CACHE_RESET: return "cache_reset";
    }
    return "invalid";
}

static const char *pipeline_role_name(MetalPipelineCommandRole role)
{
    switch (role) {
    case METAL_PIPELINE_ROLE_STARTUP: return "startup";
    case METAL_PIPELINE_ROLE_RENDER: return "render";
    }
    return "invalid";
}

static const char *pipeline_status_name(MetalPipelineStatus status)
{
    switch (status) {
    case METAL_PIPELINE_STATUS_OK: return "ok";
    case METAL_PIPELINE_STATUS_ARTIFACT_UNAVAILABLE:
        return "artifact_unavailable";
    case METAL_PIPELINE_STATUS_ARTIFACT_REJECTED:
        return "artifact_rejected";
    case METAL_PIPELINE_STATUS_ALLOCATION_FAILED: return "allocation_failed";
    case METAL_PIPELINE_STATUS_METAL_FAILED: return "metal_failed";
    }
    return "invalid";
}

static const char *pipeline_timing_status_name(MetalPipelineTimingStatus status)
{
    switch (status) {
    case METAL_PIPELINE_TIMING_NOT_APPLICABLE: return "not_applicable";
    case METAL_PIPELINE_TIMING_PENDING: return "pending";
    case METAL_PIPELINE_TIMING_OK: return "ok";
    case METAL_PIPELINE_TIMING_UNAVAILABLE: return "unavailable";
    case METAL_PIPELINE_TIMING_NON_MONOTONIC: return "non_monotonic";
    case METAL_PIPELINE_TIMING_NOT_COMPLETED: return "not_completed";
    }
    return "invalid";
}

static MetalPipelineTimingAggregate *pipeline_timing_aggregate(
    MetalPipelineEventKind kind)
{
    switch (kind) {
    case METAL_PIPELINE_EVENT_LIBRARY_LOAD:
        return &s_pipeline_diagnostic.library_load;
    case METAL_PIPELINE_EVENT_LIBRARY_COMPILE:
        return &s_pipeline_diagnostic.library;
    case METAL_PIPELINE_EVENT_FUNCTION_SPECIALIZATION:
        return &s_pipeline_diagnostic.specialization;
    case METAL_PIPELINE_EVENT_PIPELINE_CREATE:
        return &s_pipeline_diagnostic.pipeline;
    case METAL_PIPELINE_EVENT_CACHE_MISS:
    case METAL_PIPELINE_EVENT_CACHE_RESET:
        return NULL;
    }
    return NULL;
}

static void pipeline_diagnostic_note_status(MetalPipelineStatus status)
{
    if (status == METAL_PIPELINE_STATUS_ALLOCATION_FAILED)
        s_pipeline_diagnostic.allocation_failures++;
    else if (status == METAL_PIPELINE_STATUS_METAL_FAILED)
        s_pipeline_diagnostic.metal_failures++;
}

static void pipeline_diagnostic_store(MetalPipelineEvent event)
{
    if (!s_pipeline_diagnostic.enabled)
        return;
    s_pipeline_diagnostic.final_reported = 0;
    s_pipeline_diagnostic.summary_dirty = 1;
    pipeline_diagnostic_note_status(event.status);
    if (s_pipeline_diagnostic.event_count < METAL_PIPELINE_EVENT_CAPACITY) {
        s_pipeline_diagnostic.events[s_pipeline_diagnostic.event_count++] = event;
    } else {
        s_pipeline_diagnostic.events_dropped++;
    }
}

static MetalPipelineTimer pipeline_diagnostic_begin(void)
{
    MetalPipelineTimer timer = {0, METAL_PIPELINE_TIMING_NOT_APPLICABLE};
    if (!s_pipeline_diagnostic.enabled)
        return timer;
    timer.start_ns = xbox_HostNowNs();
    timer.status = timer.start_ns
                 ? METAL_PIPELINE_TIMING_PENDING
                 : METAL_PIPELINE_TIMING_UNAVAILABLE;
    return timer;
}

static void pipeline_diagnostic_record_instant(
    MetalPipelineEventKind kind, uint64_t key_hash, uint32_t frame,
    MetalPipelineCommandRole role, MetalPipelineStatus status)
{
    if (!s_pipeline_diagnostic.enabled)
        return;
    pipeline_diagnostic_store((MetalPipelineEvent){
        .key_hash = key_hash,
        .frame = frame,
        .kind = kind,
        .role = role,
        .status = status,
        .timing_status = METAL_PIPELINE_TIMING_NOT_APPLICABLE,
        .wall_timing_status = role == METAL_PIPELINE_ROLE_STARTUP
                            ? METAL_PIPELINE_TIMING_NOT_APPLICABLE
                            : METAL_PIPELINE_TIMING_PENDING,
    });
}

static void pipeline_diagnostic_finish(
    MetalPipelineEventKind kind, uint64_t key_hash, uint32_t frame,
    MetalPipelineCommandRole role, MetalPipelineStatus status,
    MetalPipelineTimer timer)
{
    if (!s_pipeline_diagnostic.enabled)
        return;
    MetalPipelineTimingAggregate *aggregate = pipeline_timing_aggregate(kind);
    MetalPipelineTimingStatus timing_status = timer.status;
    uint64_t duration = 0;
    if (aggregate)
        aggregate->attempts++;
    if (timing_status == METAL_PIPELINE_TIMING_PENDING) {
        uint64_t end = xbox_HostNowNs();
        if (!end) {
            timing_status = METAL_PIPELINE_TIMING_UNAVAILABLE;
        } else if (end <= timer.start_ns) {
            timing_status = METAL_PIPELINE_TIMING_NON_MONOTONIC;
        } else {
            timing_status = METAL_PIPELINE_TIMING_OK;
            duration = end - timer.start_ns;
        }
    }
    if (timing_status == METAL_PIPELINE_TIMING_OK && aggregate) {
        aggregate->timed++;
        if (UINT64_MAX - aggregate->total_ns >= duration)
            aggregate->total_ns += duration;
        if (duration > aggregate->max_ns)
            aggregate->max_ns = duration;
    } else if (timing_status == METAL_PIPELINE_TIMING_UNAVAILABLE) {
        s_pipeline_diagnostic.timing_unavailable++;
    } else if (timing_status == METAL_PIPELINE_TIMING_NON_MONOTONIC) {
        s_pipeline_diagnostic.timing_non_monotonic++;
    }
    pipeline_diagnostic_store((MetalPipelineEvent){
        .key_hash = key_hash,
        .duration_ns = duration,
        .frame = frame,
        .kind = kind,
        .role = role,
        .status = status,
        .timing_status = timing_status,
        .wall_timing_status = role == METAL_PIPELINE_ROLE_STARTUP
                            ? METAL_PIPELINE_TIMING_NOT_APPLICABLE
                            : METAL_PIPELINE_TIMING_PENDING,
    });
}

static void pipeline_diagnostic_cache_hit(void)
{
    if (s_pipeline_diagnostic.enabled) {
        s_pipeline_diagnostic.cache_hits++;
        s_pipeline_diagnostic.summary_dirty = 1;
    }
}

static void pipeline_diagnostic_cache_miss(uint64_t key_hash)
{
    if (!s_pipeline_diagnostic.enabled)
        return;
    s_pipeline_diagnostic.cache_misses++;
    s_pipeline_diagnostic.summary_dirty = 1;
    pipeline_diagnostic_record_instant(
        METAL_PIPELINE_EVENT_CACHE_MISS, key_hash, s_frame,
        METAL_PIPELINE_ROLE_RENDER, METAL_PIPELINE_STATUS_OK);
}

static void pipeline_diagnostic_cache_reset(uint64_t key_hash)
{
    if (!s_pipeline_diagnostic.enabled)
        return;
    s_pipeline_diagnostic.whole_cache_resets++;
    s_pipeline_diagnostic.summary_dirty = 1;
    pipeline_diagnostic_record_instant(
        METAL_PIPELINE_EVENT_CACHE_RESET, key_hash, s_frame,
        METAL_PIPELINE_ROLE_RENDER, METAL_PIPELINE_STATUS_OK);
}

static void pipeline_diagnostic_cache_count(NSUInteger count)
{
    if (s_pipeline_diagnostic.enabled &&
        count > s_pipeline_diagnostic.cache_entry_high_water) {
        s_pipeline_diagnostic.cache_entry_high_water = (unsigned)count;
        s_pipeline_diagnostic.summary_dirty = 1;
    }
}

static void pipeline_diagnostic_emit_ready_events(void);
static void pipeline_diagnostic_emit_summary(int final);

static void pipeline_diagnostic_complete_frame(
    uint32_t frame, uint64_t duration_ns, MetalPipelineTimingStatus status)
{
    if (!s_pipeline_diagnostic.enabled)
        return;
    if (status == METAL_PIPELINE_TIMING_OK) {
        s_pipeline_diagnostic.wall_frame_intervals++;
        if (duration_ns > s_pipeline_diagnostic.wall_frame_max_ns) {
            s_pipeline_diagnostic.wall_frame_max_ns = duration_ns;
            s_pipeline_diagnostic.wall_frame_max_frame = frame;
        }
    } else if (status == METAL_PIPELINE_TIMING_UNAVAILABLE) {
        s_pipeline_diagnostic.wall_timing_unavailable++;
    } else if (status == METAL_PIPELINE_TIMING_NON_MONOTONIC) {
        s_pipeline_diagnostic.wall_timing_non_monotonic++;
    }
    s_pipeline_diagnostic.summary_dirty = 1;
    for (size_t i = 0; i < s_pipeline_diagnostic.event_count; i++) {
        MetalPipelineEvent *event = &s_pipeline_diagnostic.events[i];
        if (event->frame != frame ||
            event->wall_timing_status != METAL_PIPELINE_TIMING_PENDING)
            continue;
        event->wall_timing_status = status;
        event->wall_frame_ns =
            status == METAL_PIPELINE_TIMING_OK ? duration_ns : 0;
    }
    size_t reported = s_pipeline_diagnostic.events_reported;
    pipeline_diagnostic_emit_ready_events();
    if (s_pipeline_diagnostic.events_reported != reported || !(frame % 100u))
        pipeline_diagnostic_emit_summary(0);
}

static int pipeline_diagnostic_format_event(
    const MetalPipelineEvent *event, size_t sequence, char *buffer, size_t size)
{
    char duration[32], wall_duration[32];
    if (!buffer || !size || !event)
        return 0;
    if (event->timing_status == METAL_PIPELINE_TIMING_OK)
        snprintf(duration, sizeof duration, "%llu",
                 (unsigned long long)event->duration_ns);
    else
        memcpy(duration, "null", sizeof "null");
    if (event->wall_timing_status == METAL_PIPELINE_TIMING_OK)
        snprintf(wall_duration, sizeof wall_duration, "%llu",
                 (unsigned long long)event->wall_frame_ns);
    else
        memcpy(wall_duration, "null", sizeof "null");
    int wall_max =
        event->wall_timing_status == METAL_PIPELINE_TIMING_OK &&
        event->frame == s_pipeline_diagnostic.wall_frame_max_frame &&
        event->wall_frame_ns == s_pipeline_diagnostic.wall_frame_max_ns;
    int written = snprintf(
        buffer, size,
        "{\"schema\":\"" METAL_PIPELINE_SCHEMA "\",\"record\":\"event\","
        "\"sequence\":%zu,\"kind\":\"%s\",\"key_hash\":\"%016llx\","
        "\"frame\":%u,\"command_role\":\"%s\",\"status\":\"%s\","
        "\"timing_status\":\"%s\",\"duration_ns\":%s,"
        "\"wall_timing_status\":\"%s\",\"wall_frame_ns\":%s,"
        "\"wall_frame_max_so_far\":%s}",
        sequence, pipeline_event_kind_name(event->kind),
        (unsigned long long)event->key_hash, event->frame,
        pipeline_role_name(event->role), pipeline_status_name(event->status),
        pipeline_timing_status_name(event->timing_status), duration,
        pipeline_timing_status_name(event->wall_timing_status), wall_duration,
        wall_max ? "true" : "false");
    return written >= 0 && (size_t)written < size;
}

static int pipeline_diagnostic_format_summary(char *buffer, size_t size)
{
    if (!buffer || !size)
        return 0;
    char max_frame[32], max_duration[32];
    if (s_pipeline_diagnostic.wall_frame_intervals) {
        snprintf(max_frame, sizeof max_frame, "%u",
                 s_pipeline_diagnostic.wall_frame_max_frame);
        snprintf(max_duration, sizeof max_duration, "%llu",
                 (unsigned long long)s_pipeline_diagnostic.wall_frame_max_ns);
    } else {
        memcpy(max_frame, "null", sizeof "null");
        memcpy(max_duration, "null", sizeof "null");
    }
    int written = snprintf(
        buffer, size,
        "{\"schema\":\"" METAL_PIPELINE_SCHEMA "\",\"record\":\"summary\","
        "\"sequence\":%u,\"enabled\":%s,\"event_capacity\":%u,"
        "\"events_recorded\":%zu,\"events_reported\":%zu,"
        "\"events_dropped\":%llu,\"cache_hits\":%llu,\"cache_misses\":%llu,"
        "\"cache_entry_high_water\":%u,\"cache_entry_limit\":%u,"
        "\"whole_cache_resets\":%llu,"
        "\"library_artifact_available\":%s,\"library_artifact_loaded\":%s,"
        "\"library_load_attempts\":%llu,\"library_load_timed\":%llu,"
        "\"library_load_total_ns\":%llu,\"library_load_max_ns\":%llu,"
        "\"library_fallbacks\":%llu,\"library_unavailable\":%llu,"
        "\"library_rejections\":%llu,"
        "\"library_attempts\":%llu,\"library_timed\":%llu,"
        "\"library_total_ns\":%llu,\"library_max_ns\":%llu,"
        "\"specialization_attempts\":%llu,\"specialization_timed\":%llu,"
        "\"specialization_total_ns\":%llu,\"specialization_max_ns\":%llu,"
        "\"pipeline_attempts\":%llu,\"pipeline_timed\":%llu,"
        "\"pipeline_total_ns\":%llu,\"pipeline_max_ns\":%llu,"
        "\"wall_frame_intervals\":%llu,\"wall_frame_max_frame\":%s,"
        "\"wall_frame_max_ns\":%s,\"timing_unavailable\":%llu,"
        "\"timing_non_monotonic\":%llu,\"wall_timing_unavailable\":%llu,"
        "\"wall_timing_non_monotonic\":%llu,\"allocation_failures\":%llu,"
        "\"metal_failures\":%llu}",
        s_pipeline_diagnostic.summaries_reported,
        s_pipeline_diagnostic.enabled ? "true" : "false",
        METAL_PIPELINE_EVENT_CAPACITY, s_pipeline_diagnostic.event_count,
        s_pipeline_diagnostic.events_reported,
        (unsigned long long)s_pipeline_diagnostic.events_dropped,
        (unsigned long long)s_pipeline_diagnostic.cache_hits,
        (unsigned long long)s_pipeline_diagnostic.cache_misses,
        s_pipeline_diagnostic.cache_entry_high_water, METAL_MAX_PIPELINES,
        (unsigned long long)s_pipeline_diagnostic.whole_cache_resets,
        BW_METAL_OFFLINE_SHADER_LIBRARIES ? "true" : "false",
        s_pipeline_diagnostic.library_artifact_loaded ? "true" : "false",
        (unsigned long long)s_pipeline_diagnostic.library_load.attempts,
        (unsigned long long)s_pipeline_diagnostic.library_load.timed,
        (unsigned long long)s_pipeline_diagnostic.library_load.total_ns,
        (unsigned long long)s_pipeline_diagnostic.library_load.max_ns,
        (unsigned long long)s_pipeline_diagnostic.library_fallbacks,
        (unsigned long long)s_pipeline_diagnostic.library_unavailable,
        (unsigned long long)s_pipeline_diagnostic.library_rejections,
        (unsigned long long)s_pipeline_diagnostic.library.attempts,
        (unsigned long long)s_pipeline_diagnostic.library.timed,
        (unsigned long long)s_pipeline_diagnostic.library.total_ns,
        (unsigned long long)s_pipeline_diagnostic.library.max_ns,
        (unsigned long long)s_pipeline_diagnostic.specialization.attempts,
        (unsigned long long)s_pipeline_diagnostic.specialization.timed,
        (unsigned long long)s_pipeline_diagnostic.specialization.total_ns,
        (unsigned long long)s_pipeline_diagnostic.specialization.max_ns,
        (unsigned long long)s_pipeline_diagnostic.pipeline.attempts,
        (unsigned long long)s_pipeline_diagnostic.pipeline.timed,
        (unsigned long long)s_pipeline_diagnostic.pipeline.total_ns,
        (unsigned long long)s_pipeline_diagnostic.pipeline.max_ns,
        (unsigned long long)s_pipeline_diagnostic.wall_frame_intervals,
        max_frame, max_duration,
        (unsigned long long)s_pipeline_diagnostic.timing_unavailable,
        (unsigned long long)s_pipeline_diagnostic.timing_non_monotonic,
        (unsigned long long)s_pipeline_diagnostic.wall_timing_unavailable,
        (unsigned long long)s_pipeline_diagnostic.wall_timing_non_monotonic,
        (unsigned long long)s_pipeline_diagnostic.allocation_failures,
        (unsigned long long)s_pipeline_diagnostic.metal_failures);
    return written >= 0 && (size_t)written < size;
}

static void pipeline_diagnostic_emit_ready_events(void)
{
    if (!s_pipeline_diagnostic.enabled)
        return;
    char line[METAL_PIPELINE_REPORT_LINE_MAX];
    while (s_pipeline_diagnostic.events_reported <
           s_pipeline_diagnostic.event_count) {
        size_t i = s_pipeline_diagnostic.events_reported;
        const MetalPipelineEvent *event = &s_pipeline_diagnostic.events[i];
        if (event->wall_timing_status == METAL_PIPELINE_TIMING_PENDING)
            break;
        if (pipeline_diagnostic_format_event(event, i, line, sizeof line)) {
            fprintf(stderr, "%s\n", line);
        } else {
            fprintf(stderr,
                    "{\"schema\":\"" METAL_PIPELINE_SCHEMA "\","
                    "\"record\":\"error\",\"sequence\":%zu,"
                    "\"status\":\"format_failed\"}\n", i);
        }
        s_pipeline_diagnostic.events_reported++;
    }
}

static void pipeline_diagnostic_emit_summary(int final)
{
    unsigned limit = final ? METAL_PIPELINE_SUMMARY_CAPACITY
                           : METAL_PIPELINE_SUMMARY_CAPACITY - 1u;
    if (!s_pipeline_diagnostic.enabled ||
        !s_pipeline_diagnostic.summary_dirty ||
        s_pipeline_diagnostic.summaries_reported >= limit)
        return;
    char line[METAL_PIPELINE_REPORT_LINE_MAX];
    if (pipeline_diagnostic_format_summary(line, sizeof line)) {
        fprintf(stderr, "%s\n", line);
    } else {
        fprintf(stderr,
                "{\"schema\":\"" METAL_PIPELINE_SCHEMA "\","
                "\"record\":\"error\",\"status\":\"summary_format_failed\"}\n");
    }
    s_pipeline_diagnostic.summaries_reported++;
    s_pipeline_diagnostic.summary_dirty = 0;
}

static void pipeline_diagnostic_report(void)
{
    if (!s_pipeline_diagnostic.enabled || s_pipeline_diagnostic.final_reported)
        return;
    for (size_t i = s_pipeline_diagnostic.events_reported;
         i < s_pipeline_diagnostic.event_count; i++)
        if (s_pipeline_diagnostic.events[i].wall_timing_status ==
            METAL_PIPELINE_TIMING_PENDING)
            s_pipeline_diagnostic.events[i].wall_timing_status =
                METAL_PIPELINE_TIMING_NOT_COMPLETED;
    pipeline_diagnostic_emit_ready_events();
    s_pipeline_diagnostic.summary_dirty = 1;
    pipeline_diagnostic_emit_summary(1);
    s_pipeline_diagnostic.final_reported = 1;
}

static MTLRenderPipelineDescriptor *new_pipeline_descriptor(void)
{
#if defined(BW_METAL_TESTING)
    if (s_test_fail_next_pipeline_allocation) {
        s_test_fail_next_pipeline_allocation = 0;
        return nil;
    }
#endif
    return [MTLRenderPipelineDescriptor new];
}

static id<MTLRenderPipelineState> create_render_pipeline_state(
    MTLRenderPipelineDescriptor *descriptor, uint64_t key_hash, uint32_t frame,
    MetalPipelineCommandRole role, NSError **error)
{
    if (!descriptor) {
        pipeline_diagnostic_finish(
            METAL_PIPELINE_EVENT_PIPELINE_CREATE, key_hash, frame, role,
            METAL_PIPELINE_STATUS_ALLOCATION_FAILED,
            (MetalPipelineTimer){0, METAL_PIPELINE_TIMING_NOT_APPLICABLE});
        return nil;
    }
    MetalPipelineTimer timer = pipeline_diagnostic_begin();
    id<MTLRenderPipelineState> state =
        [s_device newRenderPipelineStateWithDescriptor:descriptor error:error];
    pipeline_diagnostic_finish(
        METAL_PIPELINE_EVENT_PIPELINE_CREATE, key_hash, frame, role,
        state ? METAL_PIPELINE_STATUS_OK : METAL_PIPELINE_STATUS_METAL_FAILED,
        timer);
    return state;
}

static int valid_anisotropy(unsigned value)
{
    return value == 1u || value == 2u || value == 4u || value == 8u || value == 16u;
}

static unsigned anisotropy_index(unsigned value)
{
    switch (value) {
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    case 16: return 4;
    default: return 0;
    }
}

static unsigned sampler_anisotropy(int min_linear, unsigned mip_mode)
{
    return min_linear && mip_mode ? s_active_anisotropy : 1u;
}

static void apply_pending_render_settings(void)
{
    s_active_anisotropy =
        atomic_load_explicit(&s_requested_anisotropy, memory_order_acquire);
    if ((!s_water_benchmark_enabled || s_water_benchmark_complete ||
         s_water_benchmark_failed) &&
        (!s_water_capture_enabled || s_water_capture_complete))
        s_active_water_preset =
            atomic_load_explicit(&s_requested_water_preset, memory_order_acquire);
}

int bw_metal_backend_set_anisotropy(unsigned anisotropy)
{
    if (!valid_anisotropy(anisotropy))
        return 0;
    atomic_store_explicit(&s_requested_anisotropy, anisotropy, memory_order_release);
    return 1;
}

unsigned bw_metal_backend_anisotropy(void)
{
    unsigned value =
        atomic_load_explicit(&s_requested_anisotropy, memory_order_acquire);
    return valid_anisotropy(value) ? value : 1u;
}

int bw_metal_backend_set_water_preset(BwMetalWaterPreset preset)
{
    if (preset != BW_METAL_WATER_RETAIL &&
        preset != BW_METAL_WATER_REALISTIC)
        return 0;
    if (s_water_benchmark_enabled && !s_water_benchmark_complete &&
        !s_water_benchmark_failed)
        return 0;
    if (s_water_capture_enabled && !s_water_capture_complete)
        return 0;
    if (preset == BW_METAL_WATER_REALISTIC &&
        !bw_edition_at_least(BW_ED_REMASTERED))
        return 0;
    atomic_store_explicit(&s_requested_water_preset, (unsigned)preset,
                          memory_order_release);
    return 1;
}

BwMetalWaterPreset bw_metal_backend_water_preset(void)
{
    unsigned value =
        atomic_load_explicit(&s_requested_water_preset, memory_order_acquire);
    return value == BW_METAL_WATER_REALISTIC
         ? BW_METAL_WATER_REALISTIC : BW_METAL_WATER_RETAIL;
}

static void end_draw_encoder(void)
{
    if (s_draw_encoder)
        [s_draw_encoder endEncoding];
    s_draw_encoder = nil;
    s_draw_color = nil;
    s_draw_depth = nil;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *)a, bv = *(const uint64_t *)b;
    return av < bv ? -1 : av > bv;
}

static void report_frame_times(const char *label, const uint64_t *samples, size_t count)
{
    if (!count)
        return;
    uint64_t *sorted = malloc(count * sizeof *sorted);
    if (!sorted) {
        fprintf(stderr, "[GPU-METAL] frame timing unavailable: allocation failed\n");
        return;
    }
    uint64_t sum = 0, maximum = 0;
    for (size_t i = 0; i < count; i++) {
        sorted[i] = samples[i];
        sum += samples[i];
        if (samples[i] > maximum)
            maximum = samples[i];
    }
    qsort(sorted, count, sizeof *sorted, compare_u64);
    uint64_t median = sorted[count / 2u];
    uint64_t p95 = sorted[((count * 95u + 99u) / 100u) - 1u];
    fprintf(stderr,
            "[GPU-METAL] frame timing %s: %zu intervals, %.2f fps average, "
            "%.2f ms median, %.2f ms p95, %.2f ms max\n",
            label, count, sum ? (double)count * 1e9 / (double)sum : 0.0,
            (double)median / 1e6, (double)p95 / 1e6, (double)maximum / 1e6);
    free(sorted);
}

static void report_duration_times(const char *kind, const char *label,
                                  const uint64_t *samples, size_t count)
{
    if (!count)
        return;
    uint64_t *sorted = malloc(count * sizeof *sorted);
    if (!sorted) {
        fprintf(stderr, "[GPU-METAL] %s timing unavailable: allocation failed\n", kind);
        return;
    }
    uint64_t sum = 0, maximum = 0;
    for (size_t i = 0; i < count; i++) {
        sorted[i] = samples[i];
        sum += samples[i];
        if (samples[i] > maximum)
            maximum = samples[i];
    }
    qsort(sorted, count, sizeof *sorted, compare_u64);
    uint64_t median = sorted[count / 2u];
    uint64_t p95 = sorted[((count * 95u + 99u) / 100u) - 1u];
    fprintf(stderr,
            "[GPU-METAL] %s timing %s: %zu frames, %.2f ms mean, "
            "%.2f ms median, %.2f ms p95, %.2f ms max\n",
            kind, label, count, (double)sum / (double)count / 1e6,
            (double)median / 1e6, (double)p95 / 1e6, (double)maximum / 1e6);
    free(sorted);
}

static void command_reason_text(uint32_t reason_mask, char *text, size_t size)
{
    static const struct {
        uint32_t flag;
        const char *name;
    } reasons[] = {
        {METAL_COMMAND_REASON_RENDER, "render"},
        {METAL_COMMAND_REASON_TARGET_SWITCH, "target-switch"},
        {METAL_COMMAND_REASON_SURFACE_UPLOAD, "surface-upload"},
        {METAL_COMMAND_REASON_SURFACE_READBACK, "surface-readback"},
        {METAL_COMMAND_REASON_FEEDBACK, "feedback"},
        {METAL_COMMAND_REASON_PRESENT, "present"},
        {METAL_COMMAND_REASON_DEPTH_COPY, "depth-copy"},
    };
    size_t used = 0;
    if (!size)
        return;
    text[0] = '\0';
    for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
        if (!(reason_mask & reasons[i].flag) || used >= size - 1u)
            continue;
        int written = snprintf(text + used, size - used, "%s%s",
                               used ? "+" : "", reasons[i].name);
        if (written < 0)
            break;
        if ((size_t)written >= size - used) {
            used = size - 1u;
            break;
        }
        used += (size_t)written;
    }
    if (!used)
        snprintf(text, size, "unspecified");
}

typedef struct {
    double mean_ms, median_ms, p95_ms;
} MetalWaterBenchmarkStats;

static MetalWaterBenchmarkStats water_benchmark_stats(const uint64_t *samples,
                                                       size_t count)
{
    uint64_t sorted[METAL_WATER_BENCHMARK_SAMPLES_PER_PRESET];
    long double sum = 0.0;
    MetalWaterBenchmarkStats stats = {0};
    if (!count || count > METAL_WATER_BENCHMARK_SAMPLES_PER_PRESET)
        return stats;
    for (size_t i = 0; i < count; i++) {
        sorted[i] = samples[i];
        sum += samples[i];
    }
    qsort(sorted, count, sizeof *sorted, compare_u64);
    stats.mean_ms = (double)(sum / count / 1.0e6L);
    stats.median_ms = (double)sorted[count / 2u] / 1.0e6;
    stats.p95_ms =
        (double)sorted[((count * 95u + 99u) / 100u) - 1u] / 1.0e6;
    return stats;
}

static void water_benchmark_set_preset(BwMetalWaterPreset preset)
{
    s_active_water_preset = preset;
    atomic_store_explicit(&s_requested_water_preset, (unsigned)preset,
                          memory_order_release);
}

static void water_benchmark_fail(const char *reason)
{
    if (!s_water_benchmark_failed)
        fprintf(stderr, "[GPU-METAL] water benchmark unavailable: %s\n", reason);
    s_water_benchmark_failed = 1;
    water_benchmark_set_preset(BW_METAL_WATER_RETAIL);
}

static void water_benchmark_report(void)
{
    MetalWaterBenchmarkStats retail =
        water_benchmark_stats(s_water_benchmark_samples[BW_METAL_WATER_RETAIL],
                              s_water_benchmark_sample_count[BW_METAL_WATER_RETAIL]);
    MetalWaterBenchmarkStats realistic =
        water_benchmark_stats(s_water_benchmark_samples[BW_METAL_WATER_REALISTIC],
                              s_water_benchmark_sample_count[BW_METAL_WATER_REALISTIC]);
    double mean_delta = retail.mean_ms > 0.0
                      ? (realistic.mean_ms / retail.mean_ms - 1.0) * 100.0 : 0.0;
    double median_delta = retail.median_ms > 0.0
                        ? (realistic.median_ms / retail.median_ms - 1.0) * 100.0 : 0.0;
    double p95_delta = retail.p95_ms > 0.0
                     ? (realistic.p95_ms / retail.p95_ms - 1.0) * 100.0 : 0.0;
    fprintf(stderr,
            "[GPU-METAL] water benchmark v1 "
            "retail_samples=%zu retail_mean_ms=%.6f retail_median_ms=%.6f "
            "retail_p95_ms=%.6f realistic_samples=%zu realistic_mean_ms=%.6f "
            "realistic_median_ms=%.6f realistic_p95_ms=%.6f "
            "mean_delta_percent=%+.6f median_delta_percent=%+.6f "
            "p95_delta_percent=%+.6f\n",
            s_water_benchmark_sample_count[BW_METAL_WATER_RETAIL],
            retail.mean_ms, retail.median_ms, retail.p95_ms,
            s_water_benchmark_sample_count[BW_METAL_WATER_REALISTIC],
            realistic.mean_ms, realistic.median_ms, realistic.p95_ms,
            mean_delta, median_delta, p95_delta);
}

static BwMetalWaterPreset water_benchmark_block_preset(unsigned block)
{
    static const BwMetalWaterPreset presets[METAL_WATER_BENCHMARK_BLOCKS] = {
        BW_METAL_WATER_RETAIL, BW_METAL_WATER_REALISTIC,
        BW_METAL_WATER_REALISTIC, BW_METAL_WATER_RETAIL,
    };
    return presets[block];
}

static void water_benchmark_frame(void)
{
    if (!s_water_benchmark_enabled || s_water_benchmark_complete ||
        s_water_benchmark_failed)
        goto reset;
    if (!s_frame_supported_water_batches)
        goto reset;
    if (!s_frame_water_gpu_valid || s_frame_water_gpu_invalid ||
        !s_frame_water_gpu_ns) {
        water_benchmark_fail("Metal GPU timestamps were unavailable");
        goto reset;
    }
    if (!s_water_benchmark_started) {
        s_water_benchmark_started = 1;
        s_water_benchmark_block = 0;
        s_water_benchmark_frames = 0;
        s_water_benchmark_sampling = 0;
        water_benchmark_set_preset(water_benchmark_block_preset(0));
        fprintf(stderr,
                "[GPU-METAL] water benchmark started: same-scene ABBA, "
                "%u warm-up and %u measured water frames per block\n",
                METAL_WATER_BENCHMARK_WARMUP_FRAMES,
                METAL_WATER_BENCHMARK_SAMPLE_FRAMES);
    }
    BwMetalWaterPreset expected =
        water_benchmark_block_preset(s_water_benchmark_block);
    if (s_active_water_preset != (unsigned)expected) {
        water_benchmark_fail("the active preset changed during measurement");
        goto reset;
    }
    if (s_water_benchmark_sampling) {
        size_t index = s_water_benchmark_sample_count[expected];
        if (index >= METAL_WATER_BENCHMARK_SAMPLES_PER_PRESET) {
            water_benchmark_fail("the sample table overflowed");
            goto reset;
        }
        s_water_benchmark_samples[expected][index] = s_frame_water_gpu_ns;
        s_water_benchmark_sample_count[expected] = index + 1u;
    }
    s_water_benchmark_frames++;
    unsigned target = s_water_benchmark_sampling
                    ? METAL_WATER_BENCHMARK_SAMPLE_FRAMES
                    : METAL_WATER_BENCHMARK_WARMUP_FRAMES;
    if (s_water_benchmark_frames < target)
        goto reset;
    s_water_benchmark_frames = 0;
    if (!s_water_benchmark_sampling) {
        s_water_benchmark_sampling = 1;
        goto reset;
    }
    s_water_benchmark_sampling = 0;
    s_water_benchmark_block++;
    if (s_water_benchmark_block == METAL_WATER_BENCHMARK_BLOCKS) {
        if (s_water_benchmark_sample_count[BW_METAL_WATER_RETAIL] !=
                METAL_WATER_BENCHMARK_SAMPLES_PER_PRESET ||
            s_water_benchmark_sample_count[BW_METAL_WATER_REALISTIC] !=
                METAL_WATER_BENCHMARK_SAMPLES_PER_PRESET) {
            water_benchmark_fail("the ABBA sample counts are inconsistent");
            goto reset;
        }
        water_benchmark_report();
        s_water_benchmark_complete = 1;
        water_benchmark_set_preset(BW_METAL_WATER_RETAIL);
        goto reset;
    }
    water_benchmark_set_preset(
        water_benchmark_block_preset(s_water_benchmark_block));

reset:
    s_frame_supported_water_batches = 0;
    s_frame_water_gpu_ns = 0;
    s_frame_water_gpu_valid = 0;
    s_frame_water_gpu_invalid = 0;
}

static unsigned water_capture_offset(unsigned image)
{
#if defined(BW_METAL_TESTING)
    return image;
#else
    return image * 60u;
#endif
}

static int water_capture_arm_frame(void)
{
    if (!s_water_capture_enabled || s_water_capture_complete ||
        s_water_capture_trigger_active ||
        s_water_capture_images >= METAL_WATER_CAPTURE_IMAGES ||
        s_water_capture_supported_frames !=
            water_capture_offset(s_water_capture_images))
        return 1;
    int fd = open(s_water_capture_trigger,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return metal_fail("could not arm the private water capture: %s",
                          strerror(errno));
    if (close(fd) != 0) {
        int error = errno;
        (void)unlink(s_water_capture_trigger);
        return metal_fail("could not close the private water capture trigger: %s",
                          strerror(error));
    }
    s_water_capture_trigger_active = 1;
    return 1;
}

static int water_capture_frame(void)
{
    if (!s_water_capture_enabled || s_water_capture_complete)
        return 1;
    if (s_active_water_preset != s_water_capture_preset)
        return metal_fail("the active preset changed during private water capture");
    if (s_water_capture_trigger_active) {
        if (unlink(s_water_capture_trigger) != 0)
            return metal_fail("could not clear the private water capture trigger: %s",
                              strerror(errno));
        s_water_capture_trigger_active = 0;
        s_water_capture_images++;
        if (s_water_capture_images == METAL_WATER_CAPTURE_IMAGES) {
            s_water_capture_complete = 1;
            fprintf(stderr,
                    "[GPU-METAL] water capture v1 preset=%s images=%u "
                    "supported_offsets=%u,%u,%u,%u width=%u height=%u\n",
                    s_water_capture_preset == BW_METAL_WATER_REALISTIC
                        ? "realistic" : "retail",
                    METAL_WATER_CAPTURE_IMAGES,
                    water_capture_offset(0), water_capture_offset(1),
                    water_capture_offset(2), water_capture_offset(3),
                    s_present_framebuffer.width, s_present_framebuffer.height);
        }
    }
    if (s_frame_supported_water_batches)
        s_water_capture_supported_frames++;
    return 1;
}

static int metal_fail(const char *format, ...)
{
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&s_error_ready, &expected, -1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        va_list args;
        va_start(args, format);
        vsnprintf(s_error, sizeof s_error, format, args);
        va_end(args);
        fprintf(stderr, "[GPU-METAL] %s\n", s_error);
        atomic_store_explicit(&s_error_ready, 1, memory_order_release);
    }
    return 0;
}

static const char *metal_error_text(NSError *error)
{
    return error ? error.localizedDescription.UTF8String : "unknown Metal error";
}

const char *bw_metal_backend_error(void)
{
    return atomic_load_explicit(&s_error_ready, memory_order_acquire) == 1 ? s_error : NULL;
}

static int presenter_fail(const char *message)
{
    fprintf(stderr, "[GPU-METAL] direct presenter unavailable: %s\n", message);
    return 0;
}

int bw_metal_backend_attach_presenter(void *metal_layer, uint32_t drawable_width,
                                      uint32_t drawable_height, int vsync)
{
    @autoreleasepool {
        if (![NSThread isMainThread])
            return presenter_fail("attachment did not run on the process main thread");
        if (!metal_layer || !drawable_width || !drawable_height)
            return presenter_fail("invalid CAMetalLayer or drawable size");
        if (s_registered || s_queue)
            return presenter_fail("backend already started");
        CAMetalLayer *layer = (__bridge CAMetalLayer *)metal_layer;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
            return presenter_fail("no Metal device");
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.presentsWithTransaction = NO;
        layer.maximumDrawableCount = 3;
        layer.displaySyncEnabled = vsync != 0;
        layer.allowsNextDrawableTimeout = YES;
        layer.drawableSize = CGSizeMake(drawable_width, drawable_height);
        s_device = device;
        s_present_layer = layer;
        atomic_store_explicit(&s_present_width, drawable_width, memory_order_release);
        atomic_store_explicit(&s_present_height, drawable_height, memory_order_release);
        fprintf(stderr, "[GPU-METAL] direct CAMetalLayer presenter attached (%ux%u, vsync %s)\n",
                drawable_width, drawable_height, vsync ? "on" : "off");
        return 1;
    }
}

int bw_metal_backend_resize_presenter(uint32_t drawable_width,
                                      uint32_t drawable_height)
{
    @autoreleasepool {
        if (![NSThread isMainThread])
            return presenter_fail("resize did not run on the process main thread");
        if (!s_present_layer)
            return presenter_fail("resize occurred before attachment");
        if (!drawable_width || !drawable_height)
            return 1;
        s_present_layer.drawableSize = CGSizeMake(drawable_width, drawable_height);
        atomic_store_explicit(&s_present_width, drawable_width, memory_order_release);
        atomic_store_explicit(&s_present_height, drawable_height, memory_order_release);
        return 1;
    }
}

static uint8_t *guest_pointer(uint32_t address)
{
    return (uint8_t *)(s_guest + (uintptr_t)address);
}

static int parse_scale(const char *value, float *scale)
{
    char *end;
    float parsed;
    if (!value || !*value)
        return 0;
    errno = 0;
    parsed = strtof(value, &end);
    if (errno || *end || !isfinite(parsed) || parsed < 0.5f || parsed > 10.0f)
        return 0;
    *scale = parsed;
    return 1;
}

static float automatic_render_scale(void)
{
    uint32_t width = atomic_load_explicit(&s_present_width, memory_order_acquire);
    uint32_t height = atomic_load_explicit(&s_present_height, memory_order_acquire);
    if (!width || !height) {
        const char *window_scale = getenv("RECOMP_WINDOW_SCALE");
        unsigned scale = window_scale ? (unsigned)atoi(window_scale) : 2u;
        if (scale < 1u || scale > 8u)
            scale = 2u;
        width = 640u * scale;
        height = 480u * scale;
    }
    float picture_height = fminf((float)height, (float)width * 0.75f);
    float scale = picture_height / 480.0f * s_render_ssaa;
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 10.0f) scale = 10.0f;
    return scale;
}

static int refresh_render_scale(int discard_surfaces)
{
    float next = s_render_scale_auto ? automatic_render_scale() : s_render_fixed_scale;
    if (fabsf(next - s_render_scale) <= 0.001f)
        return 1;
    if (discard_surfaces && (s_surface || s_dirty))
        return metal_fail("render scale changed while a colour surface was active");
    if (discard_surfaces && !publish_all_dirty_color_surfaces())
        return 0;
    if (discard_surfaces && !preserve_depth_surfaces())
        return 0;
    s_render_scale = next;
    s_render_scale_epoch++;
    if (discard_surfaces) {
        [s_color_surfaces removeAllObjects];
        [s_depth_surfaces removeAllObjects];
        s_present_resolve = nil;
    }
    fprintf(stderr, "[GPU-METAL] internal render scale %.3f "
                    "(640x480 target %.0fx%.0f, epoch %u)\n",
            s_render_scale, 640.0f * s_render_scale, 480.0f * s_render_scale,
            s_render_scale_epoch);
    return 1;
}

static int configure_render_scale(void)
{
    const char *override = getenv("BW_METAL_RENDER_SCALE");
    const char *vfx = getenv("BW_VFX");
    float fixed;
    int native = 0;

    s_render_scale = 1.0f;
    s_render_fixed_scale = 1.0f;
    s_render_ssaa = 1.0f;
    s_render_scale_epoch = 1;
    s_render_scale_auto = 0;
    s_render_guest_aa = 0;
    if (override) {
        if (!strcmp(override, "auto"))
            s_render_scale_auto = 1;
        else if (parse_scale(override, &fixed))
            s_render_fixed_scale = fixed;
        else
            return metal_fail("BW_METAL_RENDER_SCALE must be auto or a number from 0.5 to 10");
    } else if (bw_edition_at_least(BW_ED_REMASTERED) && !(vfx && vfx[0] == '0')) {
        const char *native_env = getenv("BW_VFX_NATIVE_RES");
        native = bw_fix_enabled(BW_FIX_VFX_NATIVE_RES);
        if (native_env && *native_env)
            native = native_env[0] != '0' &&
                     !(native_env[0] == 'o' && native_env[1] == 'f') &&
                     !(native_env[0] == 'O' && native_env[1] == 'F');
        const char *fixed_env = getenv("BW_VFX_SCALE");
        if (fixed_env && parse_scale(fixed_env, &fixed)) {
            s_render_fixed_scale = fixed;
            native = 1;
        } else if (native) {
            const char *ssaa = getenv("BW_VFX_SSAA");
            if (ssaa && parse_scale(ssaa, &fixed) && fixed >= 1.0f)
                s_render_ssaa = fixed > 4.0f ? 4.0f : fixed;
            s_render_scale_auto = 1;
        }
        const char *guest_aa = getenv("BW_VFX_GUESTAA");
        s_render_guest_aa = guest_aa && !strcmp(guest_aa, "1");
    }
    if (!refresh_render_scale(0))
        return 0;
    if (override)
        fprintf(stderr, "[GPU-METAL] BW_METAL_RENDER_SCALE override enabled%s\n",
                s_render_scale_auto ? " (auto)" : "");
    return 1;
}

static int surface_physical_size(const Nv2aSurface *surface,
                                 uint32_t *width, uint32_t *height)
{
    if (fabsf(s_render_scale - 1.0f) <= 0.001f) {
        *width = surface->width;
        *height = surface->height;
        return 1;
    }
    float sx = s_render_scale;
    float sy = s_render_scale;
    if (!s_render_guest_aa) {
        sx /= surface->aa_sx;
        sy /= surface->aa_sy;
    }
    double scaled_width = floor((double)surface->width * sx + 0.5);
    double scaled_height = floor((double)surface->height * sy + 0.5);
    if (scaled_width < 1.0) scaled_width = 1.0;
    if (scaled_height < 1.0) scaled_height = 1.0;
    if (scaled_width > METAL_MAX_DIMENSION || scaled_height > METAL_MAX_DIMENSION)
        return metal_fail("scaled colour surface dimensions exceed %u",
                          METAL_MAX_DIMENSION);
    *width = (uint32_t)scaled_width;
    *height = (uint32_t)scaled_height;
    return 1;
}

static uint64_t hash_bytes(const uint8_t *bytes, size_t size)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t i = 0;
    for (; i + 8u <= size; i += 8u) {
        uint64_t value;
        memcpy(&value, bytes + i, sizeof value);
        hash = (hash ^ value) * UINT64_C(0x100000001B3);
        hash ^= hash >> 29;
    }
    for (; i < size; i++)
        hash = (hash ^ bytes[i]) * UINT64_C(0x100000001B3);
    return hash;
}

static uint64_t sparse_hash(const uint8_t *bytes, size_t size)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    if (size < 8u)
        return 0;
    for (size_t i = 0; i < 128u; i++) {
        uint64_t value;
        size_t offset = ((size - 8u) * i / 127u) & ~(size_t)7u;
        memcpy(&value, bytes + offset, sizeof value);
        hash = (hash ^ value) * UINT64_C(0x100000001B3);
        hash ^= hash >> 29;
    }
    return hash;
}

static int span_valid(uint32_t address, uint32_t width, uint32_t height,
                      uint32_t pitch)
{
    uint64_t row, end;

    if (!width || !height || (address & 3u) || (pitch & 3u))
        return 0;
    row = (uint64_t)width * 4u;
    if (pitch < row)
        return 0;
    end = (uint64_t)address + (uint64_t)(height - 1u) * pitch + row;
    for (unsigned i = 0; i < 2; i++)
        if (address >= s_guest_regions[i].start && end <= s_guest_regions[i].end)
            return 1;
    return 0;
}

static int byte_span_valid(uint32_t address, uint64_t bytes)
{
    uint64_t end = (uint64_t)address + bytes;
    if (!bytes || end < address)
        return 0;
    for (unsigned i = 0; i < 2; i++)
        if (address >= s_guest_regions[i].start && end <= s_guest_regions[i].end)
            return 1;
    return 0;
}

static uint64_t span_end(uint32_t address, uint32_t width, uint32_t height,
                         uint32_t pitch)
{
    return (uint64_t)address + (uint64_t)(height - 1u) * pitch + (uint64_t)width * 4u;
}

static int depth_overlaps(uint32_t address, uint64_t end)
{
    for (NSNumber *key in s_depth_surfaces) {
        BWMetalDepthSurface *depth = s_depth_surfaces[key];
        if ((uint64_t)address < depth->end && (uint64_t)depth->va < end)
            return 1;
    }
    return 0;
}

#define METAL_MAX_TEXTURES 1024u

static int texture_cache_hit(BWMetalTextureEntry *entry, uint32_t offset,
                             size_t size, uint64_t *full_hash)
{
    const uint8_t *bytes = guest_pointer(offset);
    if (entry) {
        entry->last_use = s_frame;
        if (entry->frame == s_frame)
            return 1;
        if (entry->stable >= 30u &&
            ((s_frame + (offset >> 12)) & 31u) != 0u) {
            uint64_t sparse = sparse_hash(bytes, size);
            if (sparse == entry->sparse) {
                entry->frame = s_frame;
                return 1;
            }
        }
    }
    *full_hash = hash_bytes(bytes, size);
    if (!entry || entry->hash != *full_hash)
        return 0;
    if (entry->stable != UINT32_MAX)
        entry->stable++;
    if (entry->stable == 30u)
        entry->sparse = sparse_hash(bytes, size);
    entry->frame = s_frame;
    return 1;
}

static int cache_texture(NSData *key, BWMetalTextureEntry *entry,
                         id<MTLTexture> texture, uint32_t offset,
                         uint64_t end, uint64_t hash)
{
    if (!s_textures)
        s_textures = [NSMutableDictionary new];
    if (!s_textures)
        return metal_fail("could not create texture cache");
    if (!entry) {
        if (s_textures.count >= METAL_MAX_TEXTURES) {
            NSData *oldest_key = nil;
            uint32_t oldest_frame = UINT32_MAX;
            for (NSData *candidate in s_textures) {
                BWMetalTextureEntry *candidate_entry = s_textures[candidate];
                if (!oldest_key || candidate_entry->last_use < oldest_frame) {
                    oldest_key = candidate;
                    oldest_frame = candidate_entry->last_use;
                }
            }
            if (oldest_key)
                [s_textures removeObjectForKey:oldest_key];
        }
        entry = [BWMetalTextureEntry new];
        if (!entry)
            return metal_fail("could not create texture cache entry");
        s_textures[key] = entry;
    }
    entry->texture = texture;
    entry->hash = hash;
    entry->sparse = 0;
    entry->end = end;
    entry->offset = offset;
    entry->frame = s_frame;
    entry->last_use = s_frame;
    entry->stable = 0;
    return 1;
}

static void invalidate_texture_cache(uint32_t address, uint64_t end)
{
    NSMutableArray<NSData *> *keys = nil;
    for (NSData *key in s_textures) {
        BWMetalTextureEntry *entry = s_textures[key];
        if ((uint64_t)address < entry->end && (uint64_t)entry->offset < end) {
            if (!keys)
                keys = [NSMutableArray new];
            [keys addObject:key];
        }
    }
    if (keys)
        [s_textures removeObjectsForKeys:keys];
}

static int surface_valid_for(const Nv2aSurface *s, uint32_t format,
                             uint32_t clip_x, uint32_t clip_y)
{
    uint32_t color = format & 0xFu, aa = (format >> 12) & 0xFu;
    if (!s || s->bytes_per_pixel != 4 || ((format >> 8) & 0xFu) > 1u ||
        !((color == METAL_COLOR_ARGB8 && aa == 0u && s->aa_sx == 1u && s->aa_sy == 1u) ||
          (color == METAL_COLOR_XRGB_ZERO && ((format >> 8) & 0xFu) == 1u &&
           s->aa_sy == 1u &&
           ((aa == 0u && s->aa_sx == 1u) || (aa == 1u && s->aa_sx == 2u)))))
        return metal_fail("unsupported colour surface format or anti-aliasing");
    if (s->width > METAL_MAX_DIMENSION || s->height > METAL_MAX_DIMENSION)
        return metal_fail("colour surface dimensions exceed %u", METAL_MAX_DIMENSION);
    if (clip_x || clip_y)
        return metal_fail("nonzero surface clip origin is unsupported");
    if (!span_valid(s->color_va, s->width, s->height, s->pitch))
        return metal_fail("invalid colour surface guest-memory span");
    return 1;
}

static int surface_valid(const Nv2aSurface *s)
{
    return surface_valid_for(s, bw_backend_reg(0x0208u),
                             bw_backend_reg(0x0200u) & 0xFFFFu,
                             bw_backend_reg(0x0204u) & 0xFFFFu);
}

static int same_surface(const Nv2aSurface *a, const Nv2aSurface *b)
{
    return a->color_va == b->color_va && a->width == b->width &&
           a->height == b->height && a->pitch == b->pitch &&
           a->bytes_per_pixel == b->bytes_per_pixel &&
           a->aa_sx == b->aa_sx && a->aa_sy == b->aa_sy;
}

static int ensure_surface_upload(uint32_t width, uint32_t height)
{
    if (!pending_command() || !s_encoding_submission)
        return 0;
    if (s_encoding_submission->surface_upload &&
        s_encoding_submission->surface_upload.width == width &&
        s_encoding_submission->surface_upload.height == height)
        return 1;
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width height:height mipmapped:NO];
    if (!desc)
        return metal_fail("could not create colour surface upload descriptor");
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    s_encoding_submission->surface_upload = [s_device newTextureWithDescriptor:desc];
    if (!s_encoding_submission->surface_upload)
        return metal_fail("could not create colour surface upload texture");
    return 1;
}

static int ensure_surface_readback(uint32_t width, uint32_t height)
{
    if (!pending_command() || !s_encoding_submission)
        return 0;
    if (s_encoding_submission->surface_readback &&
        s_encoding_submission->surface_readback.width == width &&
        s_encoding_submission->surface_readback.height == height)
        return 1;
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width height:height mipmapped:NO];
    if (!desc)
        return metal_fail("could not create colour surface readback descriptor");
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageRenderTarget;
    s_encoding_submission->surface_readback = [s_device newTextureWithDescriptor:desc];
    if (!s_encoding_submission->surface_readback)
        return metal_fail("could not create colour surface readback texture");
    return 1;
}

static int ensure_feedback_resolve(uint32_t width, uint32_t height)
{
    if (!pending_command() || !s_encoding_submission)
        return 0;
    if (s_encoding_submission->feedback_resolve &&
        s_encoding_submission->feedback_resolve.width == width &&
        s_encoding_submission->feedback_resolve.height == height)
        return 1;
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width height:height mipmapped:NO];
    if (!desc)
        return metal_fail("could not create feedback resolve descriptor");
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s_encoding_submission->feedback_resolve = [s_device newTextureWithDescriptor:desc];
    if (!s_encoding_submission->feedback_resolve)
        return metal_fail("could not create feedback resolve texture");
    return 1;
}

static int encode_surface_copy(id<MTLTexture> source, id<MTLTexture> destination,
                               int box_filter)
{
    end_draw_encoder();
    id<MTLCommandBuffer> command = pending_command();
    if (!command)
        return 0;
    if (source.width == destination.width && source.height == destination.height) {
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!blit)
            return metal_fail("could not create colour surface copy command");
        [blit copyFromTexture:source sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(source.width, source.height, 1)
                   toTexture:destination destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        return 1;
    }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (!pass)
        return metal_fail("could not create colour surface scaling pass");
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder)
        return metal_fail("could not create colour surface scaling command");
    MetalScaleUniforms uniforms = {
        (uint32_t)source.width, (uint32_t)source.height,
        (uint32_t)destination.width, (uint32_t)destination.height,
    };
    [encoder setRenderPipelineState:box_filter ? s_box_pipeline : s_scale_pipeline];
    [encoder setFragmentTexture:source atIndex:0];
    if (box_filter)
        [encoder setFragmentBytes:&uniforms length:sizeof uniforms atIndex:0];
    else
        [encoder setFragmentSamplerState:s_sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    return 1;
}

static int mark_pending_command_reason(uint32_t reason)
{
    if (!s_pending_command || !s_encoding_submission)
        return metal_fail("Metal command reason has no pending submission");
    s_encoding_submission->reason_mask |= reason;
    return 1;
}

static int seed_color_surface(id<MTLTexture> destination,
                              const Nv2aSurface *surface,
                              const void *pixels, size_t row)
{
    if (!ensure_surface_upload(surface->width, surface->height) ||
        !mark_pending_command_reason(METAL_COMMAND_REASON_SURFACE_UPLOAD))
        return 0;
    [s_encoding_submission->surface_upload
        replaceRegion:MTLRegionMake2D(0, 0, surface->width, surface->height)
          mipmapLevel:0 withBytes:pixels bytesPerRow:row];
    s_surface_seed_count++;
    return encode_surface_copy(s_encoding_submission->surface_upload, destination, 0);
}

static int ensure_surface_sync_pixels(BWMetalColorSurface *entry, size_t bytes)
{
    if (entry->guest_sync.length == bytes)
        return 1;
    entry->guest_sync = [NSMutableData dataWithLength:bytes];
    if (!entry->guest_sync)
        return metal_fail("could not allocate colour surface synchronization snapshot");
    return 1;
}

static int record_surface_sync_from_guest(BWMetalColorSurface *entry,
                                          const Nv2aSurface *surface,
                                          uint32_t format)
{
    size_t row = (size_t)surface->width * 4u;
    if ((size_t)surface->height > SIZE_MAX / row ||
        !ensure_surface_sync_pixels(entry, row * surface->height))
        return 0;
    for (uint32_t y = 0; y < surface->height; y++)
        memcpy((uint8_t *)entry->guest_sync.mutableBytes + (size_t)y * row,
               guest_pointer(surface->color_va) + (size_t)y * surface->pitch,
               row);
    entry->row = row;
    entry->surface = *surface;
    entry->format = format;
    entry->sync_valid = 1;
    return 1;
}

static int surface_matches_guest_sync(BWMetalColorSurface *entry,
                                      const Nv2aSurface *surface,
                                      uint32_t format)
{
    if (!entry->sync_valid || format != entry->format ||
        !same_surface(surface, &entry->surface))
        return 0;
    for (uint32_t y = 0; y < surface->height; y++)
        if (memcmp(guest_pointer(surface->color_va) + (size_t)y * surface->pitch,
                   (const uint8_t *)entry->guest_sync.bytes + (size_t)y * entry->row,
                   entry->row))
            return 0;
    return 1;
}

static NSMutableData *copy_guest_surface(const Nv2aSurface *surface, size_t row)
{
    if ((size_t)surface->height > SIZE_MAX / row)
        return metal_fail("colour surface guest copy size overflows"), nil;
    NSMutableData *copy = [NSMutableData dataWithLength:row * surface->height];
    if (!copy)
        return metal_fail("could not allocate colour surface guest copy"), nil;
    for (uint32_t y = 0; y < surface->height; y++)
        memcpy((uint8_t *)copy.mutableBytes + (size_t)y * row,
               guest_pointer(surface->color_va) + (size_t)y * surface->pitch,
               row);
    return copy;
}

static int publish_cached_surface(BWMetalColorSurface *entry,
                                  const Nv2aSurface *surface,
                                  uint32_t format, int merge_guest_changes,
                                  uint32_t reason)
{
    size_t row = (size_t)surface->width * 4u;
    if ((size_t)surface->height > SIZE_MAX / row)
        return metal_fail("colour surface publication size overflows");
    size_t bytes = row * surface->height;
    NSMutableData *guest_before = copy_guest_surface(surface, row);
    NSMutableData *pixels = [NSMutableData dataWithLength:bytes];
    if (!guest_before || !pixels)
        return pixels ? 0 : metal_fail("could not allocate colour surface publication buffer");
    if (!ensure_surface_readback(surface->width, surface->height) ||
        !s_encoding_submission ||
        !mark_pending_command_reason(METAL_COMMAND_REASON_SURFACE_READBACK |
                                     reason))
        return 0;
    id<MTLTexture> readback = s_encoding_submission->surface_readback;
    if (!readback || !encode_surface_copy(entry->texture, readback, 1) ||
        !flush_commands())
        return 0;
    [readback getBytes:pixels.mutableBytes bytesPerRow:row
            fromRegion:MTLRegionMake2D(0, 0, surface->width, surface->height)
           mipmapLevel:0];

    int guest_changed = 0;
    if (merge_guest_changes && entry->sync_valid &&
        entry->guest_sync.length == bytes &&
        format == entry->format && same_surface(surface, &entry->surface)) {
        const uint8_t *base = entry->guest_sync.bytes;
        const uint8_t *guest = guest_before.bytes;
        uint8_t *merged = pixels.mutableBytes;
        for (size_t offset = 0; offset < bytes; offset += 4u) {
            if (memcmp(guest + offset, base + offset, 4u)) {
                memcpy(merged + offset, guest + offset, 4u);
                guest_changed = 1;
            }
        }
    }

    if (!ensure_surface_sync_pixels(entry, bytes))
        return 0;
    memcpy(entry->guest_sync.mutableBytes, pixels.bytes, bytes);
    for (uint32_t y = 0; y < surface->height; y++)
        memcpy(guest_pointer(surface->color_va) + (size_t)y * surface->pitch,
               (const uint8_t *)pixels.bytes + (size_t)y * row, row);
    entry->row = row;
    entry->surface = *surface;
    entry->format = format;
    entry->sync_valid = 1;
    entry->published_generation = entry->completed_generation;
    entry->private_dirty = 0;
    s_guest_publications++;
    s_guest_publication_bytes += bytes;
    invalidate_texture_cache(surface->color_va,
                             span_end(surface->color_va, surface->width,
                                      surface->height, surface->pitch));

    if (guest_changed) {
        if (!seed_color_surface(entry->texture, surface,
                                entry->guest_sync.bytes, entry->row) ||
            !flush_commands())
            return 0;
        entry->private_dirty = 0;
    }
    return 1;
}

static int prepare_cached_surface(BWMetalColorSurface *entry,
                                  const Nv2aSurface *surface,
                                  uint32_t format)
{
    if (surface_matches_guest_sync(entry, surface, format))
        return 1;
    if (entry->private_dirty &&
        !publish_cached_surface(entry, surface, format, 1, 0))
        return 0;
    if (surface_matches_guest_sync(entry, surface, format))
        return 1;
    if (!record_surface_sync_from_guest(entry, surface, format) ||
        !seed_color_surface(entry->texture, surface,
                            entry->guest_sync.bytes, entry->row))
        return 0;
    entry->private_dirty = 0;
    entry->published_generation = entry->completed_generation;
    return 1;
}

static int publish_overlapping_color_surfaces(uint32_t address, uint64_t end)
{
    NSArray<BWMetalColorSurface *> *entries = s_color_surfaces.allValues;
    for (BWMetalColorSurface *entry in entries) {
        if (!entry->private_dirty || !entry->sync_valid)
            continue;
        uint64_t surface_end =
            span_end(entry->surface.color_va, entry->surface.width,
                     entry->surface.height, entry->surface.pitch);
        if ((uint64_t)address < surface_end &&
            (uint64_t)entry->surface.color_va < end &&
            !publish_cached_surface(entry, &entry->surface,
                                    entry->format, 1,
                                    METAL_COMMAND_REASON_FEEDBACK))
            return 0;
    }
    return 1;
}

static int publish_all_dirty_color_surfaces(void)
{
    NSArray<BWMetalColorSurface *> *entries = s_color_surfaces.allValues;
    for (BWMetalColorSurface *entry in entries)
        if (entry->private_dirty && entry->sync_valid &&
            !publish_cached_surface(entry, &entry->surface,
                                    entry->format, 1, 0))
            return 0;
    return wait_for_all_submissions();
}

static int writeback(void)
{
    size_t row, bytes;
    id<MTLTexture> readback = nil;

    if (s_surface && s_dirty) {
        if (!ensure_surface_readback(s_current.width, s_current.height) ||
            !s_encoding_submission ||
            !(readback = s_encoding_submission->surface_readback) ||
            !mark_pending_command_reason(METAL_COMMAND_REASON_SURFACE_READBACK) ||
            !encode_surface_copy(s_surface, readback, 1))
            return 0;
    }
    if (!flush_commands())
        return 0;
    if (!s_surface || !s_dirty)
        return 1;
    row = (size_t)s_current.width * 4u;
    if ((size_t)s_current.height > SIZE_MAX / row)
        return metal_fail("colour surface readback size overflows");
    bytes = row * s_current.height;
    if (!s_surface_entry)
        return metal_fail("active colour surface has no cache entry");
    if (!ensure_surface_sync_pixels(s_surface_entry, bytes))
        return 0;
    [readback getBytes:s_surface_entry->guest_sync.mutableBytes bytesPerRow:row
            fromRegion:MTLRegionMake2D(0, 0, s_current.width, s_current.height)
           mipmapLevel:0];
    for (uint32_t y = 0; y < s_current.height; y++)
        memcpy(guest_pointer(s_current.color_va) + (size_t)y * s_current.pitch,
               (const uint8_t *)s_surface_entry->guest_sync.bytes + (size_t)y * row, row);
    s_surface_entry->row = row;
    s_surface_entry->surface = s_current;
    s_surface_entry->format = s_current_format;
    s_surface_entry->sync_valid = 1;
    s_surface_entry->private_dirty = 0;
    s_surface_entry->published_generation =
        s_surface_entry->completed_generation;
    s_guest_publications++;
    s_guest_publication_bytes += bytes;
    invalidate_texture_cache(s_current.color_va,
                             span_end(s_current.color_va, s_current.width,
                                      s_current.height, s_current.pitch));
    s_dirty = 0;
    return 1;
}

static BWMetalColorSurface *cached_surface(const Nv2aSurface *s, uint32_t format)
{
    uint32_t physical_width, physical_height;
    if (!surface_physical_size(s, &physical_width, &physical_height))
        return nil;
    MetalColorSurfaceKey value = {
        .color_va = s->color_va,
        .width = s->width,
        .height = s->height,
        .pitch = s->pitch,
        .physical_width = physical_width,
        .physical_height = physical_height,
        .format = format,
        .bytes_per_pixel = s->bytes_per_pixel,
        .aa_sx = s->aa_sx,
        .aa_sy = s->aa_sy,
        .scale_epoch = s_render_scale_epoch,
    };
    NSData *key = [NSData dataWithBytes:&value length:sizeof value];
    BWMetalColorSurface *entry = s_color_surfaces[key];
    if (entry)
        return entry;
    if (!s_color_surfaces)
        s_color_surfaces = [NSMutableDictionary new];
    if (!s_color_surfaces)
        return metal_fail("could not create colour surface cache"), nil;
    if (s_color_surfaces.count >= METAL_MAX_COLOR_SURFACES)
        return metal_fail("colour surface cache is full"), nil;
    MTLTextureDescriptor *desc;
    desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:physical_width
                                                            height:physical_height
                                                         mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    entry = [BWMetalColorSurface new];
    if (!entry)
        return metal_fail("could not create colour surface cache entry"), nil;
    entry->texture = [s_device newTextureWithDescriptor:desc];
    if (!entry->texture)
        return metal_fail("could not create colour surface %ux%u (guest %ux%u)",
                          physical_width, physical_height, s->width, s->height), nil;
    s_color_surfaces[key] = entry;
    return entry;
}

static int track_dirty_surface(BWMetalSubmission *submission)
{
    if (!s_dirty)
        return 1;
    if (!submission || !s_surface_entry)
        return metal_fail("dirty colour surface has no submission owner");
    for (size_t i = 0; i < submission->color_entry_count; i++)
        if (submission->color_entries[i] == s_surface_entry)
            return 1;
    if (submission->color_entry_count >= METAL_MAX_COLOR_SURFACES)
        return metal_fail("GPU submission touched too many colour surfaces");
    uint64_t generation = ++s_surface_generation;
    if (!generation)
        return metal_fail("colour surface generation overflowed");
    size_t index = submission->color_entry_count++;
    submission->color_entries[index] = s_surface_entry;
    submission->color_generations[index] = generation;
    s_surface_entry->submitted_generation = generation;
    s_surface_entry->private_dirty = 1;
    return 1;
}

static int prepare_surface(const Nv2aSurface *s)
{
    uint32_t format = bw_backend_reg(0x0208u) & 0xFu;
    BWMetalColorSurface *entry;

    if (!surface_valid(s))
        return 0;
    if (depth_overlaps(s->color_va, span_end(s->color_va, s->width, s->height, s->pitch)))
        return metal_fail("colour surface aliases a cleared depth/stencil surface");
    if (s_surface && same_surface(s, &s_current) && format == s_current_format)
        return 1;
    if (s_pending_command) {
        end_draw_encoder();
        BWMetalSubmission *submission = s_encoding_submission;
        if (!submission ||
            !mark_pending_command_reason(METAL_COMMAND_REASON_TARGET_SWITCH) ||
            !track_dirty_surface(submission))
            return 0;
        s_dirty = 0;
        submission->target_switches++;
        if (submission->target_switches >=
                METAL_MAX_TARGET_SWITCHES_PER_SUBMISSION &&
            !commit_commands_async())
            return 0;
    } else if (!retire_completed_submissions()) {
        return 0;
    }
    entry = cached_surface(s, format);
    if (!entry)
        return 0;
    if (surface_matches_guest_sync(entry, s, format)) {
        s_surface_cache_reuses++;
    } else {
        if (!prepare_cached_surface(entry, s, format))
            return 0;
    }
    s_surface_entry = entry;
    s_surface = entry->texture;
    s_current = *s;
    s_current_format = format;
    s_dirty = 0;
    return 1;
}

static int validate_unmodified_surface(const Nv2aSurface *s)
{
    if (s_dirty)
        return 1;
    if (!surface_valid(s))
        return 0;
    if (depth_overlaps(s->color_va, span_end(s->color_va, s->width, s->height, s->pitch)))
        return metal_fail("colour surface aliases a cleared depth/stencil surface");
    return 1;
}

static BWMetalDepthSurface *prepare_depth_surface(const Nv2aSurface *s,
                                                   const Nv2aRenderState *rs)
{
    uint32_t format, pitch, physical_width, physical_height;
    uint64_t end;
    BWMetalDepthSurface *depth;
    MTLTextureDescriptor *desc;

    if (!rs || !rs->zeta_va)
        return metal_fail("depth/stencil clear requires a bound depth surface"), nil;
    if (!surface_valid(s))
        return nil;
    if (!surface_physical_size(s, &physical_width, &physical_height))
        return nil;
    format = bw_backend_reg(0x0208u);
    if (((format >> 4) & 0xFu) != 2u || ((format >> 8) & 0xFu) != 1u)
        return metal_fail("unsupported depth/stencil surface format or layout"), nil;
    pitch = bw_backend_reg(0x020Cu) >> 16;
    if (!span_valid(rs->zeta_va, s->width, s->height, pitch))
        return metal_fail("invalid depth/stencil surface guest-memory span"), nil;
    end = span_end(rs->zeta_va, s->width, s->height, pitch);
    if ((uint64_t)rs->zeta_va < span_end(s->color_va, s->width, s->height, s->pitch) &&
        (uint64_t)s->color_va < end)
        return metal_fail("depth/stencil surface aliases the colour surface"), nil;
    if (s_surface &&
        (uint64_t)rs->zeta_va < span_end(s_current.color_va, s_current.width,
                                          s_current.height, s_current.pitch) &&
        (uint64_t)s_current.color_va < end)
        return metal_fail("depth/stencil surface aliases a pending colour surface"), nil;

    depth = s_depth_surfaces[@(rs->zeta_va)];
    if (depth) {
        if (depth->guest_width != s->width || depth->guest_height != s->height ||
            depth->width != physical_width || depth->height != physical_height ||
            depth->pitch != pitch || depth->scale_epoch != s_render_scale_epoch)
            return metal_fail("reinterpreting a depth/stencil surface is unsupported"), nil;
        return depth;
    }
    if (depth_overlaps(rs->zeta_va, end))
        return metal_fail("overlapping depth/stencil surfaces are unsupported"), nil;
    if (!s_depth_surfaces)
        s_depth_surfaces = [NSMutableDictionary new];
    if (!s_depth_surfaces)
        return metal_fail("could not create depth/stencil surface cache"), nil;
    if (s_depth_surfaces.count >= METAL_MAX_DEPTH_SURFACES)
        return metal_fail("depth/stencil surface cache is full"), nil;
    desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                             width:physical_width
                                                            height:physical_height
                                                         mipmapped:NO];
    if (!desc)
        return metal_fail("could not create depth/stencil surface descriptor"), nil;
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget;
    depth = [BWMetalDepthSurface new];
    if (!depth)
        return metal_fail("could not create depth/stencil surface metadata"), nil;
    depth->texture = [s_device newTextureWithDescriptor:desc];
    if (!depth->texture)
        return metal_fail("could not create depth/stencil surface"), nil;
    depth->va = rs->zeta_va;
    depth->width = physical_width;
    depth->height = physical_height;
    depth->guest_width = s->width;
    depth->guest_height = s->height;
    depth->pitch = pitch;
    depth->scale_epoch = s_render_scale_epoch;
    depth->end = end;
    s_depth_surfaces[@(depth->va)] = depth;
    return depth;
}

static int finish_command(id<MTLCommandBuffer> command)
{
    [command commit];
    s_command_submissions++;
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
        return metal_fail("GPU command failed: %s", metal_error_text(command.error));
    return 1;
}

static int command_terminal(id<MTLCommandBuffer> command)
{
    MTLCommandBufferStatus status = command.status;
    return status == MTLCommandBufferStatusCompleted ||
           status == MTLCommandBufferStatusError;
}

static void record_submission_timing(BWMetalSubmission *submission)
{
    id<MTLCommandBuffer> command = submission->command;
    if (s_frame_timing_enabled) {
        CFTimeInterval start = command.GPUStartTime;
        CFTimeInterval end = command.GPUEndTime;
        double duration = (end - start) * 1.0e9;
        if (isfinite(start) && isfinite(end) && end > start &&
            isfinite(duration) && duration > 0.0 &&
            duration <= (double)(UINT64_MAX - s_frame_gpu_command_ns)) {
            uint64_t nanoseconds = (uint64_t)llround(duration);
            s_frame_gpu_command_ns += nanoseconds;
            s_frame_gpu_command_valid = 1;
            uint64_t *total = submission->presented
                            ? &s_present_command_gpu_ns
                            : &s_offscreen_command_gpu_ns;
            uint64_t *count = submission->presented
                            ? &s_present_command_count
                            : &s_offscreen_command_count;
            if (UINT64_MAX - *total >= nanoseconds) {
                *total += nanoseconds;
                (*count)++;
            }
            int recorded_reason = 0;
            for (size_t i = 0;
                 i < sizeof s_command_reason_timings /
                         sizeof s_command_reason_timings[0];
                 i++) {
                MetalCommandReasonTiming *timing = &s_command_reason_timings[i];
                if (!timing->count ||
                    timing->reason_mask == submission->reason_mask) {
                    if (!timing->count)
                        timing->reason_mask = submission->reason_mask;
                    if (UINT64_MAX - timing->nanoseconds >= nanoseconds) {
                        timing->nanoseconds += nanoseconds;
                        timing->count++;
                        recorded_reason = 1;
                    }
                    break;
                }
            }
            if (!recorded_reason)
                s_command_reason_timing_overflows++;
            if (submission->color_entry_count) {
                BWMetalColorSurface *entry =
                    submission->color_entries[submission->color_entry_count - 1u];
                for (size_t i = 0;
                     i < sizeof s_command_role_surfaces /
                             sizeof s_command_role_surfaces[0];
                     i++) {
                    MetalCommandRoleSurface *role = &s_command_role_surfaces[i];
                    if (!role->count ||
                        (role->color_va == entry->surface.color_va &&
                         role->width == entry->surface.width &&
                         role->height == entry->surface.height &&
                         role->presented == submission->presented)) {
                        if (!role->count) {
                            role->color_va = entry->surface.color_va;
                            role->width = entry->surface.width;
                            role->height = entry->surface.height;
                            role->presented = submission->presented;
                        }
                        if (UINT64_MAX - role->nanoseconds >= nanoseconds) {
                            role->nanoseconds += nanoseconds;
                            role->count++;
                        }
                        break;
                    }
                }
            }
        } else {
            s_gpu_command_timestamp_failures++;
        }
    }
    if (submission->water_command && s_water_benchmark_enabled &&
        !s_water_benchmark_complete && !s_water_benchmark_failed) {
        CFTimeInterval start = command.GPUStartTime;
        CFTimeInterval end = command.GPUEndTime;
        double duration = (end - start) * 1.0e9;
        if (!isfinite(start) || !isfinite(end) || end <= start ||
            !isfinite(duration) || duration <= 0.0 ||
            duration > (double)(UINT64_MAX - s_frame_water_gpu_ns)) {
            s_frame_water_gpu_invalid = 1;
        } else {
            s_frame_water_gpu_ns += (uint64_t)llround(duration);
            s_frame_water_gpu_valid = 1;
        }
    }
}

static int retire_oldest_submission(int wait, int saturation_wait)
{
    if (!s_inflight_submissions.count)
        return 1;
    BWMetalSubmission *submission = s_inflight_submissions[0];
    if (!wait && !command_terminal(submission->command))
        return 1;
    uint64_t wait_start = wait && s_frame_timing_enabled ? xbox_HostNowNs() : 0;
    if (wait)
        [submission->command waitUntilCompleted];
    uint64_t wait_end = wait && s_frame_timing_enabled ? xbox_HostNowNs() : 0;
    if (wait_start && wait_end > wait_start) {
        uint64_t elapsed = wait_end - wait_start;
        if (UINT64_MAX - s_frame_command_wait_ns >= elapsed)
            s_frame_command_wait_ns += elapsed;
        if (saturation_wait &&
            UINT64_MAX - s_queue_saturation_wait_ns >= elapsed)
            s_queue_saturation_wait_ns += elapsed;
    }
    int result = 1;
    int failed = submission->command.status != MTLCommandBufferStatusCompleted;
#if defined(BW_METAL_TESTING)
    failed = failed || submission->test_failure;
#endif
    if (failed) {
        result = metal_fail("GPU command failed: %s",
                            submission->command.error
                                ? metal_error_text(submission->command.error)
                                : "synthetic test failure");
    } else {
        for (size_t i = 0; i < submission->color_entry_count; i++)
            if (submission->color_generations[i] >
                submission->color_entries[i]->completed_generation)
                submission->color_entries[i]->completed_generation =
                    submission->color_generations[i];
        record_submission_timing(submission);
        CFTimeInterval completion_time = submission->command.GPUEndTime;
        double latency = (completion_time - submission->commit_time) * 1.0e9;
        if (isfinite(submission->commit_time) &&
            isfinite(completion_time) &&
            completion_time > submission->commit_time &&
            isfinite(latency) && latency > 0.0 &&
            latency <= (double)UINT64_MAX &&
            s_submission_latency_time_count < METAL_MAX_FRAME_SAMPLES) {
            s_submission_latency_times[s_submission_latency_time_count++] =
                (uint64_t)llround(latency);
            if (s_frame_timing_enabled &&
                s_submission_latency_time_count >= 300u &&
                !(s_submission_latency_time_count % 100u))
                report_duration_times(
                    "submission latency", "300-command window",
                    s_submission_latency_times +
                        s_submission_latency_time_count - 300u,
                    300u);
        }
    }
    [s_inflight_submissions removeObjectAtIndex:0];
    submission->command = nil;
    for (size_t i = 0; i < submission->color_entry_count; i++) {
        submission->color_entries[i] = nil;
        submission->color_generations[i] = 0;
    }
    submission->color_entry_count = 0;
    submission->target_switches = 0;
    submission->commit_time = 0.0;
    submission->frame = 0;
    submission->water_command = 0;
    submission->presented = 0;
#if defined(BW_METAL_TESTING)
    submission->test_failure = 0;
#endif
    submission->vertex_upload_offset = 0;
    if (!s_free_submissions)
        s_free_submissions = [NSMutableArray new];
    if (s_free_submissions)
        [s_free_submissions addObject:submission];
    return result;
}

static int retire_completed_submissions(void)
{
#if defined(BW_METAL_TESTING)
    if (s_test_defer_retirement)
        return 1;
#endif
    while (s_inflight_submissions.count &&
           command_terminal(s_inflight_submissions[0]->command))
        if (!retire_oldest_submission(0, 0))
            return 0;
    return 1;
}

static int wait_for_all_submissions(void)
{
    while (s_inflight_submissions.count)
        if (!retire_oldest_submission(1, 0))
            return 0;
    return 1;
}

static id<MTLCommandBuffer> pending_command(void)
{
    if (!retire_completed_submissions())
        return nil;
    if (!s_pending_command) {
        while (s_inflight_submissions.count >= METAL_MAX_IN_FLIGHT) {
            s_queue_saturation_waits++;
            if (!retire_oldest_submission(1, 1))
                return nil;
        }
        BWMetalSubmission *submission = nil;
        if (s_free_submissions.count) {
            submission = s_free_submissions.lastObject;
            [s_free_submissions removeLastObject];
        } else {
            submission = [BWMetalSubmission new];
        }
        if (!submission)
            return metal_fail("could not allocate GPU submission record"), nil;
        submission->command = [s_queue commandBuffer];
        submission->frame = s_frame;
        submission->vertex_upload_offset = 0;
        submission->reason_mask = 0;
        s_encoding_submission = submission;
        s_pending_command = submission->command;
    }
    if (!s_pending_command)
        metal_fail("could not create pending GPU command");
    return s_pending_command;
}

static int submit_commands(int wait)
{
    end_draw_encoder();
    if (s_pending_command) {
        BWMetalSubmission *submission = s_encoding_submission;
        if (!submission || submission->command != s_pending_command)
            return metal_fail("pending GPU command has no submission owner");
        if (!track_dirty_surface(submission))
            return 0;
        submission->water_command = s_pending_supported_water;
        s_pending_supported_water = 0;
#if defined(BW_METAL_TESTING)
        submission->test_failure = s_test_fail_next_submission;
        s_test_fail_next_submission = 0;
#endif
        submission->commit_time = CACurrentMediaTime();
        [submission->command commit];
        s_command_submissions++;
        if (!s_inflight_submissions)
            s_inflight_submissions = [NSMutableArray new];
        if (!s_inflight_submissions)
            return metal_fail("could not allocate GPU submission queue");
        [s_inflight_submissions addObject:submission];
        if (s_inflight_submissions.count > s_queue_depth_high_water)
            s_queue_depth_high_water = (unsigned)s_inflight_submissions.count;
        s_pending_command = nil;
        s_encoding_submission = nil;
    } else {
        s_pending_supported_water = 0;
    }
    if (wait)
        return wait_for_all_submissions();
    return retire_completed_submissions();
}

static int flush_commands(void)
{
    return submit_commands(1);
}

static int commit_commands_async(void)
{
    return submit_commands(0);
}

static int upload_vertices(size_t length, id<MTLBuffer> *buffer,
                           NSUInteger *offset, void **contents)
{
    if (!pending_command() || !s_encoding_submission)
        return 0;
    BWMetalSubmission *submission = s_encoding_submission;
    size_t aligned = (submission->vertex_upload_offset + 255u) & ~(size_t)255u;
    if (aligned < submission->vertex_upload_offset)
        return metal_fail("vertex upload offset overflows");
    if (!submission->vertex_upload ||
        aligned > submission->vertex_upload_capacity ||
        length > submission->vertex_upload_capacity - aligned) {
        size_t capacity = submission->vertex_upload_capacity;
        if (capacity < METAL_INITIAL_VERTEX_UPLOAD)
            capacity = METAL_INITIAL_VERTEX_UPLOAD;
        while (capacity < length) {
            if (capacity > SIZE_MAX / 2u)
                return metal_fail("vertex upload size overflows");
            capacity *= 2u;
        }
        if (capacity > s_device.maxBufferLength)
            return metal_fail("vertex upload exceeds Metal buffer limit");
        submission->vertex_upload =
            [s_device newBufferWithLength:capacity options:MTLResourceStorageModeShared];
        if (!submission->vertex_upload || !submission->vertex_upload.contents)
            return metal_fail("could not allocate vertex upload buffer");
        submission->vertex_upload_capacity = capacity;
        submission->vertex_upload_offset = 0;
        aligned = 0;
        s_vertex_upload_allocations++;
    }
    *buffer = submission->vertex_upload;
    *offset = aligned;
    *contents = (uint8_t *)submission->vertex_upload.contents + aligned;
    submission->vertex_upload_offset = aligned + length;
    return 1;
}

typedef struct {
    size_t depth_row, stencil_row, stencil_offset, total;
} MetalDepthTransfer;

static int depth_transfer_layout(BWMetalDepthSurface *depth, MetalDepthTransfer *layout)
{
    layout->depth_row = ((size_t)depth->width * sizeof(float) + 255u) & ~(size_t)255u;
    layout->stencil_row = ((size_t)depth->width + 255u) & ~(size_t)255u;
    if (depth->height > SIZE_MAX / layout->depth_row ||
        depth->height > SIZE_MAX / layout->stencil_row)
        return metal_fail("depth/stencil transfer size overflows");
    layout->stencil_offset = layout->depth_row * depth->height;
    size_t stencil_size = layout->stencil_row * depth->height;
    if (stencil_size > SIZE_MAX - layout->stencil_offset)
        return metal_fail("depth/stencil transfer size overflows");
    layout->total = layout->stencil_offset + stencil_size;
    if (layout->total > s_device.maxBufferLength)
        return metal_fail("depth/stencil transfer exceeds Metal buffer limit");
    return 1;
}

static int preserve_depth_surfaces(void)
{
    if (!s_depth_surfaces.count)
        return 1;
    if (!flush_commands())
        return 0;
    for (NSNumber *key in s_depth_surfaces) {
        BWMetalDepthSurface *depth = s_depth_surfaces[key];
        MetalDepthTransfer layout;
        if (!depth->initialized)
            continue;
        if (!depth_transfer_layout(depth, &layout))
            return 0;
        id<MTLBuffer> buffer =
            [s_device newBufferWithLength:layout.total options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [s_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!buffer || !buffer.contents || !command || !blit)
            return metal_fail("could not create depth/stencil preservation resources");
        MTLSize size = MTLSizeMake(depth->width, depth->height, 1);
        if (depth->initialized & 1u)
            [blit copyFromTexture:depth->texture sourceSlice:0 sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
                        toBuffer:buffer destinationOffset:0
           destinationBytesPerRow:layout.depth_row
        destinationBytesPerImage:layout.depth_row * depth->height
                         options:MTLBlitOptionDepthFromDepthStencil];
        if (depth->initialized & 2u)
            [blit copyFromTexture:depth->texture sourceSlice:0 sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
                        toBuffer:buffer destinationOffset:layout.stencil_offset
           destinationBytesPerRow:layout.stencil_row
        destinationBytesPerImage:layout.stencil_row * depth->height
                         options:MTLBlitOptionStencilFromDepthStencil];
        [blit endEncoding];
        if (!finish_command(command))
            return 0;
        for (uint32_t y = 0; y < depth->guest_height; y++) {
            uint32_t source_y = (uint32_t)(((uint64_t)y * 2u + 1u) * depth->height /
                                           ((uint64_t)depth->guest_height * 2u));
            if (source_y >= depth->height)
                source_y = depth->height - 1u;
            const float *z = (const float *)((const uint8_t *)buffer.contents +
                                             (size_t)source_y * layout.depth_row);
            const uint8_t *stencil = (const uint8_t *)buffer.contents +
                                     layout.stencil_offset +
                                     (size_t)source_y * layout.stencil_row;
            uint8_t *guest = guest_pointer(depth->va) + (size_t)y * depth->pitch;
            for (uint32_t x = 0; x < depth->guest_width; x++) {
                uint32_t source_x =
                    (uint32_t)(((uint64_t)x * 2u + 1u) * depth->width /
                               ((uint64_t)depth->guest_width * 2u));
                uint32_t packed;
                if (source_x >= depth->width)
                    source_x = depth->width - 1u;
                memcpy(&packed, guest + (size_t)x * 4u, sizeof packed);
                if (depth->initialized & 1u) {
                    float value = fminf(1.0f, fmaxf(0.0f, z[source_x]));
                    uint32_t z24 = (uint32_t)lroundf(value * 16777215.0f);
                    packed = (packed & 0xFFu) | (z24 << 8);
                }
                if (depth->initialized & 2u)
                    packed = (packed & 0xFFFFFF00u) | stencil[source_x];
                memcpy(guest + (size_t)x * 4u, &packed, sizeof packed);
            }
        }
        invalidate_texture_cache(depth->va, depth->end);
    }
    return 1;
}

static int seed_depth_surface(BWMetalDepthSurface *depth)
{
    unsigned missing = 3u & ~depth->initialized;
    MetalDepthTransfer layout;
    id<MTLBuffer> buffer;
    id<MTLCommandBuffer> command;
    id<MTLBlitCommandEncoder> blit;

    if (!missing)
        return 1;
    if (!depth_transfer_layout(depth, &layout))
        return 0;
    buffer = [s_device newBufferWithLength:layout.total options:MTLResourceStorageModeShared];
    if (!buffer || !buffer.contents)
        return metal_fail("could not allocate depth/stencil upload buffer");
    for (uint32_t y = 0; y < depth->height; y++) {
        uint32_t guest_y = (uint32_t)((uint64_t)y * depth->guest_height / depth->height);
        const uint8_t *guest =
            guest_pointer(depth->va) + (size_t)guest_y * depth->pitch;
        float *z = (float *)((uint8_t *)buffer.contents + (size_t)y * layout.depth_row);
        uint8_t *stencil = (uint8_t *)buffer.contents + layout.stencil_offset
                         + (size_t)y * layout.stencil_row;
        for (uint32_t x = 0; x < depth->width; x++) {
            uint32_t guest_x =
                (uint32_t)((uint64_t)x * depth->guest_width / depth->width);
            uint32_t packed;
            memcpy(&packed, guest + (size_t)guest_x * 4u, sizeof packed);
            if (missing & 1u)
                z[x] = (float)(packed >> 8) / 16777215.0f;
            if (missing & 2u)
                stencil[x] = (uint8_t)packed;
        }
    }
    end_draw_encoder();
    command = pending_command();
    blit = [command blitCommandEncoder];
    if (!command || !blit)
        return metal_fail("could not create depth/stencil upload command");
    MTLSize size = MTLSizeMake(depth->width, depth->height, 1);
    if (missing & 1u)
        [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:layout.depth_row
         sourceBytesPerImage:layout.depth_row * depth->height sourceSize:size
                  toTexture:depth->texture destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0) options:MTLBlitOptionDepthFromDepthStencil];
    if (missing & 2u)
        [blit copyFromBuffer:buffer sourceOffset:layout.stencil_offset
          sourceBytesPerRow:layout.stencil_row
        sourceBytesPerImage:layout.stencil_row * depth->height sourceSize:size
                  toTexture:depth->texture destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0) options:MTLBlitOptionStencilFromDepthStencil];
    [blit endEncoding];
    depth->initialized |= missing;
    return 1;
}

static id<MTLRenderPipelineState> partial_clear_pipeline(uint32_t color_flags, int has_depth)
{
    uint32_t key_value = (color_flags >> 4) | (has_depth ? 16u : 0u);
    NSNumber *key = @(key_value);
    id<MTLRenderPipelineState> state = s_clear_pipelines[key];
    if (state)
        return state;
    if (!s_clear_pipelines)
        s_clear_pipelines = [NSMutableDictionary new];
    if (!s_clear_pipelines)
        return metal_fail("could not create clear pipeline cache"), nil;
    uint64_t key_hash = s_pipeline_diagnostic.enabled
                      ? pipeline_key_hash(4, &key_value, sizeof key_value) : 0;
    MTLRenderPipelineDescriptor *pipeline = new_pipeline_descriptor();
    if (!pipeline) {
        (void)create_render_pipeline_state(
            nil, key_hash, s_frame, METAL_PIPELINE_ROLE_RENDER, NULL);
        return metal_fail("could not create clear pipeline descriptor"), nil;
    }
    pipeline.vertexFunction = s_clear_vertex_function;
    pipeline.fragmentFunction = color_flags ? s_clear_color_function : s_clear_depth_function;
    if (color_flags) {
        MTLRenderPipelineColorAttachmentDescriptor *attachment = pipeline.colorAttachments[0];
        attachment.pixelFormat = MTLPixelFormatBGRA8Unorm;
        attachment.writeMask = MTLColorWriteMaskNone;
        if (color_flags & 0x10u) attachment.writeMask |= MTLColorWriteMaskRed;
        if (color_flags & 0x20u) attachment.writeMask |= MTLColorWriteMaskGreen;
        if (color_flags & 0x40u) attachment.writeMask |= MTLColorWriteMaskBlue;
        if (color_flags & 0x80u) attachment.writeMask |= MTLColorWriteMaskAlpha;
    }
    if (has_depth) {
        pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pipeline.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
    NSError *error = nil;
    state = create_render_pipeline_state(
        pipeline, key_hash, s_frame, METAL_PIPELINE_ROLE_RENDER, &error);
    if (!state)
        return metal_fail("clear pipeline creation failed: %s", metal_error_text(error)), nil;
    s_clear_pipelines[key] = state;
    return state;
}

static id<MTLDepthStencilState> partial_clear_depth_state(uint32_t flags)
{
    id<MTLDepthStencilState> state = s_clear_depth_states[flags];
    if (state)
        return state;
    MTLDepthStencilDescriptor *desc = [MTLDepthStencilDescriptor new];
    if (!desc)
        return metal_fail("could not create clear depth/stencil descriptor"), nil;
    desc.depthCompareFunction = MTLCompareFunctionAlways;
    desc.depthWriteEnabled = (flags & 1u) != 0;
    if (flags & 2u) {
        MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
        if (!stencil)
            return metal_fail("could not create clear stencil descriptor"), nil;
        stencil.stencilCompareFunction = MTLCompareFunctionAlways;
        stencil.stencilFailureOperation = MTLStencilOperationKeep;
        stencil.depthFailureOperation = MTLStencilOperationKeep;
        stencil.depthStencilPassOperation = MTLStencilOperationReplace;
        stencil.readMask = 0xFFu;
        stencil.writeMask = 0xFFu;
        desc.frontFaceStencil = stencil;
        desc.backFaceStencil = stencil;
    }
    state = [s_device newDepthStencilStateWithDescriptor:desc];
    if (!state)
        return metal_fail("could not create clear depth/stencil state"), nil;
    s_clear_depth_states[flags] = state;
    return state;
}

static int compare_function(uint32_t value, MTLCompareFunction *result)
{
    switch (value) {
    case 0x200u: *result = MTLCompareFunctionNever; break;
    case 0x201u: *result = MTLCompareFunctionLess; break;
    case 0x202u: *result = MTLCompareFunctionEqual; break;
    case 0x203u: *result = MTLCompareFunctionLessEqual; break;
    case 0x204u: *result = MTLCompareFunctionGreater; break;
    case 0x205u: *result = MTLCompareFunctionNotEqual; break;
    case 0x206u: *result = MTLCompareFunctionGreaterEqual; break;
    case 0x207u: *result = MTLCompareFunctionAlways; break;
    default: return 0;
    }
    return 1;
}

static int stencil_operation(uint32_t value, MTLStencilOperation *result)
{
    switch (value) {
    case 0x1E00u: *result = MTLStencilOperationKeep; break;
    case 0: *result = MTLStencilOperationZero; break;
    case 0x1E01u: *result = MTLStencilOperationReplace; break;
    case 0x1E02u: *result = MTLStencilOperationIncrementClamp; break;
    case 0x1E03u: *result = MTLStencilOperationDecrementClamp; break;
    case 0x150Au: *result = MTLStencilOperationInvert; break;
    case 0x8507u: *result = MTLStencilOperationIncrementWrap; break;
    case 0x8508u: *result = MTLStencilOperationDecrementWrap; break;
    default: return 0;
    }
    return 1;
}

static int polygon_offset_state(float *factor, float *units)
{
    uint32_t factor_bits, units_bits;
    if (!bw_backend_reg(0x0338u))
        return 0;
    factor_bits = bw_backend_reg(0x0384u);
    units_bits = bw_backend_reg(0x0388u);
    memcpy(factor, &factor_bits, sizeof *factor);
    memcpy(units, &units_bits, sizeof *units);
    if (!isfinite(*factor) || !isfinite(*units))
        return -1;
    *units = roundf(*units);
    return *factor != 0.0f || *units != 0.0f;
}

static int validate_state(const Nv2aRenderState *rs, int extended)
{
    MTLCompareFunction comparison;
    MTLStencilOperation operation;
    float polygon_factor, polygon_units;
    if (!rs)
        return metal_fail("missing render state");
    if (rs->depth_test_enable && !compare_function(rs->depth_func, &comparison))
        return metal_fail("unsupported depth comparison 0x%08x", rs->depth_func);
    if (rs->stencil_enable &&
        (!compare_function(rs->stencil_func, &comparison) ||
         !stencil_operation(rs->stencil_fail, &operation) ||
         !stencil_operation(rs->stencil_zfail, &operation) ||
         !stencil_operation(rs->stencil_zpass, &operation)))
        return metal_fail("unsupported stencil comparison or operation");
    if (rs->cull_enable &&
        ((rs->cull_face != 0x404u && rs->cull_face != 0x405u && rs->cull_face != 0x408u) ||
         (rs->front_face != 0x900u && rs->front_face != 0x901u)))
        return metal_fail("unsupported cull face or winding");
    if (rs->alpha_test_enable && (rs->alpha_func < 0x200u || rs->alpha_func > 0x207u))
        return metal_fail("unsupported alpha comparison 0x%08x", rs->alpha_func);
    if (!extended && bw_backend_reg(0x02A4u))
        return metal_fail("unsupported fog state (enable %08x mode %08x color %08x "
                          "params %08x/%08x)",
                          bw_backend_reg(0x02A4u), bw_backend_reg(0x029Cu),
                          bw_backend_reg(0x02A8u), bw_backend_reg(0x09C0u),
                          bw_backend_reg(0x09C4u));
    if (polygon_offset_state(&polygon_factor, &polygon_units) < 0)
        return metal_fail("invalid polygon offset state (enable %08x scale %08x bias %08x)",
                          bw_backend_reg(0x0338u), bw_backend_reg(0x0384u),
                          bw_backend_reg(0x0388u));
    if (bw_backend_reg(0x17F8u))
        return metal_fail("unsupported clip plane state (mode %08x)",
                          bw_backend_reg(0x17F8u));
    if (!extended && (bw_backend_combiner_seen() || bw_backend_reg(0x1E70u)))
        return metal_fail("unsupported register-combiner state on basic draw "
                          "(seen %u program %08x)",
                          bw_backend_combiner_seen(), bw_backend_reg(0x1E70u));
    if (!isfinite(rs->depth_min) || !isfinite(rs->depth_max) ||
        rs->depth_max <= rs->depth_min || !isfinite(rs->depth_max - rs->depth_min))
        return metal_fail("invalid depth range");
    if (!extended)
        for (unsigned stage = 1; stage < 4; stage++) {
            uint32_t base = 0x1B00u + stage * 0x40u;
            if (bw_backend_reg(base) || bw_backend_reg(base + 4u) ||
                bw_backend_reg(base + 0x10u) || bw_backend_reg(base + 0x1Cu))
                return metal_fail("texture stages 1-3 are unsupported");
        }
    return 1;
}

static int blend_factor(uint32_t factor, int alpha, MTLBlendFactor *result)
{
    switch (factor) {
    case 0: *result = MTLBlendFactorZero; break;
    case 1: *result = MTLBlendFactorOne; break;
    case 0x300: *result = alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor; break;
    case 0x301: *result = alpha ? MTLBlendFactorOneMinusSourceAlpha
                                : MTLBlendFactorOneMinusSourceColor; break;
    case 0x302: *result = MTLBlendFactorSourceAlpha; break;
    case 0x303: *result = MTLBlendFactorOneMinusSourceAlpha; break;
    case 0x304: *result = MTLBlendFactorDestinationAlpha; break;
    case 0x305: *result = MTLBlendFactorOneMinusDestinationAlpha; break;
    case 0x306: *result = alpha ? MTLBlendFactorDestinationAlpha
                                : MTLBlendFactorDestinationColor; break;
    case 0x307: *result = alpha ? MTLBlendFactorOneMinusDestinationAlpha
                                : MTLBlendFactorOneMinusDestinationColor; break;
    case 0x308: *result = alpha ? MTLBlendFactorOne : MTLBlendFactorSourceAlphaSaturated; break;
    case 0x8001: *result = alpha ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor; break;
    case 0x8002: *result = alpha ? MTLBlendFactorOneMinusBlendAlpha
                                 : MTLBlendFactorOneMinusBlendColor; break;
    case 0x8003: *result = MTLBlendFactorBlendAlpha; break;
    case 0x8004: *result = MTLBlendFactorOneMinusBlendAlpha; break;
    default: return metal_fail("unsupported blend factor 0x%08x", factor);
    }
    return 1;
}

static int blend_operation(uint32_t equation, MTLBlendOperation *result)
{
    switch (equation) {
    /* The executor's unwritten equation is zero; the D3D11 path treats it as ADD. */
    case 0:
    case 0x8006: *result = MTLBlendOperationAdd; break;
    case 0x8007: *result = MTLBlendOperationMin; break;
    case 0x8008: *result = MTLBlendOperationMax; break;
    case 0x800A: *result = MTLBlendOperationSubtract; break;
    case 0x800B: *result = MTLBlendOperationReverseSubtract; break;
    default: return metal_fail("unsupported blend equation 0x%08x", equation);
    }
    return 1;
}

static id<MTLRenderPipelineState> draw_pipeline(int shader_kind, const Nv2aRenderState *rs,
                                                int has_depth,
                                                const MetalExtendedSpecialization *specialization)
{
    int depth_only = shader_kind == METAL_SHADER_DEPTH_ONLY;
    MetalPipelineKey key = {
        .textured = (uint32_t)shader_kind,
        .color_mask = depth_only ? 0u : rs->color_mask & 0x01010101u,
        .fixed_alpha_zero = !depth_only &&
                            (bw_backend_reg(0x0208u) & 0xFu) == METAL_COLOR_XRGB_ZERO,
        .blend_enable = !depth_only && rs->blend_enable != 0,
        .blend_src = !depth_only && rs->blend_enable ? rs->blend_src : 0,
        .blend_dst = !depth_only && rs->blend_enable ? rs->blend_dst : 0,
        .blend_eq = !depth_only && rs->blend_enable ? rs->blend_eq : 0,
        .depth_attachment = has_depth != 0,
    };
    if (specialization) {
        key.specialized = 1;
        memcpy(key.texture, specialization->texture, sizeof key.texture);
        memcpy(key.stages, specialization->stages, sizeof key.stages);
        memcpy(key.dot, specialization->dot, sizeof key.dot);
        memcpy(key.combiner, specialization->combiner, sizeof key.combiner);
        memcpy(key.final, specialization->final, sizeof key.final);
        memcpy(key.alpha, specialization->alpha, sizeof key.alpha);
    }
    MTLBlendFactor src_rgb, dst_rgb, src_alpha, dst_alpha;
    MTLBlendOperation operation;
    if (key.blend_enable) {
        if (key.blend_dst == 0x308u)
            return metal_fail("unsupported destination blend factor 0x%08x", key.blend_dst), nil;
        if (!blend_factor(key.blend_src, 0, &src_rgb) ||
            !blend_factor(key.blend_dst, 0, &dst_rgb) ||
            !blend_factor(key.blend_src, 1, &src_alpha) ||
            !blend_factor(key.blend_dst, 1, &dst_alpha) ||
            !blend_operation(key.blend_eq, &operation))
            return nil;
        if (key.fixed_alpha_zero) {
            if (key.blend_src == 0x304u) src_rgb = MTLBlendFactorZero;
            if (key.blend_src == 0x305u) src_rgb = MTLBlendFactorOne;
            if (key.blend_src == 0x308u) src_rgb = MTLBlendFactorSourceAlpha;
            if (key.blend_dst == 0x304u) dst_rgb = MTLBlendFactorZero;
            if (key.blend_dst == 0x305u) dst_rgb = MTLBlendFactorOne;
        }
    }
    uint64_t key_hash = s_pipeline_diagnostic.enabled
                      ? pipeline_key_hash(3, &key, sizeof key) : 0;
    NSData *cache_key = [NSData dataWithBytes:&key length:sizeof key];
    if (!cache_key)
        return metal_fail("could not create draw pipeline cache key"), nil;
    id<MTLRenderPipelineState> state = s_pipelines[cache_key];
    if (state) {
        pipeline_diagnostic_cache_hit();
        return state;
    }
    pipeline_diagnostic_cache_miss(key_hash);
    MTLRenderPipelineDescriptor *pipeline = new_pipeline_descriptor();
    if (!pipeline) {
        (void)create_render_pipeline_state(
            nil, key_hash, s_frame, METAL_PIPELINE_ROLE_RENDER, NULL);
        return metal_fail("could not create draw pipeline descriptor"), nil;
    }
    pipeline.vertexFunction = shader_kind >= METAL_SHADER_EXTENDED
                            ? s_extended_vertex_function : s_vertex_function;
    if (shader_kind == METAL_SHADER_EXTENDED && specialization) {
        MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
        if (!values) {
            pipeline_diagnostic_finish(
                METAL_PIPELINE_EVENT_FUNCTION_SPECIALIZATION, key_hash, s_frame,
                METAL_PIPELINE_ROLE_RENDER,
                METAL_PIPELINE_STATUS_ALLOCATION_FAILED,
                (MetalPipelineTimer){0,
                                     METAL_PIPELINE_TIMING_NOT_APPLICABLE});
            return metal_fail("could not create extended shader constants"), nil;
        }
        const uint32_t *constant_groups[] = {
            specialization->texture, specialization->stages, specialization->dot,
        };
        for (NSUInteger group = 0; group < 3; group++)
            for (NSUInteger component = 0; component < 4; component++)
                [values setConstantValue:&constant_groups[group][component]
                                    type:MTLDataTypeUInt
                                 atIndex:group * 4u + component];
        for (NSUInteger stage = 0; stage < 8; stage++)
            for (NSUInteger component = 0; component < 4; component++)
                [values setConstantValue:&specialization->combiner[stage][component]
                                    type:MTLDataTypeUInt
                                 atIndex:12u + stage * 4u + component];
        for (NSUInteger component = 0; component < 2; component++) {
            [values setConstantValue:&specialization->final[component]
                                type:MTLDataTypeUInt atIndex:44u + component];
            [values setConstantValue:&specialization->alpha[component]
                                type:MTLDataTypeUInt atIndex:46u + component];
        }
        NSError *function_error = nil;
        MetalPipelineTimer specialization_timer = pipeline_diagnostic_begin();
        pipeline.fragmentFunction =
            [s_shader_library newFunctionWithName:@"extended_specialized_fragment"
                                   constantValues:values
                                            error:&function_error];
        pipeline_diagnostic_finish(
            METAL_PIPELINE_EVENT_FUNCTION_SPECIALIZATION, key_hash, s_frame,
            METAL_PIPELINE_ROLE_RENDER,
            pipeline.fragmentFunction ? METAL_PIPELINE_STATUS_OK
                                      : METAL_PIPELINE_STATUS_METAL_FAILED,
            specialization_timer);
        if (!pipeline.fragmentFunction)
            return metal_fail("extended shader specialization failed: %s",
                              metal_error_text(function_error)), nil;
    } else {
        pipeline.fragmentFunction =
            shader_kind == METAL_SHADER_REALISTIC_WATER ? s_realistic_water_fragment_function
          : shader_kind == METAL_SHADER_EXTENDED ? s_extended_fragment_function
          : shader_kind == METAL_SHADER_DEPTH_ONLY ? nil
          : shader_kind == METAL_SHADER_TEXTURE ? s_texture_function : s_color_function;
    }
    if (!pipeline.fragmentFunction && !depth_only)
        return nil;
    MTLRenderPipelineColorAttachmentDescriptor *attachment = pipeline.colorAttachments[0];
    attachment.pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (key.depth_attachment) {
        pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pipeline.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
    attachment.writeMask = MTLColorWriteMaskNone;
    if (key.color_mask & 0x00010000u) attachment.writeMask |= MTLColorWriteMaskRed;
    if (key.color_mask & 0x00000100u) attachment.writeMask |= MTLColorWriteMaskGreen;
    if (key.color_mask & 0x00000001u) attachment.writeMask |= MTLColorWriteMaskBlue;
    if (key.color_mask & 0x01000000u) attachment.writeMask |= MTLColorWriteMaskAlpha;
    if (key.blend_enable || key.fixed_alpha_zero) {
        attachment.blendingEnabled = YES;
        attachment.sourceRGBBlendFactor = key.blend_enable ? src_rgb : MTLBlendFactorOne;
        attachment.destinationRGBBlendFactor = key.blend_enable ? dst_rgb : MTLBlendFactorZero;
        attachment.sourceAlphaBlendFactor = key.fixed_alpha_zero ? MTLBlendFactorZero : src_alpha;
        attachment.destinationAlphaBlendFactor = key.fixed_alpha_zero ? MTLBlendFactorZero : dst_alpha;
        attachment.rgbBlendOperation = key.blend_enable ? operation : MTLBlendOperationAdd;
        attachment.alphaBlendOperation = key.fixed_alpha_zero ? MTLBlendOperationAdd : operation;
    }
    NSError *error = nil;
    state = create_render_pipeline_state(
        pipeline, key_hash, s_frame, METAL_PIPELINE_ROLE_RENDER, &error);
    if (!state && shader_kind == METAL_SHADER_REALISTIC_WATER) {
        fprintf(stderr,
                "[GPU-METAL] realistic water pipeline creation failed: %s; "
                "using retail water\n", metal_error_text(error));
        return nil;
    }
    if (!state)
        return metal_fail("draw pipeline creation failed: %s", metal_error_text(error)), nil;
    if (s_pipelines.count >= METAL_MAX_PIPELINES) {
        pipeline_diagnostic_cache_reset(key_hash);
        [s_pipelines removeAllObjects];
    }
    s_pipelines[cache_key] = state;
    pipeline_diagnostic_cache_count(s_pipelines.count);
    return state;
}

static id<MTLDepthStencilState> draw_depth_state(const Nv2aRenderState *rs, int has_depth)
{
    MetalDepthStateKey value = {
        .depth_test = has_depth && rs->depth_test_enable,
        .depth_func = has_depth && rs->depth_test_enable ? rs->depth_func : 0x207u,
        .depth_write = has_depth && rs->depth_write,
        .stencil_enable = has_depth && rs->stencil_enable,
        .stencil_func = has_depth && rs->stencil_enable ? rs->stencil_func : 0x207u,
        .stencil_func_mask = has_depth && rs->stencil_enable ? rs->stencil_func_mask & 0xFFu : 0,
        .stencil_write_mask = has_depth && rs->stencil_enable ? rs->stencil_write_mask & 0xFFu : 0,
        .stencil_fail = has_depth && rs->stencil_enable ? rs->stencil_fail : 0x1E00u,
        .stencil_zfail = has_depth && rs->stencil_enable ? rs->stencil_zfail : 0x1E00u,
        .stencil_zpass = has_depth && rs->stencil_enable ? rs->stencil_zpass : 0x1E00u,
    };
    NSData *key = [NSData dataWithBytes:&value length:sizeof value];
    id<MTLDepthStencilState> state = s_depth_states[key];
    if (state)
        return state;
    MTLDepthStencilDescriptor *descriptor = [MTLDepthStencilDescriptor new];
    if (!descriptor)
        return metal_fail("could not create draw depth/stencil descriptor"), nil;
    MTLCompareFunction depth_function;
    if (!compare_function(value.depth_func, &depth_function))
        return metal_fail("unsupported depth comparison 0x%08x", value.depth_func), nil;
    descriptor.depthCompareFunction = depth_function;
    descriptor.depthWriteEnabled = value.depth_write != 0;
    if (value.stencil_enable) {
        MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
        MTLCompareFunction stencil_function;
        MTLStencilOperation fail, zfail, zpass;
        if (!stencil)
            return metal_fail("could not create draw stencil descriptor"), nil;
        if (!compare_function(value.stencil_func, &stencil_function) ||
            !stencil_operation(value.stencil_fail, &fail) ||
            !stencil_operation(value.stencil_zfail, &zfail) ||
            !stencil_operation(value.stencil_zpass, &zpass))
            return metal_fail("unsupported stencil comparison or operation"), nil;
        stencil.stencilCompareFunction = stencil_function;
        stencil.stencilFailureOperation = fail;
        stencil.depthFailureOperation = zfail;
        stencil.depthStencilPassOperation = zpass;
        stencil.readMask = value.stencil_func_mask;
        stencil.writeMask = value.stencil_write_mask;
        descriptor.frontFaceStencil = stencil;
        descriptor.backFaceStencil = stencil;
    }
    state = [s_device newDepthStencilStateWithDescriptor:descriptor];
    if (!state)
        return metal_fail("could not create draw depth/stencil state"), nil;
    if (!s_depth_states)
        s_depth_states = [NSMutableDictionary new];
    if (!s_depth_states)
        return metal_fail("could not create draw depth/stencil state cache"), nil;
    if (s_depth_states.count >= METAL_MAX_DEPTH_STATES)
        [s_depth_states removeAllObjects];
    s_depth_states[key] = state;
    return state;
}

static BWMetalDepthSurface *prepare_draw_depth(const Nv2aSurface *surface,
                                               const Nv2aRenderState *state)
{
    BWMetalDepthSurface *depth;
    if (!state->zeta_va)
        return nil;
    depth = prepare_depth_surface(surface, state);
    if (!depth || !seed_depth_surface(depth))
        return nil;
    return depth;
}

static void configure_draw_depth(MTLRenderPassDescriptor *pass,
                                 BWMetalDepthSurface *depth)
{
    if (!depth)
        return;
    pass.depthAttachment.texture = depth->texture;
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.texture = depth->texture;
    pass.stencilAttachment.loadAction = MTLLoadActionLoad;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
}

static id<MTLRenderCommandEncoder> draw_encoder(BWMetalDepthSurface *depth)
{
    id<MTLTexture> depth_texture = depth ? depth->texture : nil;
    if (s_draw_encoder && s_draw_color == s_surface && s_draw_depth == depth_texture)
        return s_draw_encoder;
    end_draw_encoder();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (!pass)
        return metal_fail("could not create draw pass descriptor"), nil;
    pass.colorAttachments[0].texture = s_surface;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    configure_draw_depth(pass, depth);
    id<MTLCommandBuffer> command = pending_command();
    if (command &&
        !mark_pending_command_reason(METAL_COMMAND_REASON_RENDER))
        return nil;
    s_draw_encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!command || !s_draw_encoder) {
        s_draw_encoder = nil;
        return metal_fail("could not create draw command"), nil;
    }
    s_draw_color = s_surface;
    s_draw_depth = depth_texture;
    s_draw_encoder_creations++;
    return s_draw_encoder;
}

static void water_reflect_reset_run(void)
{
    s_water_reflect_active = 0;
    s_water_reflect_primed = 0;
    s_water_reflect_view = BW_X_WATER_VIEW_UNKNOWN;
    s_water_reflect_source = nil;
}

static void water_reflect_fail(const char *reason)
{
    if (!s_water_reflect_single_failed)
        fprintf(stderr,
                "[GPU-METAL] distant-water-line fix unavailable: %s; "
                "using retail reflection depth\n", reason);
    s_water_reflect_single_failed = 1;
    water_reflect_reset_run();
}

static id<MTLDepthStencilState> water_reflect_depth_state(int write)
{
    id<MTLDepthStencilState> state =
        write ? s_water_reflect_prime_state : s_water_reflect_test_state;
    if (state)
        return state;
    MTLDepthStencilDescriptor *descriptor = [MTLDepthStencilDescriptor new];
    if (!descriptor)
        return nil;
    descriptor.depthCompareFunction = MTLCompareFunctionLessEqual;
    descriptor.depthWriteEnabled = write != 0;
    state = [s_device newDepthStencilStateWithDescriptor:descriptor];
    if (write)
        s_water_reflect_prime_state = state;
    else
        s_water_reflect_test_state = state;
    return state;
}

static int water_reflect_copy_depth(BWMetalDepthSurface *source)
{
#if defined(BW_METAL_TESTING)
    if (s_water_reflect_test_failure)
        return 0;
#endif
    if (!source || !source->texture)
        return 0;
    if (s_water_reflect_depth &&
        (s_water_reflect_depth->width != source->width ||
         s_water_reflect_depth->height != source->height ||
         s_water_reflect_depth->scale_epoch != source->scale_epoch))
        s_water_reflect_depth = nil;
    if (!s_water_reflect_depth) {
        MTLTextureDescriptor *descriptor =
            [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                             width:source->width
                                            height:source->height
                                         mipmapped:NO];
        if (!descriptor)
            return 0;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageRenderTarget;
        BWMetalDepthSurface *depth = [BWMetalDepthSurface new];
        if (!depth)
            return 0;
        depth->texture = [s_device newTextureWithDescriptor:descriptor];
        if (!depth->texture)
            return 0;
        depth->width = source->width;
        depth->height = source->height;
        depth->guest_width = source->guest_width;
        depth->guest_height = source->guest_height;
        depth->scale_epoch = source->scale_epoch;
        s_water_reflect_depth = depth;
    }
    end_draw_encoder();
    id<MTLCommandBuffer> command = pending_command();
    if (command &&
        !mark_pending_command_reason(METAL_COMMAND_REASON_DEPTH_COPY))
        return 0;
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (!command || !blit)
        return 0;
    [blit copyFromTexture:source->texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(source->width, source->height, 1)
                toTexture:s_water_reflect_depth->texture
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    s_water_reflect_source = source;
    s_water_reflect_copies++;
    return 1;
}

enum {
    WATER_REFLECT_RETAIL,
    WATER_REFLECT_FILTER,
    WATER_REFLECT_PRIME,
};

static int water_reflect_mode(const BwXBatch *batch, BWMetalDepthSurface *depth)
{
    int current = batch->water.version == BW_X_WATER_SEMANTIC_VERSION;
    int lit = current && batch->water.pass == BW_X_WATER_PASS_LIT;
    int reflection = current && batch->water.pass == BW_X_WATER_PASS_REFLECTION;
    int foam = current && batch->water.pass == BW_X_WATER_PASS_FOAM;

    if (!s_water_reflect_single_enabled || s_water_reflect_single_failed ||
        bw_ab_off("oceanreflect") || !depth) {
        water_reflect_reset_run();
        return WATER_REFLECT_RETAIL;
    }
    if (lit) {
        if (batch->water.view == BW_X_WATER_VIEW_UNKNOWN) {
            water_reflect_reset_run();
            return WATER_REFLECT_RETAIL;
        }
        if (!s_water_reflect_active ||
            s_water_reflect_source != depth ||
            s_water_reflect_view != batch->water.view) {
            water_reflect_reset_run();
            if (!water_reflect_copy_depth(depth)) {
                water_reflect_fail("private depth allocation or copy failed");
                return WATER_REFLECT_RETAIL;
            }
            s_water_reflect_active = 1;
            s_water_reflect_view = batch->water.view;
        }
        return WATER_REFLECT_PRIME;
    }
    if (reflection && s_water_reflect_active && s_water_reflect_primed &&
        s_water_reflect_source == depth && s_water_reflect_depth &&
        s_water_reflect_view == batch->water.view)
        return WATER_REFLECT_FILTER;
    if (foam && s_water_reflect_active &&
        s_water_reflect_source == depth &&
        s_water_reflect_view == batch->water.view) {
        s_water_reflect_primed = 0;
        return WATER_REFLECT_RETAIL;
    }
    water_reflect_reset_run();
    return WATER_REFLECT_RETAIL;
}

static int water_reflect_prime(id<MTLBuffer> vertices, NSUInteger vertex_offset,
                               const MetalExtendedUniforms *uniforms,
                               const Nv2aRenderState *state, uint32_t count)
{
    id<MTLRenderPipelineState> pipeline =
        draw_pipeline(METAL_SHADER_DEPTH_ONLY, state, 1, NULL);
    id<MTLDepthStencilState> depth_state = water_reflect_depth_state(1);
    id<MTLRenderCommandEncoder> encoder = draw_encoder(s_water_reflect_depth);
    float factor = 0.0f, units = 0.0f;
    int offset = polygon_offset_state(&factor, &units);
    if (!pipeline || !depth_state || !encoder || offset < 0)
        return 0;
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:vertices offset:vertex_offset atIndex:0];
    [encoder setVertexBytes:uniforms length:sizeof *uniforms atIndex:1];
    [encoder setDepthStencilState:depth_state];
    set_draw_state(encoder, state);
    [encoder setDepthBias:units + 64.0f slopeScale:factor + 2.0f clamp:0.0f];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:count];
    s_water_reflect_prime_draws++;
    s_water_reflect_primed = 1;
    return 1;
}

static id<MTLTexture> decode_texture(const Nv2aTexture *tex)
{
    uint32_t *pixels;
    size_t count;
    MTLTextureDescriptor *desc;
    id<MTLTexture> texture;

    uint32_t format = bw_backend_reg(0x1B04u);
    if (tex->color != 0x12u || tex->addr_u != 3 || tex->addr_v != 3 ||
        tex->levels != 1 || tex->filter != 0x01010000u || (format & 4u) ||
        ((format >> 16) & 0xFu) > 1u)
        return metal_fail("unsupported stage-0 texture format, addressing or filter"), nil;
    if (tex->width > METAL_MAX_DIMENSION || tex->height > METAL_MAX_DIMENSION)
        return metal_fail("stage-0 texture dimensions exceed %u", METAL_MAX_DIMENSION), nil;
    if (!span_valid(tex->offset, tex->width, tex->height, tex->pitch) ||
        (size_t)tex->width > SIZE_MAX / tex->height ||
        (size_t)tex->width * tex->height > SIZE_MAX / sizeof(uint32_t))
        return metal_fail("invalid stage-0 texture guest-memory span"), nil;
    if (depth_overlaps(tex->offset, span_end(tex->offset, tex->width, tex->height, tex->pitch)))
        return metal_fail("sampling a depth/stencil surface is unsupported"), nil;
    if (s_surface &&
        (uint64_t)tex->offset < span_end(s_current.color_va, s_current.width,
                                          s_current.height, s_current.pitch) &&
        (uint64_t)s_current.color_va < span_end(tex->offset, tex->width,
                                                tex->height, tex->pitch))
        return metal_fail(s_dirty ? "sampling a pending render target is unsupported"
                                  : "sampling the active colour surface is unsupported"), nil;
    if (!publish_overlapping_color_surfaces(
            tex->offset, span_end(tex->offset, tex->width,
                                  tex->height, tex->pitch)))
        return nil;
    count = (size_t)tex->width * tex->height;
    pixels = malloc(count * sizeof *pixels);
    if (!pixels)
        return metal_fail("could not allocate texture decode buffer"), nil;
    if (!nv2a_backend_decode_texture(tex, pixels)) {
        free(pixels);
        return metal_fail("stage-0 texture decoding failed"), nil;
    }
    desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:tex->width
                                                            height:tex->height
                                                         mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    texture = [s_device newTextureWithDescriptor:desc];
    if (!texture) {
        free(pixels);
        return metal_fail("could not create stage-0 texture"), nil;
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, tex->width, tex->height)
               mipmapLevel:0 withBytes:pixels bytesPerRow:(size_t)tex->width * 4u];
    free(pixels);
    return texture;
}

static int valid_combiner_input(uint32_t byte, int final)
{
    uint32_t reg = byte & 15u;
    return (reg <= 5u || (reg >= 8u && reg <= 13u) ||
            (final && (reg == 14u || reg == 15u)));
}

static int valid_combiner_word(uint32_t word, unsigned inputs, int final)
{
    for (unsigned i = 0; i < inputs; i++)
        if (!valid_combiner_input(word >> (24u - i * 8u), final))
            return 0;
    return 1;
}

static int valid_combiner_destinations(uint32_t word, int alpha)
{
    uint32_t dest[3] = {word & 15u, (word >> 4) & 15u, (word >> 8) & 15u};
    uint32_t flags = word >> 12, mapping = flags & 0x38u;
    if ((word & ~0xFFFFFu) || (alpha && (flags & 0xC3u)) ||
        mapping == 0x28u || mapping == 0x38u)
        return 0;
    for (unsigned i = 0; i < 3; i++) {
        if (dest[i] && dest[i] != 12u && dest[i] != 13u)
            return 0;
        for (unsigned j = 0; j < i; j++)
            if (dest[i] && dest[i] == dest[j])
                return 0;
    }
    return 1;
}

static int validate_extended_combiner(const BwXBatch *batch)
{
    const Nv2aCombiner *rc = batch->rc;
    uint32_t count = rc->control & 0xFFu;
    if (!count || count > 8u || (rc->control & ~0x111FFu))
        return metal_fail("unsupported register-combiner stage count or control");
    if (rc->stage_program & ~0xFFFFFu)
        return metal_fail("unsupported register-combiner texture-stage program %08x",
                          rc->stage_program);
    uint32_t stage_mode[4];
    for (unsigned i = 0; i < 4; i++) {
        uint32_t mode = (rc->stage_program >> (i * 5u)) & 0x1Fu;
        stage_mode[i] = mode;
        if (mode != 0u && mode != 1u && mode != 4u &&
            !(i == 1u && mode == 0x11u) &&
            !(i == 2u && mode == 0x0Bu) &&
            !(i == 3u && mode == 0x0Cu))
            return metal_fail("unsupported register-combiner texture-stage program %08x "
                              "(stage %u opcode %u; textures "
                              "0=%u/%u/%u/%x/%u/%ux%u "
                              "1=%u/%u/%u/%x/%u/%ux%u "
                              "2=%u/%u/%u/%x/%u/%ux%u "
                              "3=%u/%u/%u/%x/%u/%ux%u)",
                              rc->stage_program, i, mode,
                              batch->tex_valid[0], batch->tex_normalized[0],
                              batch->tex_cube[0], batch->tex[0].color,
                              batch->tex[0].levels, batch->tex[0].width,
                              batch->tex[0].height,
                              batch->tex_valid[1], batch->tex_normalized[1],
                              batch->tex_cube[1], batch->tex[1].color,
                              batch->tex[1].levels, batch->tex[1].width,
                              batch->tex[1].height,
                              batch->tex_valid[2], batch->tex_normalized[2],
                              batch->tex_cube[2], batch->tex[2].color,
                              batch->tex[2].levels, batch->tex[2].width,
                              batch->tex[2].height,
                              batch->tex_valid[3], batch->tex_normalized[3],
                              batch->tex_cube[3], batch->tex[3].color,
                              batch->tex[3].levels, batch->tex[3].width,
                              batch->tex[3].height);
    }
    if ((stage_mode[2] == 0x0Bu &&
         (stage_mode[1] != 0x11u || ((batch->other_stage_input >> 16) & 15u) >= 2u ||
          ((batch->other_stage_input >> 20) & 15u) >= 3u)) ||
        (stage_mode[3] == 0x0Cu &&
         (stage_mode[1] != 0x11u || ((batch->other_stage_input >> 20) & 15u) >= 3u)) ||
        ((stage_mode[1] == 0x11u || stage_mode[2] == 0x0Bu || stage_mode[3] == 0x0Cu) &&
         (((batch->dot_rgbmapping >> 0) & 15u) > 3u ||
          ((batch->dot_rgbmapping >> 4) & 15u) > 3u ||
          ((batch->dot_rgbmapping >> 8) & 15u) > 3u)))
        return metal_fail("unsupported register-combiner dot-product state");
    for (uint32_t i = 0; i < count; i++)
        if (!valid_combiner_word(rc->color_icw[i], 4, 0) ||
            !valid_combiner_word(rc->alpha_icw[i], 4, 0) ||
            !valid_combiner_destinations(rc->color_ocw[i], 0) ||
            !valid_combiner_destinations(rc->alpha_ocw[i], 1))
            return metal_fail("unsupported register-combiner general-stage inputs or outputs");
    if ((rc->final1 & 0x1Fu) ||
        !valid_combiner_word(rc->final0, 4, 1) || !valid_combiner_word(rc->final1, 3, 1) ||
        ((rc->final1 >> 24) & 15u) == 15u || ((rc->final1 >> 16) & 15u) == 15u)
        return metal_fail("unsupported register-combiner final-stage inputs or flags");
    return 1;
}

static BWMetalColorSurface *exact_feedback_surface(const Nv2aTexture *texture)
{
    for (BWMetalColorSurface *entry in s_color_surfaces.allValues)
        if (entry->texture && entry->sync_valid &&
            entry->format == METAL_COLOR_XRGB_ZERO &&
            entry->surface.color_va == texture->offset &&
            entry->surface.width == texture->width &&
            entry->surface.height == texture->height &&
            entry->surface.pitch == texture->pitch &&
            entry->surface.bytes_per_pixel == 4u &&
            entry->surface.aa_sx == 2u &&
            entry->surface.aa_sy == 1u)
            return entry;
    return nil;
}

static id<MTLTexture> quincunx_approx_texture(const Nv2aSurface *surface,
                                              const BwXBatch *batch)
{
    const Nv2aTexture *tex = &batch->tex[0];
    uint32_t format = bw_backend_reg(0x1B04u), address = bw_backend_reg(0x1B08u);
    uint32_t control = bw_backend_reg(0x1B0Cu), filter = tex->filter;
    if (batch->tex_normalized[0] || batch->tex_cube[0] ||
        tex->color != 0x1Eu || tex->levels != 1u || tex->addr_u != 3u || tex->addr_v != 3u ||
        ((format >> 4) & 15u) != 2u || (format & 4u) ||
        ((format >> 8) & 0xFFu) != 0x1Eu || ((format >> 16) & 15u) != 1u ||
        (address & ~0x000F0F0Fu) || (filter & 0xF0000000u) ||
        !(control & (1u << 30)) || (control & 0x8000003Fu) ||
        (bw_backend_reg(0x0208u) & 15u) != METAL_COLOR_XRGB_ZERO ||
        surface->aa_sx != 1u || surface->aa_sy != 1u)
        return metal_fail("unsupported quincunx approximation texture or surface state"), nil;
    if (batch->state->blend_enable)
        return metal_fail("blending with quincunx approximation is unsupported"), nil;
    if (!tex->width || !tex->height || tex->width > METAL_MAX_DIMENSION ||
        tex->height > METAL_MAX_DIMENSION || tex->width != surface->width * 2u ||
        tex->height != surface->height)
        return metal_fail("unsupported quincunx approximation texture dimensions"), nil;
    if (!span_valid(tex->offset, tex->width, tex->height, tex->pitch) ||
        (size_t)tex->width > SIZE_MAX / tex->height ||
        (size_t)tex->width * tex->height > SIZE_MAX / sizeof(uint32_t))
        return metal_fail("invalid quincunx approximation texture guest-memory span"), nil;
    uint64_t end = span_end(tex->offset, tex->width, tex->height, tex->pitch);
    if (depth_overlaps(tex->offset, end))
        return metal_fail("sampling a depth/stencil surface is unsupported"), nil;
    if ((uint64_t)tex->offset < span_end(surface->color_va, surface->width,
                                         surface->height, surface->pitch) &&
        (uint64_t)surface->color_va < end)
        return metal_fail("sampling the active colour surface is unsupported"), nil;
    if (s_dirty && (uint64_t)tex->offset < span_end(s_current.color_va, s_current.width,
                                                   s_current.height, s_current.pitch) &&
        (uint64_t)s_current.color_va < end)
        return metal_fail("sampling a pending render target is unsupported"), nil;
    uint64_t feedback_start = s_frame_timing_enabled ? xbox_HostNowNs() : 0;
    s_feedback_checks++;
    BWMetalColorSurface *feedback = exact_feedback_surface(tex);
    if (feedback &&
        surface_matches_guest_sync(feedback, &feedback->surface,
                                   feedback->format)) {
        if (!ensure_feedback_resolve(tex->width, tex->height) ||
            !mark_pending_command_reason(METAL_COMMAND_REASON_FEEDBACK) ||
            !encode_surface_copy(feedback->texture,
                                 s_encoding_submission->feedback_resolve, 1))
            return nil;
        id<MTLTexture> resolved = s_encoding_submission->feedback_resolve;
        uint64_t feedback_end = s_frame_timing_enabled ? xbox_HostNowNs() : 0;
        s_gpu_private_feedback_resolves++;
        if (feedback_start && feedback_end > feedback_start &&
            UINT64_MAX - s_feedback_publication_ns >=
                feedback_end - feedback_start)
            s_feedback_publication_ns += feedback_end - feedback_start;
        return resolved;
    }

    s_feedback_fallbacks++;
    uint64_t publications_before = s_guest_publications;
    int published = publish_overlapping_color_surfaces(tex->offset, end);
    uint64_t feedback_end = s_frame_timing_enabled ? xbox_HostNowNs() : 0;
    if (s_guest_publications >= publications_before &&
        UINT64_MAX - s_feedback_publications >=
            s_guest_publications - publications_before)
        s_feedback_publications += s_guest_publications - publications_before;
    if (feedback_start && feedback_end > feedback_start &&
        UINT64_MAX - s_feedback_publication_ns >= feedback_end - feedback_start)
        s_feedback_publication_ns += feedback_end - feedback_start;
    if (!published)
        return nil;

    uint64_t preparation_start = s_frame_timing_enabled ? xbox_HostNowNs() : 0;
    size_t count = (size_t)tex->width * tex->height;
    uint32_t *pixels = malloc(count * sizeof *pixels);
    if (!pixels)
        return metal_fail("could not allocate quincunx approximation texture"), nil;
    for (uint32_t y = 0; y < tex->height; y++) {
        uint32_t *row = pixels + (size_t)y * tex->width;
        memcpy(row, guest_pointer(tex->offset) + (size_t)y * tex->pitch,
               (size_t)tex->width * sizeof *row);
        for (uint32_t x = 0; x < tex->width; x++)
            row[x] |= 0xFF000000u;
    }
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:tex->width height:tex->height
                                                       mipmapped:NO];
    if (!desc) {
        free(pixels);
        return metal_fail("could not create quincunx approximation texture descriptor"), nil;
    }
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [s_device newTextureWithDescriptor:desc];
    if (!texture) {
        free(pixels);
        return metal_fail("could not create quincunx approximation texture"), nil;
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, tex->width, tex->height)
               mipmapLevel:0 withBytes:pixels bytesPerRow:(size_t)tex->width * sizeof *pixels];
    free(pixels);
    uint64_t preparation_end = s_frame_timing_enabled ? xbox_HostNowNs() : 0;
    s_quincunx_preparations++;
    if (UINT64_MAX - s_quincunx_preparation_bytes >= count * sizeof(uint32_t))
        s_quincunx_preparation_bytes += count * sizeof(uint32_t);
    if (preparation_start && preparation_end > preparation_start &&
        UINT64_MAX - s_quincunx_preparation_ns >=
            preparation_end - preparation_start)
        s_quincunx_preparation_ns += preparation_end - preparation_start;
    return texture;
}

static id<MTLTexture> extended_texture(const Nv2aSurface *surface, const BwXBatch *batch,
                                       unsigned stage, int cube, int *quincunx_approx)
{
    const Nv2aTexture *tex = &batch->tex[stage];
    uint32_t base = 0x1B00u + stage * 0x40u;
    uint32_t format = bw_backend_reg(base + 4u), address = bw_backend_reg(base + 8u);
    uint32_t control = bw_backend_reg(base + 12u);
    uint32_t filter = tex->filter, min = (filter >> 16) & 0xFFu, mag = (filter >> 24) & 15u;
    int compressed = tex->color == 0x0Cu || tex->color == 0x0Eu || tex->color == 0x0Fu;
    int swizzled = tex->color == 0x05u || tex->color == 0x06u || tex->color == 0x07u;
    int pitch_linear = tex->color == 0x12u || tex->color == 0x1Eu;
    *quincunx_approx = 0;
    if (!cube && batch->tex_valid[stage] && (min == 7u || mag == 4u)) {
        if (((filter >> 13) & 7u) == 1u) {
            if (stage == 0u && s_quincunx_approx && min == 7u && mag == 4u) {
                id<MTLTexture> texture = quincunx_approx_texture(surface, batch);
                if (texture)
                    *quincunx_approx = 1;
                return texture;
            }
            return metal_fail("unsupported extended texture convolution filter (quincunx)"), nil;
        }
        return metal_fail("unsupported extended texture convolution filter"), nil;
    }
    int filter_supported = min >= 1u && min <= 6u && mag >= 1u && mag <= 2u;
    if (!batch->tex_valid[stage] ||
        batch->tex_normalized[stage] == pitch_linear ||
        (cube && (!batch->tex_cube[stage] || !batch->tex_normalized[stage] ||
                  pitch_linear || tex->width != tex->height)) ||
        (!compressed && !swizzled && !pitch_linear) ||
        !tex->levels || tex->levels > 12u ||
        (pitch_linear && tex->levels != 1u) ||
        (tex->addr_u != 1u && tex->addr_u != 2u &&
         tex->addr_u != 3u && tex->addr_u != 5u) ||
        (tex->addr_v != 1u && tex->addr_v != 2u &&
         tex->addr_v != 3u && tex->addr_v != 5u) ||
        ((format >> 4) & 15u) != 2u || (cube && !(format & 4u)) ||
        ((format >> 8) & 0xFFu) != tex->color ||
        ((format >> 16) & 15u) != tex->levels ||
        (address & ~0x000F0F0Fu) || (address & 15u) != tex->addr_u ||
        ((address >> 8) & 15u) != tex->addr_v || (filter & 0xF0000000u) ||
        !filter_supported ||
        !(control & (1u << 30)) || (control & 0x8000003Fu))
        return metal_fail("unsupported extended texture format, sampler or layout "
                          "(stage %u valid %u normalized %u cube %u color %x levels %u "
                          "addr %u/%u format %08x address %08x filter %08x control %08x)",
                          stage, batch->tex_valid[stage], batch->tex_normalized[stage],
                          batch->tex_cube[stage], tex->color, tex->levels,
                          tex->addr_u, tex->addr_v, format, address, filter, control), nil;
    if (!tex->width || !tex->height ||
        (compressed && (tex->width < 4u || tex->height < 4u)) ||
        tex->width > METAL_MAX_DIMENSION || tex->height > METAL_MAX_DIMENSION ||
        (!pitch_linear &&
         ((tex->width & (tex->width - 1u)) || (tex->height & (tex->height - 1u)) ||
          tex->width != (1u << ((format >> 20) & 15u)) ||
          tex->height != (1u << ((format >> 24) & 15u)))) ||
        (pitch_linear && tex->pitch < tex->width * 4u))
        return metal_fail("unsupported extended texture dimensions"), nil;
    uint32_t texel_bytes = tex->color == 0x05u ? 2u : 4u;
    uint32_t block_bytes = tex->color == 0x0Cu ? 8u : 16u;
    uint32_t width = tex->width, height = tex->height;
    uint64_t bytes = 0;
    if (!pitch_linear) {
        for (uint32_t level = 0; level < tex->levels; level++) {
            bytes += compressed
                   ? (uint64_t)((width + 3u) / 4u) * ((height + 3u) / 4u) * block_bytes
                   : (uint64_t)width * height * texel_bytes;
            if (level + 1u < tex->levels && width == 1u && height == 1u)
                return metal_fail("unsupported extended texture mip count"), nil;
            width = width > 1u ? width / 2u : 1u;
            height = height > 1u ? height / 2u : 1u;
        }
    }
    uint64_t face_stride = cube ? (bytes + 127u) & ~UINT64_C(127) : bytes;
    uint64_t texture_bytes = cube ? face_stride * 6u : bytes;
    if (!pitch_linear && tex->levels > 1u && min >= 3u &&
        ((filter & 0x1FFFu) || ((control >> 18) & 0xFFFu) ||
         ((control >> 6) & 0xFFFu) < tex->levels - 1u))
        return metal_fail("unsupported extended texture mip LOD state"), nil;
    uint64_t end = pitch_linear
                 ? span_end(tex->offset, tex->width, tex->height, tex->pitch)
                 : (uint64_t)tex->offset + texture_bytes;
    uint32_t alignment = compressed ? 8u : texel_bytes;
    if ((tex->offset & (alignment - 1u)) ||
        (pitch_linear
             ? !span_valid(tex->offset, tex->width, tex->height, tex->pitch)
             : !byte_span_valid(tex->offset, texture_bytes)))
        return metal_fail("invalid extended texture guest-memory span"), nil;
    if (depth_overlaps(tex->offset, end))
        return metal_fail("sampling a depth/stencil surface is unsupported"), nil;
    if ((uint64_t)tex->offset < span_end(surface->color_va, surface->width,
                                         surface->height, surface->pitch) &&
        (uint64_t)surface->color_va < end)
        return metal_fail("sampling the active colour surface is unsupported"), nil;
    if (s_dirty && (uint64_t)tex->offset < span_end(s_current.color_va, s_current.width,
                                                   s_current.height, s_current.pitch) &&
        (uint64_t)s_current.color_va < end)
        return metal_fail("sampling a pending render target is unsupported"), nil;
    if (!publish_overlapping_color_surfaces(tex->offset, end))
        return nil;
    if (compressed && !s_device.supportsBCTextureCompression)
        return metal_fail("Metal device does not support BC texture compression"), nil;
    if (end < tex->offset || end - tex->offset > SIZE_MAX)
        return metal_fail("extended texture cache span overflows"), nil;
    MetalTextureKey key_value = {
        .offset = tex->offset,
        .width = tex->width,
        .height = tex->height,
        .pitch = pitch_linear ? tex->pitch : 0,
        .color = tex->color,
        .levels = tex->levels,
        .cube = cube,
    };
    NSData *cache_key = [NSData dataWithBytes:&key_value length:sizeof key_value];
    if (!cache_key)
        return metal_fail("could not create extended texture cache key"), nil;
    BWMetalTextureEntry *cache_entry = s_textures[cache_key];
    uint64_t content_hash = 0;
    if (texture_cache_hit(cache_entry, tex->offset, (size_t)(end - tex->offset),
                          &content_hash))
        return cache_entry->texture;
    MTLPixelFormat pixel_format = tex->color == 0x0Cu ? MTLPixelFormatBC1_RGBA
                                : tex->color == 0x0Eu ? MTLPixelFormatBC2_RGBA
                                : tex->color == 0x0Fu ? MTLPixelFormatBC3_RGBA
                                : tex->color == 0x05u ? MTLPixelFormatB5G6R5Unorm
                                                      : MTLPixelFormatBGRA8Unorm;
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                           width:tex->width height:tex->height
                                                       mipmapped:tex->levels > 1u];
    if (cube) {
        desc.textureType = MTLTextureType2DArray;
        desc.arrayLength = 6;
    }
    desc.mipmapLevelCount = tex->levels;
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [s_device newTextureWithDescriptor:desc];
    if (!texture)
        return metal_fail("could not create extended texture"), nil;
    uint8_t *decoded = NULL;
    if (swizzled || pitch_linear) {
        decoded = malloc((size_t)tex->width * tex->height * texel_bytes);
        if (!decoded)
            return metal_fail("could not allocate extended texture decode buffer"), nil;
    }
    width = tex->width;
    height = tex->height;
    const uint8_t *source = guest_pointer(tex->offset);
    if (pitch_linear) {
        for (uint32_t y = 0; y < tex->height; y++) {
            uint32_t *row = (uint32_t *)decoded + (size_t)y * tex->width;
            memcpy(row, source + (size_t)y * tex->pitch, (size_t)tex->width * 4u);
            if (tex->color == 0x1Eu)
                for (uint32_t x = 0; x < tex->width; x++)
                    row[x] |= 0xFF000000u;
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0 withBytes:decoded bytesPerRow:(size_t)width * 4u];
        free(decoded);
        s_texture_uploads++;
        if (!cache_texture(cache_key, cache_entry, texture, tex->offset,
                           end, content_hash))
            return nil;
        return texture;
    }
    for (uint32_t face = 0; face < (cube ? 6u : 1u); face++) {
        width = tex->width;
        height = tex->height;
        source = guest_pointer(tex->offset) + (size_t)face * face_stride;
        for (uint32_t level = 0; level < tex->levels; level++) {
            size_t row = compressed ? (size_t)((width + 3u) / 4u) * block_bytes
                                    : (size_t)width * texel_bytes;
            size_t rows = compressed ? (height + 3u) / 4u : height;
            const void *upload = source;
            if (swizzled) {
                xbox_unswizzle_rect(decoded, source, width, height, texel_bytes);
                if (tex->color == 0x07u)
                    for (size_t i = 0; i < (size_t)width * height; i++)
                        ((uint32_t *)decoded)[i] |= 0xFF000000u;
                upload = decoded;
            }
            if (cube)
                [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                           mipmapLevel:level slice:face withBytes:upload
                           bytesPerRow:row bytesPerImage:row * rows];
            else
                [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                           mipmapLevel:level withBytes:upload bytesPerRow:row];
            source += row * rows;
            width = width > 1u ? width / 2u : 1u;
            height = height > 1u ? height / 2u : 1u;
        }
    }
    free(decoded);
    s_texture_uploads++;
    if (!cache_texture(cache_key, cache_entry, texture, tex->offset,
                       end, content_hash))
        return nil;
    return texture;
}

static void metal_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                        uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    @autoreleasepool {
        uint32_t rect[4];
        uint32_t color_flags = flags & 0xF0u, depth_flags = flags & 3u;
        BWMetalDepthSurface *depth = nil;
        if (bw_metal_backend_error())
            return;
        water_reflect_reset_run();
        if (flags & ~0xF3u) {
            metal_fail("unsupported clear flags 0x%08x", flags);
            return;
        }
        if (depth_flags && !rs) {
            metal_fail("missing depth/stencil render state");
            return;
        }
        if (depth_flags && !rs->zeta_va)
            depth_flags = 0;
        if (!color_flags && !depth_flags) {
            if (flags & 3u)
                (void)validate_unmodified_surface(s);
            return;
        }
        if (!surface_valid(s))
            return;
        uint32_t rect_mask = bw_backend_clear_rect_mask();
        if (rect_mask && rect_mask != 3u) {
            metal_fail("incomplete clear rectangle is unsupported");
            return;
        }
        if (!bw_backend_clear_rect(rect)) {
            rect[0] = rect[1] = 0;
            rect[2] = s->width;
            rect[3] = s->height;
        } else {
            rect[0] *= s->aa_sx;
            rect[1] *= s->aa_sy;
            rect[2] *= s->aa_sx;
            rect[3] *= s->aa_sy;
        }
        if (rect[0] > s->width) rect[0] = s->width;
        if (rect[1] > s->height) rect[1] = s->height;
        if (rect[2] > s->width) rect[2] = s->width;
        if (rect[3] > s->height) rect[3] = s->height;
        if (!color_flags || rect[0] >= rect[2] || rect[1] >= rect[3]) {
            if (!validate_unmodified_surface(s))
                return;
        }
        if (rect[0] >= rect[2] || rect[1] >= rect[3])
            return;
        int full = !rect[0] && !rect[1] && rect[2] == s->width && rect[3] == s->height;
        int fast_clear = full && (!color_flags || color_flags == 0xF0u);
        if (color_flags && !prepare_surface(s))
            return;
        if (depth_flags) {
            depth = prepare_depth_surface(s, rs);
            if (!depth)
                return;
        }
        if (!fast_clear) {
            uint32_t target_width = color_flags ? (uint32_t)s_surface.width : depth->width;
            uint32_t target_height = color_flags ? (uint32_t)s_surface.height : depth->height;
            rect[0] = (uint32_t)((uint64_t)rect[0] * target_width / s->width);
            rect[1] = (uint32_t)((uint64_t)rect[1] * target_height / s->height);
            rect[2] = (uint32_t)(((uint64_t)rect[2] * target_width + s->width - 1u) /
                                 s->width);
            rect[3] = (uint32_t)(((uint64_t)rect[3] * target_height + s->height - 1u) /
                                 s->height);
        }
        id<MTLRenderPipelineState> pipeline = nil;
        id<MTLDepthStencilState> depth_state = nil;
        if (!fast_clear) {
            pipeline = partial_clear_pipeline(color_flags, depth != nil);
            if (!pipeline)
                return;
            if (depth) {
                depth_state = partial_clear_depth_state(depth_flags);
                if (!depth_state || !seed_depth_surface(depth))
                    return;
            }
        }
        end_draw_encoder();
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        if (!pass) {
            metal_fail("could not create clear pass descriptor");
            return;
        }
        if (color_flags) {
            uint32_t alpha = (bw_backend_reg(0x0208u) & 0xFu) == METAL_COLOR_XRGB_ZERO
                           ? 0u : argb >> 24;
            pass.colorAttachments[0].texture = s_surface;
            pass.colorAttachments[0].loadAction = fast_clear ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor =
                MTLClearColorMake(((argb >> 16) & 255u) / 255.0,
                                  ((argb >> 8) & 255u) / 255.0,
                                  (argb & 255u) / 255.0, alpha / 255.0);
        }
        if (depth) {
            pass.depthAttachment.texture = depth->texture;
            pass.depthAttachment.loadAction = (fast_clear && (depth_flags & 1u)) ? MTLLoadActionClear
                : (depth->initialized & 1u) ? MTLLoadActionLoad : MTLLoadActionDontCare;
            pass.depthAttachment.storeAction = ((depth_flags | depth->initialized) & 1u)
                ? MTLStoreActionStore : MTLStoreActionDontCare;
            pass.depthAttachment.clearDepth = (double)(zstencil >> 8) / 16777215.0;
            pass.stencilAttachment.texture = depth->texture;
            pass.stencilAttachment.loadAction = (fast_clear && (depth_flags & 2u)) ? MTLLoadActionClear
                : (depth->initialized & 2u) ? MTLLoadActionLoad : MTLLoadActionDontCare;
            pass.stencilAttachment.storeAction = ((depth_flags | depth->initialized) & 2u)
                ? MTLStoreActionStore : MTLStoreActionDontCare;
            pass.stencilAttachment.clearStencil = zstencil & 0xFFu;
        }
        id<MTLCommandBuffer> command = pending_command();
        if (command &&
            !mark_pending_command_reason(METAL_COMMAND_REASON_RENDER))
            return;
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!command || !encoder) {
            metal_fail("could not create clear command");
            return;
        }
        if (!fast_clear) {
            float z = (float)(zstencil >> 8) / 16777215.0f;
            uint32_t alpha = (bw_backend_reg(0x0208u) & 0xFu) == METAL_COLOR_XRGB_ZERO
                           ? 0u : argb >> 24;
            float color[4] = {((argb >> 16) & 255u) / 255.0f,
                              ((argb >> 8) & 255u) / 255.0f,
                              (argb & 255u) / 255.0f, alpha / 255.0f};
            [encoder setRenderPipelineState:pipeline];
            [encoder setScissorRect:(MTLScissorRect){rect[0], rect[1],
                                                     rect[2] - rect[0], rect[3] - rect[1]}];
            [encoder setVertexBytes:&z length:sizeof z atIndex:0];
            if (color_flags)
                [encoder setFragmentBytes:color length:sizeof color atIndex:0];
            if (depth) {
                [encoder setDepthStencilState:depth_state];
                [encoder setStencilReferenceValue:zstencil & 0xFFu];
            }
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        if (fast_clear && color_flags) {
            s_draw_encoder = encoder;
            s_draw_color = s_surface;
            s_draw_depth = depth ? depth->texture : nil;
            s_draw_encoder_creations++;
        } else {
            [encoder endEncoding];
        }
        if (color_flags)
            s_dirty = 1;
        if (depth)
            depth->initialized |= depth_flags;
    }
}

#if defined(BW_METAL_TESTING)
int bw_metal_test_read_depth_surface(uint32_t va, float *z, uint8_t *stencil, size_t count)
{
    @autoreleasepool {
        BWMetalDepthSurface *depth = s_depth_surfaces[@(va)];
        MetalDepthTransfer layout;
        id<MTLBuffer> buffer;
        id<MTLCommandBuffer> command;
        id<MTLBlitCommandEncoder> blit;
        if (!depth || depth->initialized != 3u || !z || !stencil ||
            count < (size_t)depth->width * depth->height)
            return 0;
        if (!flush_commands())
            return 0;
        if (!depth_transfer_layout(depth, &layout))
            return 0;
        buffer = [s_device newBufferWithLength:layout.total options:MTLResourceStorageModeShared];
        command = [s_queue commandBuffer];
        blit = [command blitCommandEncoder];
        if (!buffer || !buffer.contents || !command || !blit)
            return metal_fail("could not create depth/stencil test readback");
        MTLSize size = MTLSizeMake(depth->width, depth->height, 1);
        [blit copyFromTexture:depth->texture sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
                    toBuffer:buffer destinationOffset:0 destinationBytesPerRow:layout.depth_row
         destinationBytesPerImage:layout.depth_row * depth->height
                     options:MTLBlitOptionDepthFromDepthStencil];
        [blit copyFromTexture:depth->texture sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
                    toBuffer:buffer destinationOffset:layout.stencil_offset
      destinationBytesPerRow:layout.stencil_row
   destinationBytesPerImage:layout.stencil_row * depth->height
                     options:MTLBlitOptionStencilFromDepthStencil];
        [blit endEncoding];
        if (!finish_command(command))
            return 0;
        for (uint32_t y = 0; y < depth->height; y++) {
            memcpy(z + (size_t)y * depth->width,
                   (const uint8_t *)buffer.contents + (size_t)y * layout.depth_row,
                   (size_t)depth->width * sizeof *z);
            memcpy(stencil + (size_t)y * depth->width,
                   (const uint8_t *)buffer.contents + layout.stencil_offset
                       + (size_t)y * layout.stencil_row, depth->width);
        }
        return 1;
    }
}

int bw_metal_test_read_depth(uint32_t va, uint32_t x, uint32_t y,
                             float *z, uint8_t *stencil)
{
    @autoreleasepool {
        BWMetalDepthSurface *depth = s_depth_surfaces[@(va)];
        id<MTLBuffer> buffer;
        id<MTLCommandBuffer> command;
        id<MTLBlitCommandEncoder> blit;
        if (!depth || depth->initialized != 3u || x >= depth->width || y >= depth->height ||
            !z || !stencil)
            return 0;
        if (!flush_commands())
            return 0;
        buffer = [s_device newBufferWithLength:512 options:MTLResourceStorageModeShared];
        command = [s_queue commandBuffer];
        blit = [command blitCommandEncoder];
        if (!buffer || !command || !blit)
            return metal_fail("could not create depth/stencil test readback");
        [blit copyFromTexture:depth->texture sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(1, 1, 1)
                    toBuffer:buffer destinationOffset:0 destinationBytesPerRow:256
         destinationBytesPerImage:256 options:MTLBlitOptionDepthFromDepthStencil];
        [blit copyFromTexture:depth->texture sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(1, 1, 1)
                    toBuffer:buffer destinationOffset:256 destinationBytesPerRow:256
         destinationBytesPerImage:256 options:MTLBlitOptionStencilFromDepthStencil];
        [blit endEncoding];
        if (!finish_command(command))
            return 0;
        memcpy(z, buffer.contents, sizeof *z);
        memcpy(stencil, (const uint8_t *)buffer.contents + 256, sizeof *stencil);
        return 1;
    }
}
#endif

static void set_draw_state(id<MTLRenderCommandEncoder> encoder, const Nv2aRenderState *state)
{
    float polygon_factor, polygon_units;
    if (state->cull_enable) {
        [encoder setFrontFacingWinding:state->front_face == 0x900u
                                        ? MTLWindingClockwise : MTLWindingCounterClockwise];
        [encoder setCullMode:state->cull_face == 0x404u ? MTLCullModeFront : MTLCullModeBack];
    }
    if (state->blend_enable) {
        uint32_t color = state->blend_color;
        [encoder setBlendColorRed:((color >> 16) & 255u) / 255.0f
                            green:((color >> 8) & 255u) / 255.0f
                             blue:(color & 255u) / 255.0f
                            alpha:((color >> 24) & 255u) / 255.0f];
    }
    if (polygon_offset_state(&polygon_factor, &polygon_units) > 0)
        [encoder setDepthBias:polygon_units slopeScale:polygon_factor clamp:0.0f];
    else
        [encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
}

static void metal_draw(const Nv2aSurface *s, const Nv2aBatch *batch)
{
    @autoreleasepool {
        if (bw_metal_backend_error())
            return;
        water_reflect_reset_run();
        if (!batch || !batch->vertices || !batch->count || batch->count % 3u ||
            (size_t)batch->count > SIZE_MAX / sizeof(MetalVertex)) {
            metal_fail("invalid triangle batch");
            return;
        }
        if (!validate_state(batch->state, 0))
            return;
        if (!prepare_surface(s))
            return;
        BWMetalDepthSurface *depth = prepare_draw_depth(s, batch->state);
        if (batch->state->zeta_va && !depth)
            return;
        id<MTLRenderPipelineState> pipeline =
            draw_pipeline(batch->texture != NULL, batch->state, depth != nil, NULL);
        id<MTLDepthStencilState> depth_state =
            draw_depth_state(batch->state, depth != nil);
        if (!pipeline || !depth_state)
            return;
        if (!batch->texture && bw_backend_reg(0x1B00u)) {
            metal_fail("bound stage-0 texture was not supplied to the draw");
            return;
        }
        id<MTLTexture> texture = nil;
        if (batch->texture && !(texture = decode_texture(batch->texture)))
            return;
        id<MTLBuffer> vertices;
        NSUInteger vertex_offset;
        MetalVertex *out;
        if (!upload_vertices((size_t)batch->count * sizeof(MetalVertex),
                             &vertices, &vertex_offset, (void **)&out))
            return;
        for (uint32_t i = 0; i < batch->count; i++) {
            const Nv2aVertex *v = &batch->vertices[i];
            if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z) ||
                !isfinite(v->rhw) || v->rhw <= 0.0f ||
                !isfinite(v->u) || !isfinite(v->v)) {
                metal_fail("invalid vertex coordinates");
                return;
            }
            float w = 1.0f / v->rhw;
            float x = (2.0f * v->x / s->width - 1.0f) * w;
            float y = (1.0f - 2.0f * v->y / s->height) * w;
            float z = ((v->z - batch->state->depth_min) /
                       (batch->state->depth_max - batch->state->depth_min)) * w;
            if (!isfinite(w) || !isfinite(x) || !isfinite(y) || !isfinite(z)) {
                metal_fail("vertex projection overflows");
                return;
            }
            out[i] = (MetalVertex){
                .position = {v->x, v->y, v->z, v->rhw},
                .color = {((v->diffuse >> 16) & 255u) / 255.0f,
                          ((v->diffuse >> 8) & 255u) / 255.0f,
                          (v->diffuse & 255u) / 255.0f,
                          (v->diffuse >> 24) / 255.0f},
                .texcoord = {v->u, v->v, 0.0f, 0.0f},
            };
        }
        MetalUniforms uniforms = {
            .surface_texture = {(float)s->width, (float)s->height,
                                texture ? (float)batch->texture->width : 1.0f,
                                texture ? (float)batch->texture->height : 1.0f},
            .depth = {batch->state->depth_min, batch->state->depth_max, 0.0f, 0.0f},
            .alpha = {batch->state->alpha_test_enable
                          ? (float)(batch->state->alpha_func - 0x200u) : 7.0f,
                      (float)(batch->state->alpha_ref & 0xFFu) / 255.0f,
                      batch->state->alpha_test_enable ? 1.0f : 0.0f, 0.0f},
        };
        if (batch->state->cull_enable && batch->state->cull_face == 0x408u)
            return;
        id<MTLRenderCommandEncoder> encoder = draw_encoder(depth);
        if (!encoder)
            return;
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vertices offset:vertex_offset atIndex:0];
        [encoder setVertexBytes:&uniforms length:sizeof uniforms atIndex:1];
        [encoder setFragmentBytes:&uniforms length:sizeof uniforms atIndex:1];
        [encoder setDepthStencilState:depth_state];
        [encoder setStencilReferenceValue:batch->state->stencil_ref & 0xFFu];
        set_draw_state(encoder, batch->state);
        if (texture) {
            [encoder setFragmentTexture:texture atIndex:0];
            [encoder setFragmentSamplerState:s_sampler atIndex:0];
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:batch->count];
        s_dirty = 1;
    }
}

static id<MTLTexture> inactive_texture(void)
{
    if (!s_inactive_texture) {
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                              width:1 height:1 mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageShaderRead;
        s_inactive_texture = [s_device newTextureWithDescriptor:desc];
        if (!s_inactive_texture)
            return metal_fail("could not create inactive texture binding"), nil;
        const uint8_t pixel[4] = {0, 0, 0, 255};
        [s_inactive_texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                             mipmapLevel:0 withBytes:pixel bytesPerRow:sizeof pixel];
    }
    return s_inactive_texture;
}

static id<MTLTexture> inactive_cube_texture(void)
{
    if (!s_inactive_cube_texture) {
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                              width:1 height:1 mipmapped:NO];
        desc.textureType = MTLTextureType2DArray;
        desc.arrayLength = 6;
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageShaderRead;
        s_inactive_cube_texture = [s_device newTextureWithDescriptor:desc];
        if (!s_inactive_cube_texture)
            return metal_fail("could not create inactive cube texture binding"), nil;
        const uint8_t pixel[4] = {0, 0, 0, 255};
        for (NSUInteger slice = 0; slice < 6; slice++)
            [s_inactive_cube_texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                                       mipmapLevel:0 slice:slice withBytes:pixel
                                       bytesPerRow:sizeof pixel bytesPerImage:sizeof pixel];
    }
    return s_inactive_cube_texture;
}

static int lit_water_batch(const BwXBatch *batch)
{
    if (batch->water.version)
        return batch->water.version == BW_X_WATER_SEMANTIC_VERSION &&
               batch->water.pass == BW_X_WATER_PASS_LIT;
    return batch->draw_marker == BW_X_DRAW_MARKER_OCEAN;
}

/* The marked lit-water path binds its sampled water/reflection input at stage
 * 0 and carries the encoded normal and eye basis through stages 1 and 2. */
static int realistic_water_supported(const BwXBatch *batch, uint32_t stage_mask,
                                     uint32_t cube_mask)
{
    uint32_t program = batch->rc->stage_program;
    (void)cube_mask;
    return lit_water_batch(batch) &&
           (stage_mask & 1u) &&
           (program & 31u) == 1u &&
           ((program >> 5) & 31u) == 4u &&
           ((program >> 10) & 31u) == 4u &&
           ((program >> 15) & 31u) == 4u;
}

static void metal_draw_extended(const Nv2aSurface *surface, const BwXBatch *batch)
{
    @autoreleasepool {
        _Static_assert(sizeof(BwXVertex) == 32u * sizeof(float), "extended vertex layout");
        if (bw_metal_backend_error())
            return;
        if (!surface || !batch || !batch->state || !batch->rc || !batch->vertices ||
            !batch->count || batch->count % 3u ||
            (size_t)batch->count > SIZE_MAX / sizeof(BwXVertex)) {
            metal_fail("invalid extended triangle batch");
            return;
        }
        if (!validate_state(batch->state, 1) || !validate_extended_combiner(batch))
            return;
        if (!surface_valid(surface))
            return;
        uint32_t stage_mask = 0, cube_mask = 0;
        uint32_t unnormalized_mask = 0, passthrough_mask = 0;
        for (unsigned stage = 0; stage < 4; stage++) {
            uint32_t mode = (batch->rc->stage_program >> (stage * 5u)) & 0x1Fu;
            if (mode == 1u && batch->tex_valid[stage]) {
                stage_mask |= 1u << stage;
                if (!batch->tex_normalized[stage])
                    unnormalized_mask |= 1u << stage;
            } else if (((stage == 2u && mode == 0x0Bu) ||
                        (stage == 3u && mode == 0x0Cu)) &&
                       batch->tex_valid[stage] && batch->tex_cube[stage])
                cube_mask |= 1u << stage;
            else if (mode == 4u)
                passthrough_mask |= 1u << stage;
        }
        int supported_water =
            realistic_water_supported(batch, stage_mask, cube_mask);
        int realistic_water =
            s_active_water_preset == BW_METAL_WATER_REALISTIC && supported_water;
        if (s_active_water_preset == BW_METAL_WATER_REALISTIC &&
            lit_water_batch(batch)) {
            if (!supported_water) {
                s_realistic_water_fallback_batches++;
                if (!s_realistic_water_fallback_reported) {
                    uint32_t program = batch->rc->stage_program;
                    fprintf(stderr,
                            "[GPU-METAL] realistic water encountered an unsupported "
                            "marked pass (stages %u/%u/%u/%u, 2D mask %X, cube mask %X); "
                            "using the retail shader for that pass\n",
                            program & 0x1Fu, (program >> 5) & 0x1Fu,
                            (program >> 10) & 0x1Fu, (program >> 15) & 0x1Fu,
                            stage_mask, cube_mask);
                    s_realistic_water_fallback_reported = 1;
                }
            }
        }
        for (uint32_t i = 0; i < batch->count; i++) {
            const BwXVertex *v = &batch->vertices[i];
            if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z) ||
                !isfinite(v->rhw) || v->rhw <= 0.0f) {
                metal_fail("invalid extended vertex coordinates or projective divisor "
                           "(vertex %u position %g/%g/%g rhw %g)",
                           i, v->x, v->y, v->z, v->rhw);
                return;
            }
            for (unsigned stage = 0; stage < 4; stage++) {
                if (!(stage_mask & (1u << stage)))
                    continue;
                float q = v->tc[stage][3] != 0.0f ? v->tc[stage][3] : 1.0f;
                if (!isfinite(v->tc[stage][3]) ||
                    (isfinite(v->tc[stage][0]) &&
                     !isfinite(v->tc[stage][0] / q)) ||
                    (isfinite(v->tc[stage][1]) &&
                     !isfinite(v->tc[stage][1] / q))) {
                    metal_fail("invalid extended vertex coordinates or projective divisor "
                               "(vertex %u stage %u valid %u normalized %u color %x "
                               "offset %08x tc %g/%g/%g/%g effective q %g)",
                               i, stage, batch->tex_valid[stage],
                               batch->tex_normalized[stage],
                               batch->tex[stage].color, batch->tex[stage].offset,
                               v->tc[stage][0], v->tc[stage][1],
                               v->tc[stage][2], v->tc[stage][3], q);
                    return;
                }
            }
            for (unsigned j = 0; j < 4; j++)
                if (!isfinite(v->d0[j]) || !isfinite(v->d1[j])) {
                    metal_fail("invalid extended vertex colour");
                    return;
                }
            float w = 1.0f / v->rhw;
            float x = (2.0f * v->x / surface->width - 1.0f) * w;
            float y = (1.0f - 2.0f * v->y / surface->height) * w;
            float z = ((v->z - batch->state->depth_min) /
                       (batch->state->depth_max - batch->state->depth_min)) * w;
            if (!isfinite(w) || w < FLT_MIN || !isfinite(x) || !isfinite(y) || !isfinite(z)) {
                metal_fail("extended vertex projection overflows "
                           "(vertex %u projected %g/%g/%g/%g)", i, x, y, z, w);
                return;
            }
        }
        if (batch->state->cull_enable && batch->state->cull_face == 0x408u)
            return;
        if (!prepare_surface(surface))
            return;
        BWMetalDepthSurface *depth = prepare_draw_depth(surface, batch->state);
        if (batch->state->zeta_va && !depth)
            return;
        int reflect_mode = water_reflect_mode(batch, depth);
        BWMetalDepthSurface *draw_depth =
            reflect_mode == WATER_REFLECT_FILTER ? s_water_reflect_depth : depth;
        uint32_t filter0 = batch->tex[0].filter;
        int quincunx_approx =
            (stage_mask & 1u) && s_quincunx_approx &&
            ((filter0 >> 16) & 0xFFu) == 7u &&
            ((filter0 >> 24) & 0xFu) == 4u &&
            ((filter0 >> 13) & 7u) == 1u;
        MetalExtendedSpecialization specialization = {
            .texture = {
                (uint32_t)quincunx_approx, batch->rc->control & 0xFFu,
                (batch->rc->control >> 8) & 1u, batch->rc->control >> 8,
            },
            .stages = {
                stage_mask, unnormalized_mask, passthrough_mask, cube_mask,
            },
            .dot = {
                batch->rc->stage_program, batch->dot_rgbmapping,
                batch->other_stage_input, cube_mask,
            },
            .final = {batch->rc->final0, batch->rc->final1},
            .alpha = {
                batch->state->alpha_test_enable
                    ? batch->state->alpha_func - 0x200u : 7u,
                batch->state->alpha_test_enable != 0,
            },
        };
        for (uint32_t i = 0; i < (batch->rc->control & 0xFFu); i++) {
            specialization.combiner[i][0] = batch->rc->color_icw[i];
            specialization.combiner[i][1] = batch->rc->alpha_icw[i];
            specialization.combiner[i][2] = batch->rc->color_ocw[i];
            specialization.combiner[i][3] = batch->rc->alpha_ocw[i];
        }
        id<MTLRenderPipelineState> pipeline =
            draw_pipeline(realistic_water ? METAL_SHADER_REALISTIC_WATER
                                          : METAL_SHADER_EXTENDED,
                          batch->state, draw_depth != nil,
                          realistic_water ? NULL : &specialization);
        if (!pipeline && realistic_water && !bw_metal_backend_error()) {
            realistic_water = 0;
            s_active_water_preset = BW_METAL_WATER_RETAIL;
            atomic_store_explicit(&s_requested_water_preset,
                                  BW_METAL_WATER_RETAIL, memory_order_release);
            s_realistic_water_fallback_batches++;
            pipeline = draw_pipeline(METAL_SHADER_EXTENDED,
                                     batch->state, draw_depth != nil,
                                     &specialization);
        }
        id<MTLDepthStencilState> depth_state = reflect_mode == WATER_REFLECT_FILTER
            ? water_reflect_depth_state(0)
            : draw_depth_state(batch->state, draw_depth != nil);
        if (reflect_mode == WATER_REFLECT_FILTER && !depth_state) {
            water_reflect_fail("private reflection depth state creation failed");
            reflect_mode = WATER_REFLECT_RETAIL;
            draw_depth = depth;
            depth_state = draw_depth_state(batch->state, depth != nil);
        }
        if (!pipeline || !depth_state)
            return;
        if (realistic_water) {
            s_realistic_water_batches++;
            if (!s_realistic_water_reported) {
                fprintf(stderr,
                        "[GPU-METAL] optional realistic water shader active for "
                        "explicitly marked ocean passes\n");
                s_realistic_water_reported = 1;
            }
        }
        id<MTLTexture> textures[4] = {nil, nil, nil, nil};
        id<MTLTexture> cube_textures[4] = {nil, nil, nil, nil};
        id<MTLTexture> fallback = nil;
        id<MTLTexture> cube_fallback = nil;
        for (unsigned stage = 0; stage < 4; stage++) {
            if (stage_mask & (1u << stage)) {
                int approximation;
                textures[stage] = extended_texture(surface, batch, stage, 0, &approximation);
                if (!textures[stage])
                    return;
                if (!fallback)
                    fallback = textures[stage];
                if (approximation)
                    quincunx_approx = 1;
            }
            if (cube_mask & (1u << stage)) {
                int approximation;
                cube_textures[stage] =
                    extended_texture(surface, batch, stage, 1, &approximation);
                if (!cube_textures[stage])
                    return;
                if (!cube_fallback)
                    cube_fallback = cube_textures[stage];
            }
        }
        if (!fallback && !(fallback = inactive_texture()))
            return;
        if (!cube_fallback && !(cube_fallback = inactive_cube_texture()))
            return;
        for (unsigned stage = 0; stage < 4; stage++)
            if (!(stage_mask & (1u << stage)))
                textures[stage] = fallback;
        for (unsigned stage = 0; stage < 4; stage++)
            if (!(cube_mask & (1u << stage)))
                cube_textures[stage] = cube_fallback;
        size_t vertex_bytes = (size_t)batch->count * sizeof(BwXVertex);
        id<MTLBuffer> vertices;
        NSUInteger vertex_offset;
        void *vertex_contents;
        if (!upload_vertices(vertex_bytes, &vertices, &vertex_offset, &vertex_contents))
            return;
        memcpy(vertex_contents, batch->vertices, vertex_bytes);
        MetalExtendedUniforms uniforms = {
            .draw = {
                .surface_texture = {(float)surface->width, (float)surface->height, 1.0f, 1.0f},
                .depth = {batch->state->depth_min, batch->state->depth_max, 0.0f, 0.0f},
                .alpha = {batch->state->alpha_test_enable
                              ? (float)(batch->state->alpha_func - 0x200u) : 7.0f,
                          (float)(batch->state->alpha_ref & 0xFFu) / 255.0f,
                          batch->state->alpha_test_enable ? 1.0f : 0.0f, 0.0f},
            },
            .fog_color = {batch->fogc[0], batch->fogc[1], batch->fogc[2], 0.0f},
            .final = {batch->rc->final0, batch->rc->final1,
                      batch->rc->final_c0, batch->rc->final_c1},
            .texture = {(uint32_t)quincunx_approx, batch->rc->control & 0xFFu,
                        (batch->rc->control >> 8) & 1u, batch->rc->control >> 8},
            .stages = {stage_mask, unnormalized_mask, passthrough_mask, cube_mask},
            .dot = {batch->rc->stage_program, batch->dot_rgbmapping,
                    batch->other_stage_input, cube_mask},
        };
        for (uint32_t i = 0; i < (batch->rc->control & 0xFFu); i++) {
            uniforms.combiner[i][0] = batch->rc->color_icw[i];
            uniforms.combiner[i][1] = batch->rc->alpha_icw[i];
            uniforms.combiner[i][2] = batch->rc->color_ocw[i];
            uniforms.combiner[i][3] = batch->rc->alpha_ocw[i];
            uniforms.factors[i][0] = batch->rc->factor0[i];
            uniforms.factors[i][1] = batch->rc->factor1[i];
        }
        id<MTLRenderCommandEncoder> encoder = draw_encoder(draw_depth);
        if (!encoder)
            return;
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vertices offset:vertex_offset atIndex:0];
        [encoder setVertexBytes:&uniforms length:sizeof uniforms atIndex:1];
        [encoder setFragmentBytes:&uniforms length:sizeof uniforms atIndex:1];
        for (unsigned stage = 0; stage < 4; stage++) {
            [encoder setFragmentTexture:textures[stage] atIndex:stage];
            [encoder setFragmentTexture:cube_textures[stage] atIndex:4u + stage];
            uint32_t min = (batch->tex[stage].filter >> 16) & 0xFFu;
            uint32_t mag = (batch->tex[stage].filter >> 24) & 0xFu;
            uint32_t texture_mask = stage_mask | cube_mask;
            int min_linear = (texture_mask & (1u << stage)) &&
                             (min == 2u || min == 4u || min == 6u);
            int mag_linear = (texture_mask & (1u << stage)) && mag == 2u;
            unsigned mip_mode = 0;
            if ((texture_mask & (1u << stage)) &&
                batch->tex[stage].levels > 1u && min >= 3u)
                mip_mode = min <= 4u ? 1u : 2u;
            unsigned u = (texture_mask & (1u << stage)) && batch->tex[stage].addr_u <= 2u
                       ? batch->tex[stage].addr_u - 1u : 2u;
            unsigned v = (texture_mask & (1u << stage)) && batch->tex[stage].addr_v <= 2u
                       ? batch->tex[stage].addr_v - 1u : 2u;
            unsigned aniso = anisotropy_index(sampler_anisotropy(min_linear, mip_mode));
            [encoder setFragmentSamplerState:
                s_samplers[aniso][u][v][min_linear][mag_linear][mip_mode] atIndex:stage];
        }
        [encoder setDepthStencilState:depth_state];
        [encoder setStencilReferenceValue:batch->state->stencil_ref & 0xFFu];
        set_draw_state(encoder, batch->state);
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:batch->count];
        if (reflect_mode == WATER_REFLECT_FILTER) {
            s_water_reflect_filtered_draws++;
            if (!s_water_reflect_single_reported) {
                fprintf(stderr,
                        "[GPU-METAL] distant-water-line private reflection depth active\n");
                s_water_reflect_single_reported = 1;
            }
        } else if (reflect_mode == WATER_REFLECT_PRIME &&
                   !water_reflect_prime(vertices, vertex_offset, &uniforms,
                                        batch->state, batch->count)) {
            water_reflect_fail("private depth priming failed");
        }
        if (supported_water) {
            if (!s_frame_supported_water_batches && !water_capture_arm_frame())
                return;
            s_frame_supported_water_batches++;
            s_pending_supported_water = 1;
        }
        s_dirty = 1;
        if (quincunx_approx && !s_quincunx_reported) {
            fprintf(stderr, "[GPU-METAL] quincunx approximation used: legacy Gaussian; "
                            "not verified NV2A filtering\n");
            s_quincunx_reported = 1;
        }
    }
}

#if defined(BW_METAL_TESTING)
void bw_metal_test_draw_extended(const Nv2aSurface *surface, const BwXBatch *batch)
{
    metal_draw_extended(surface, batch);
}

size_t bw_metal_test_color_surface_count(void)
{
    return s_color_surfaces.count;
}

uint64_t bw_metal_test_command_submission_count(void)
{
    return s_command_submissions;
}

uint64_t bw_metal_test_draw_encoder_creation_count(void)
{
    return s_draw_encoder_creations;
}

uint64_t bw_metal_test_vertex_upload_allocation_count(void)
{
    return s_vertex_upload_allocations;
}

uint64_t bw_metal_test_texture_upload_count(void)
{
    return s_texture_uploads;
}

int bw_metal_test_current_surface(uint32_t *width, uint32_t *height,
                                  int *private_storage)
{
    if (!s_surface)
        return 0;
    if (width) *width = (uint32_t)s_surface.width;
    if (height) *height = (uint32_t)s_surface.height;
    if (private_storage)
        *private_storage = s_surface.storageMode == MTLStorageModePrivate;
    return 1;
}

int bw_metal_test_current_depth(uint32_t va, uint32_t *width, uint32_t *height,
                                int *private_storage)
{
    BWMetalDepthSurface *depth = s_depth_surfaces[@(va)];
    if (!depth)
        return 0;
    if (width) *width = depth->width;
    if (height) *height = depth->height;
    if (private_storage)
        *private_storage = depth->texture.storageMode == MTLStorageModePrivate;
    return 1;
}

int bw_metal_test_water_reflect_depth(uint32_t *width, uint32_t *height,
                                      uint32_t *scale_epoch)
{
    if (!s_water_reflect_depth)
        return 0;
    if (width)
        *width = s_water_reflect_depth->width;
    if (height)
        *height = s_water_reflect_depth->height;
    if (scale_epoch)
        *scale_epoch = s_water_reflect_depth->scale_epoch;
    return 1;
}

void bw_metal_test_set_present_size(uint32_t width, uint32_t height)
{
    atomic_store_explicit(&s_present_width, width, memory_order_release);
    atomic_store_explicit(&s_present_height, height, memory_order_release);
}

int bw_metal_test_write_surface_pixel(uint32_t x, uint32_t y, uint32_t argb)
{
    @autoreleasepool {
        if (!s_surface || x >= s_surface.width || y >= s_surface.height)
            return 0;
        if (!flush_commands())
            return 0;
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s_surface.pixelFormat
                                                               width:1 height:1 mipmapped:NO];
        if (!desc)
            return metal_fail("could not create test pixel staging descriptor");
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> staging = [s_device newTextureWithDescriptor:desc];
        id<MTLCommandBuffer> command = [s_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!staging || !command || !blit)
            return metal_fail("could not create test pixel staging resources");
        [staging replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                   mipmapLevel:0 withBytes:&argb bytesPerRow:sizeof argb];
        [blit copyFromTexture:staging sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1)
                    toTexture:s_surface destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(x, y, 0)];
        [blit endEncoding];
        if (!finish_command(command))
            return 0;
        s_dirty = 1;
        return 1;
    }
}
#endif

void bw_metal_backend_set_framebuffer_callback(BwMetalFramebufferCallback callback)
{
    s_framebuffer_callback = callback;
}

static id<MTLTexture> present_resolve_target(uint32_t width, uint32_t height)
{
    if (s_present_resolve && s_present_resolve.width == width &&
        s_present_resolve.height == height)
        return s_present_resolve;
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width height:height mipmapped:NO];
    if (!desc)
        return metal_fail("could not create presentation resolve descriptor"), nil;
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s_present_resolve = [s_device newTextureWithDescriptor:desc];
    if (!s_present_resolve)
        return metal_fail("could not create presentation resolve %ux%u", width, height), nil;
    return s_present_resolve;
}

static int update_present_gamma(int *enabled)
{
    uint8_t ramp[768], pixels[1024];
    *enabled = bw_gamma_lut(ramp);
    for (unsigned i = 0; i < 256; i++) {
        pixels[i * 4u + 0u] = *enabled ? ramp[i] : (uint8_t)i;
        pixels[i * 4u + 1u] = *enabled ? ramp[256u + i] : (uint8_t)i;
        pixels[i * 4u + 2u] = *enabled ? ramp[512u + i] : (uint8_t)i;
        pixels[i * 4u + 3u] = 0xff;
    }
    if (!s_present_gamma) {
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:256 height:1 mipmapped:NO];
        if (!desc)
            return metal_fail("could not create presentation gamma descriptor");
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageShaderRead;
        s_present_gamma = [s_device newTextureWithDescriptor:desc];
        if (!s_present_gamma)
            return metal_fail("could not create presentation gamma texture");
        s_present_gamma_valid = 0;
    }
    if (!s_present_gamma_valid ||
        memcmp(s_present_gamma_bytes, pixels, sizeof pixels) != 0) {
        if (!wait_for_all_submissions())
            return 0;
        [s_present_gamma replaceRegion:MTLRegionMake2D(0, 0, 256, 1)
                           mipmapLevel:0 withBytes:pixels bytesPerRow:sizeof pixels];
        memcpy(s_present_gamma_bytes, pixels, sizeof pixels);
        s_present_gamma_valid = 1;
    }
    return 1;
}

static int direct_present(void)
{
    if (!s_present_layer || !s_present_surface)
        return 1;
    id<CAMetalDrawable> drawable = [s_present_layer nextDrawable];
    if (!drawable) {
        s_dropped_drawables++;
        if (s_dropped_drawables <= 3u)
            fprintf(stderr, "[GPU-METAL] drawable unavailable; dropped presentation frame %llu\n",
                    (unsigned long long)s_dropped_drawables);
        return 1;
    }
    end_draw_encoder();
    id<MTLCommandBuffer> command = pending_command();
    if (!command || !s_encoding_submission)
        return metal_fail("could not create presentation command buffer");
    if (!mark_pending_command_reason(METAL_COMMAND_REASON_PRESENT))
        return 0;
    id<MTLTexture> source = s_present_surface;
    uint32_t logical_width = s_present_framebuffer.width / s_present_framebuffer.aa_sx;
    uint32_t logical_height = s_present_framebuffer.height / s_present_framebuffer.aa_sy;
    uint32_t picture_width =
        (uint32_t)fmax(1.0, floor((double)logical_width * s_render_scale + 0.5));
    uint32_t picture_height =
        (uint32_t)fmax(1.0, floor((double)logical_height * s_render_scale + 0.5));
    if (source.width != picture_width || source.height != picture_height) {
        source = present_resolve_target(picture_width, picture_height);
        if (!source)
            return 0;
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        if (!pass)
            return metal_fail("could not create presentation resolve pass");
        pass.colorAttachments[0].texture = source;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder =
            [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder)
            return metal_fail("could not create presentation resolve command");
        MetalScaleUniforms uniforms = {
            (uint32_t)s_present_surface.width, (uint32_t)s_present_surface.height,
            picture_width, picture_height,
        };
        [encoder setRenderPipelineState:s_box_pipeline];
        [encoder setFragmentTexture:s_present_surface atIndex:0];
        [encoder setFragmentBytes:&uniforms length:sizeof uniforms atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    }
    int gamma_enabled;
    if (!update_present_gamma(&gamma_enabled))
        return 0;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (!pass)
        return metal_fail("could not create presentation pass");
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder)
        return metal_fail("could not create presentation render command");
    double drawable_width = drawable.texture.width;
    double drawable_height = drawable.texture.height;
    double width, height;
    if (drawable_width * logical_height <= drawable_height * logical_width) {
        width = drawable_width;
        height = drawable_width * logical_height / logical_width;
    } else {
        height = drawable_height;
        width = drawable_height * logical_width / logical_height;
    }
    MTLViewport viewport = {
        floor((drawable_width - width) * 0.5),
        floor((drawable_height - height) * 0.5),
        width, height, 0.0, 1.0,
    };
    MetalPresentUniforms uniforms = {(uint32_t)gamma_enabled, {0, 0, 0}};
    [encoder setViewport:viewport];
    [encoder setRenderPipelineState:s_present_pipeline];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder setFragmentTexture:s_present_gamma atIndex:1];
    [encoder setFragmentSamplerState:s_present_sampler atIndex:0];
    [encoder setFragmentBytes:&uniforms length:sizeof uniforms atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command presentDrawable:drawable];
    s_encoding_submission->presented = 1;
    s_presented_frames++;
    return 1;
}

static BWMetalColorSurface *prepare_present_surface_texture(const Nv2aSurface *surface,
                                                            uint32_t format)
{
    BWMetalColorSurface *entry = cached_surface(surface, format);
    if (!entry)
        return nil;
    if (entry->texture == s_surface)
        return entry;
    if (surface_matches_guest_sync(entry, surface, format)) {
        s_present_cache_reuses++;
    } else {
        if (!prepare_cached_surface(entry, surface, format))
            return nil;
        s_present_guest_reseeds++;
    }
    return entry;
}

static int report_framebuffer(const Nv2aSurface *surface, uint32_t format)
{
    if (format != METAL_COLOR_ARGB8 && format != METAL_COLOR_XRGB_ZERO)
        return metal_fail("unsupported framebuffer descriptor format");
    BwMetalFramebuffer info = {
        .guest_va = surface->color_va,
        .pitch = surface->pitch,
        .width = surface->width,
        .height = surface->height,
        .pixel_format = format == METAL_COLOR_XRGB_ZERO
                      ? BW_METAL_PIXEL_BGRX8_ZERO : BW_METAL_PIXEL_BGRA8,
        .aa_sx = surface->aa_sx,
        .aa_sy = surface->aa_sy,
    };
    if (s_framebuffer_callback && !s_framebuffer_callback(&info))
        return metal_fail("presenter rejected framebuffer descriptor");
    if (s_present_layer) {
        BWMetalColorSurface *entry = prepare_present_surface_texture(surface, format);
        if (!entry)
            return 0;
        s_present_surface = entry->texture;
        s_present_framebuffer = info;
    }
    return 1;
}

static int metal_prepare_present(uint32_t color_va, uint32_t pitch)
{
    @autoreleasepool {
        BwBackendSnapshotSurface selected;
        if (bw_metal_backend_error())
            return 0;
        bw_backend_snapshot_surface(&selected);
        if (!color_va || color_va != selected.surface.color_va || pitch != selected.surface.pitch)
            return metal_fail("presented colour surface does not match the selected surface");
        int private_display = s_present_layer != nil;
#if defined(BW_METAL_TESTING)
        private_display = private_display || s_test_async_display;
#endif
        if ((!private_display || s_water_capture_enabled ||
             s_water_benchmark_enabled) && !writeback())
            return 0;
        if (!surface_valid_for(&selected.surface, selected.format,
                               selected.clip_x, selected.clip_y))
            return 0;
        if (depth_overlaps(color_va, span_end(color_va, selected.surface.width,
                                               selected.surface.height, pitch)))
            return metal_fail("colour surface aliases a cleared depth/stencil surface");
        return report_framebuffer(&selected.surface, selected.format & 0xFu);
    }
}

static int metal_publish_snapshot(uint32_t color_va, uint32_t pitch)
{
    @autoreleasepool {
        BwBackendSnapshotSurface selected;
        if (bw_metal_backend_error())
            return 0;
        bw_backend_snapshot_surface(&selected);
        if (!color_va || color_va != selected.surface.color_va ||
            pitch != selected.surface.pitch)
            return metal_fail("published colour surface does not match the selected surface");
        uint32_t format = selected.format & 0xFu;
        if (!surface_valid_for(&selected.surface, selected.format,
                               selected.clip_x, selected.clip_y))
            return 0;
        if (s_surface_entry &&
            same_surface(&selected.surface, &s_current) &&
            format == s_current_format)
            return writeback();
        BWMetalColorSurface *entry = cached_surface(&selected.surface, format);
        if (!entry)
            return 0;
        if (entry->private_dirty)
            return publish_cached_surface(entry, &selected.surface, format, 1, 0);
        return 1;
    }
}

static void metal_flip(void)
{
    @autoreleasepool {
        if (bw_metal_backend_error())
            return;
        water_reflect_reset_run();
        int asynchronous_display = s_present_layer != nil;
#if defined(BW_METAL_TESTING)
        asynchronous_display = asynchronous_display || s_test_async_display;
#endif
        asynchronous_display = asynchronous_display &&
                               !s_water_capture_enabled &&
                               !s_water_benchmark_enabled;
        if (s_dirty && !asynchronous_display) {
            metal_fail("flip occurred without a successful surface readback");
            return;
        }
        if (!water_capture_frame())
            return;
        water_benchmark_frame();
        apply_pending_render_settings();
        if (s_frame_timing_enabled) {
            if (s_frame_gpu_command_valid &&
                s_gpu_command_time_count < METAL_MAX_FRAME_SAMPLES) {
                s_gpu_command_times[s_gpu_command_time_count++] =
                    s_frame_gpu_command_ns;
                if (s_gpu_command_time_count >= 300u &&
                    !(s_gpu_command_time_count % 100u))
                    report_duration_times(
                        "GPU command", "300-frame window",
                        s_gpu_command_times + s_gpu_command_time_count - 300u,
                        300u);
            }
            if (s_frame_command_wait_ns &&
                s_command_wait_time_count < METAL_MAX_FRAME_SAMPLES) {
                s_command_wait_times[s_command_wait_time_count++] =
                    s_frame_command_wait_ns;
                if (s_command_wait_time_count >= 300u &&
                    !(s_command_wait_time_count % 100u))
                    report_duration_times(
                        "command wait", "300-frame window",
                        s_command_wait_times + s_command_wait_time_count - 300u,
                        300u);
            }
            s_frame_gpu_command_ns = 0;
            s_frame_command_wait_ns = 0;
            s_frame_gpu_command_valid = 0;
        }
        uint64_t now = xbox_HostNowNs();
        if (now && s_last_flip_ns && now > s_last_flip_ns &&
            s_frame_time_count < METAL_MAX_FRAME_SAMPLES) {
            uint64_t frame_duration = now - s_last_flip_ns;
            s_frame_times[s_frame_time_count++] = frame_duration;
            if (s_frame_timing_enabled && s_frame_time_count >= 300u &&
                !(s_frame_time_count % 100u))
                report_frame_times("300-frame window",
                                   s_frame_times + s_frame_time_count - 300u, 300u);
            if (s_frame_timing_enabled && s_frame_time_count >= 300u &&
                !(s_frame_time_count % 100u)) {
                fprintf(stderr,
                        "[GPU-METAL] surface-cache totals: %llu target reuses, "
                        "%llu present reuses, %llu present guest reseeds\n",
                        (unsigned long long)s_surface_cache_reuses,
                        (unsigned long long)s_present_cache_reuses,
                        (unsigned long long)s_present_guest_reseeds);
                fprintf(stderr,
                        "[GPU-METAL] GPU command roles: %llu presented commands, "
                        "%.2f ms total (%.2f ms mean); %llu offscreen commands, "
                        "%.2f ms total (%.2f ms mean)\n",
                        (unsigned long long)s_present_command_count,
                        (double)s_present_command_gpu_ns / 1.0e6,
                        s_present_command_count
                            ? (double)s_present_command_gpu_ns /
                                  (double)s_present_command_count / 1.0e6
                            : 0.0,
                        (unsigned long long)s_offscreen_command_count,
                        (double)s_offscreen_command_gpu_ns / 1.0e6,
                        s_offscreen_command_count
                            ? (double)s_offscreen_command_gpu_ns /
                                  (double)s_offscreen_command_count / 1.0e6
                            : 0.0);
                for (size_t i = 0;
                     i < sizeof s_command_role_surfaces /
                             sizeof s_command_role_surfaces[0];
                     i++) {
                    MetalCommandRoleSurface *role = &s_command_role_surfaces[i];
                    if (!role->count)
                        continue;
                    fprintf(stderr,
                            "[GPU-METAL] GPU surface role: %s 0x%08x %ux%u, "
                            "%llu commands, %.2f ms mean\n",
                            role->presented ? "presented" : "offscreen",
                            role->color_va, role->width, role->height,
                            (unsigned long long)role->count,
                            (double)role->nanoseconds /
                                (double)role->count / 1.0e6);
                }
                for (size_t i = 0;
                     i < sizeof s_command_reason_timings /
                             sizeof s_command_reason_timings[0];
                     i++) {
                    MetalCommandReasonTiming *timing =
                        &s_command_reason_timings[i];
                    if (!timing->count)
                        continue;
                    char reason[128];
                    command_reason_text(timing->reason_mask,
                                        reason, sizeof reason);
                    fprintf(stderr,
                            "[GPU-METAL] GPU command reason: %s, "
                            "%llu commands, %.2f ms mean\n",
                            reason, (unsigned long long)timing->count,
                            (double)timing->nanoseconds /
                                (double)timing->count / 1.0e6);
                }
                if (s_command_reason_timing_overflows)
                    fprintf(stderr,
                            "[GPU-METAL] GPU command reason timing omitted "
                            "%llu commands because the role table was full\n",
                            (unsigned long long)
                                s_command_reason_timing_overflows);
                if (s_feedback_checks || s_quincunx_preparations)
                    fprintf(stderr,
                            "[GPU-METAL] feedback/quincunx CPU: "
                            "%llu checks, %llu GPU-private, %llu fallbacks, "
                            "%llu publications, %.2f ms/check; "
                            "%llu preparations, %.2f MiB, %.2f ms/preparation\n",
                            (unsigned long long)s_feedback_checks,
                            (unsigned long long)s_gpu_private_feedback_resolves,
                            (unsigned long long)s_feedback_fallbacks,
                            (unsigned long long)s_feedback_publications,
                            s_feedback_checks
                                ? (double)s_feedback_publication_ns /
                                      (double)s_feedback_checks / 1.0e6
                                : 0.0,
                            (unsigned long long)s_quincunx_preparations,
                            (double)s_quincunx_preparation_bytes /
                                (1024.0 * 1024.0),
                            s_quincunx_preparations
                                ? (double)s_quincunx_preparation_ns /
                                      (double)s_quincunx_preparations / 1.0e6
                                : 0.0);
                s_present_command_gpu_ns = 0;
                s_offscreen_command_gpu_ns = 0;
                s_present_command_count = 0;
                s_offscreen_command_count = 0;
                memset(s_command_role_surfaces, 0,
                       sizeof s_command_role_surfaces);
                memset(s_command_reason_timings, 0,
                       sizeof s_command_reason_timings);
                s_command_reason_timing_overflows = 0;
                s_feedback_checks = 0;
                s_gpu_private_feedback_resolves = 0;
                s_feedback_fallbacks = 0;
                s_feedback_publications = 0;
                s_feedback_publication_ns = 0;
                s_quincunx_preparations = 0;
                s_quincunx_preparation_bytes = 0;
                s_quincunx_preparation_ns = 0;
            }
        }
        if (s_pipeline_diagnostic.enabled) {
            MetalPipelineTimingStatus wall_status =
                !now || !s_last_flip_ns ? METAL_PIPELINE_TIMING_UNAVAILABLE
              : now <= s_last_flip_ns ? METAL_PIPELINE_TIMING_NON_MONOTONIC
                                      : METAL_PIPELINE_TIMING_OK;
            pipeline_diagnostic_complete_frame(
                s_frame,
                wall_status == METAL_PIPELINE_TIMING_OK
                    ? now - s_last_flip_ns : 0,
                wall_status);
        }
        if (now)
            s_last_flip_ns = now;
        if (!direct_present())
            return;
        if (asynchronous_display) {
            if (!commit_commands_async())
                return;
            s_async_display_frames++;
            s_dirty = 0;
        } else if (!flush_commands()) {
            return;
        }
        s_present_surface = nil;
        s_surface = nil;
        s_surface_entry = nil;
        if (!refresh_render_scale(1))
            return;
        bw_ab_frame(++s_frame);
    }
}

static const Nv2aBackend s_backend = {metal_clear, metal_draw, metal_flip};
static const BwXBackend s_extended_backend = {metal_draw_extended, NULL, NULL, NULL};

static void disable_legacy_fast_math(MTLCompileOptions *options)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
}

static int bind_shader_functions(id<MTLLibrary> library, int require_realistic)
{
    id<MTLFunction> vertex = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> color = [library newFunctionWithName:@"color_main"];
    id<MTLFunction> texture = [library newFunctionWithName:@"texture_main"];
    id<MTLFunction> extended_vertex =
        [library newFunctionWithName:@"extended_vertex"];
    id<MTLFunction> extended_fragment =
        [library newFunctionWithName:@"extended_fragment"];
    id<MTLFunction> realistic_water =
        [library newFunctionWithName:@"realistic_water_fragment"];
    id<MTLFunction> clear_vertex = [library newFunctionWithName:@"clear_vertex"];
    id<MTLFunction> clear_color = [library newFunctionWithName:@"clear_color"];
    id<MTLFunction> clear_depth = [library newFunctionWithName:@"clear_depth"];
    id<MTLFunction> present_vertex =
        [library newFunctionWithName:@"present_vertex"];
    id<MTLFunction> present_resolve =
        [library newFunctionWithName:@"present_resolve"];
    id<MTLFunction> present_fragment =
        [library newFunctionWithName:@"present_fragment"];
    id<MTLFunction> scale = [library newFunctionWithName:@"scale_fragment"];
    id<MTLFunction> box = [library newFunctionWithName:@"box_fragment"];
    if (!vertex || !color || !texture || !extended_vertex ||
        !extended_fragment || (require_realistic && !realistic_water) ||
        !clear_vertex || !clear_color || !clear_depth || !present_vertex ||
        !present_resolve || !present_fragment || !scale || !box)
        return 0;
    s_vertex_function = vertex;
    s_color_function = color;
    s_texture_function = texture;
    s_extended_vertex_function = extended_vertex;
    s_extended_fragment_function = extended_fragment;
    s_realistic_water_fragment_function = realistic_water;
    s_clear_vertex_function = clear_vertex;
    s_clear_color_function = clear_color;
    s_clear_depth_function = clear_depth;
    s_present_vertex_function = present_vertex;
    s_present_resolve_function = present_resolve;
    s_present_fragment_function = present_fragment;
    s_scale_function = scale;
    s_box_function = box;
    return 1;
}

static id<MTLLibrary> load_offline_shader_library(
    const uint8_t *bytes, size_t size, NSError **error,
    MetalPipelineStatus *status, int *entry_point_missing)
{
    dispatch_data_t data =
        dispatch_data_create(bytes, size, NULL, ^{});
    if (!data) {
        *status = METAL_PIPELINE_STATUS_ALLOCATION_FAILED;
        return nil;
    }
    id<MTLLibrary> library = [s_device newLibraryWithData:data error:error];
    if (!library) {
        *status = METAL_PIPELINE_STATUS_ARTIFACT_REJECTED;
        return nil;
    }
    if (!bind_shader_functions(library, 1)) {
        *entry_point_missing = 1;
        *status = METAL_PIPELINE_STATUS_ARTIFACT_REJECTED;
        return nil;
    }
    *status = METAL_PIPELINE_STATUS_OK;
    return library;
}

static int initialize_metal(void)
{
    NSError *error = nil;
    uint64_t library_hash = s_pipeline_diagnostic.enabled
                          ? pipeline_key_hash(
                                1, s_metal_shader_source,
                                s_metal_shader_source_size)
                          : 0;
    if (!s_device)
        s_device = MTLCreateSystemDefaultDevice();
    if (!s_device)
        return metal_fail("no Metal device");
    if (s_present_layer && s_present_layer.device != s_device)
        return metal_fail("direct presenter and backend use different Metal devices");
    s_queue = [s_device newCommandQueue];
    if (!s_queue)
        return metal_fail("could not create Metal command queue");
    const uint8_t *offline_bytes = s_quincunx_approx
                                  ? s_metal_shader_library_precise
                                  : s_metal_shader_library_default;
    size_t offline_size = s_quincunx_approx
                        ? s_metal_shader_library_precise_size
                        : s_metal_shader_library_default_size;
#if defined(BW_METAL_TESTING)
    static const uint8_t invalid_offline_library[] = {0};
    if (s_test_fail_offline_library) {
        offline_bytes = invalid_offline_library;
        offline_size = sizeof invalid_offline_library;
    }
#endif
    if (offline_size) {
        MetalPipelineStatus load_status = METAL_PIPELINE_STATUS_ARTIFACT_REJECTED;
        MetalPipelineTimer load_timer = pipeline_diagnostic_begin();
        int entry_point_missing = 0;
        s_shader_library = load_offline_shader_library(
            offline_bytes, offline_size, &error, &load_status,
            &entry_point_missing);
        pipeline_diagnostic_finish(
            METAL_PIPELINE_EVENT_LIBRARY_LOAD, library_hash, 0,
            METAL_PIPELINE_ROLE_STARTUP, load_status, load_timer);
        if (s_shader_library) {
            if (s_pipeline_diagnostic.enabled)
                s_pipeline_diagnostic.library_artifact_loaded = 1;
        } else {
            if (s_pipeline_diagnostic.enabled)
                s_pipeline_diagnostic.library_rejections++;
            fprintf(stderr,
                    "[GPU-METAL] offline shader library rejected (%s); "
                    "compiling source fallback\n",
                    entry_point_missing ? "required entry point missing"
                                        : metal_error_text(error));
        }
    } else {
        if (s_pipeline_diagnostic.enabled)
            s_pipeline_diagnostic.library_unavailable++;
        pipeline_diagnostic_record_instant(
            METAL_PIPELINE_EVENT_LIBRARY_LOAD, library_hash, 0,
            METAL_PIPELINE_ROLE_STARTUP,
            METAL_PIPELINE_STATUS_ARTIFACT_UNAVAILABLE);
        fprintf(stderr,
                "[GPU-METAL] offline shader library unavailable in this build; "
                "compiling source fallback\n");
    }
    if (!s_shader_library) {
        if (s_pipeline_diagnostic.enabled)
            s_pipeline_diagnostic.library_fallbacks++;
        NSString *source = [[NSString alloc]
            initWithBytes:s_metal_shader_source
                   length:s_metal_shader_source_size
                 encoding:NSUTF8StringEncoding];
        if (!source) {
            pipeline_diagnostic_finish(
                METAL_PIPELINE_EVENT_LIBRARY_COMPILE, library_hash, 0,
                METAL_PIPELINE_ROLE_STARTUP,
                METAL_PIPELINE_STATUS_ALLOCATION_FAILED,
                (MetalPipelineTimer){0,
                                     METAL_PIPELINE_TIMING_NOT_APPLICABLE});
            return metal_fail("could not create Metal shader source");
        }
        MTLCompileOptions *options = nil;
        if (s_quincunx_approx) {
            /* Keep the reference filter's float operation order. */
            options = [MTLCompileOptions new];
            if (!options) {
                pipeline_diagnostic_finish(
                    METAL_PIPELINE_EVENT_LIBRARY_COMPILE, library_hash, 0,
                    METAL_PIPELINE_ROLE_STARTUP,
                    METAL_PIPELINE_STATUS_ALLOCATION_FAILED,
                    (MetalPipelineTimer){0,
                                         METAL_PIPELINE_TIMING_NOT_APPLICABLE});
                return metal_fail("could not create precise Metal shader options");
            }
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000 && \
    !(defined(BW_METAL_TESTING) && defined(BW_METAL_TEST_FORCE_LEGACY_MATH))
            if (@available(macOS 15.0, *)) {
                options.mathMode = MTLMathModeSafe;
            } else {
                disable_legacy_fast_math(options);
            }
#else
            disable_legacy_fast_math(options);
#endif
        }
        error = nil;
        MetalPipelineTimer library_timer = pipeline_diagnostic_begin();
#if defined(BW_METAL_TESTING)
        if (s_test_fail_source_library) {
            s_shader_library = nil;
        } else
#endif
        {
            s_shader_library =
                [s_device newLibraryWithSource:source options:options error:&error];
        }
        pipeline_diagnostic_finish(
            METAL_PIPELINE_EVENT_LIBRARY_COMPILE, library_hash, 0,
            METAL_PIPELINE_ROLE_STARTUP,
            s_shader_library ? METAL_PIPELINE_STATUS_OK
                             : METAL_PIPELINE_STATUS_METAL_FAILED,
            library_timer);
        if (!s_shader_library)
            return metal_fail("shader compilation failed: %s",
                              metal_error_text(error));
        if (!bind_shader_functions(s_shader_library, 0))
            return metal_fail("Metal shader entry point missing");
    }
    if (!s_vertex_function || !s_color_function || !s_texture_function ||
        !s_extended_vertex_function || !s_extended_fragment_function ||
        !s_clear_vertex_function || !s_clear_color_function ||
        !s_clear_depth_function || !s_present_vertex_function ||
        !s_present_resolve_function || !s_present_fragment_function ||
        !s_scale_function || !s_box_function)
        return metal_fail("Metal shader entry point missing");
    if (!s_realistic_water_fragment_function &&
        s_active_water_preset == BW_METAL_WATER_REALISTIC) {
        fprintf(stderr,
                "[GPU-METAL] realistic water shader is unavailable; using retail water\n");
        s_active_water_preset = BW_METAL_WATER_RETAIL;
        atomic_store_explicit(&s_requested_water_preset,
                              BW_METAL_WATER_RETAIL, memory_order_release);
    }
    uint32_t startup_key = 1;
    uint64_t startup_hash = s_pipeline_diagnostic.enabled
                          ? pipeline_key_hash(2, &startup_key, sizeof startup_key)
                          : 0;
    MTLRenderPipelineDescriptor *present_pipeline = new_pipeline_descriptor();
    if (!present_pipeline) {
        (void)create_render_pipeline_state(
            nil, startup_hash, 0, METAL_PIPELINE_ROLE_STARTUP, NULL);
        return metal_fail("could not create presentation pipeline descriptor");
    }
    present_pipeline.vertexFunction = s_present_vertex_function;
    present_pipeline.fragmentFunction = s_present_resolve_function;
    present_pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    s_present_resolve_pipeline =
        create_render_pipeline_state(
            present_pipeline, startup_hash, 0,
            METAL_PIPELINE_ROLE_STARTUP, &error);
    if (!s_present_resolve_pipeline)
        return metal_fail("presentation resolve pipeline creation failed: %s",
                          metal_error_text(error));
    present_pipeline.fragmentFunction = s_scale_function;
    startup_key = 2;
    startup_hash = s_pipeline_diagnostic.enabled
                 ? pipeline_key_hash(2, &startup_key, sizeof startup_key) : 0;
    error = nil;
    s_scale_pipeline =
        create_render_pipeline_state(
            present_pipeline, startup_hash, 0,
            METAL_PIPELINE_ROLE_STARTUP, &error);
    if (!s_scale_pipeline)
        return metal_fail("surface scaling pipeline creation failed: %s",
                          metal_error_text(error));
    present_pipeline.fragmentFunction = s_box_function;
    startup_key = 3;
    startup_hash = s_pipeline_diagnostic.enabled
                 ? pipeline_key_hash(2, &startup_key, sizeof startup_key) : 0;
    error = nil;
    s_box_pipeline =
        create_render_pipeline_state(
            present_pipeline, startup_hash, 0,
            METAL_PIPELINE_ROLE_STARTUP, &error);
    if (!s_box_pipeline)
        return metal_fail("surface box-filter pipeline creation failed: %s",
                          metal_error_text(error));
    present_pipeline.fragmentFunction = s_present_fragment_function;
    startup_key = 4;
    startup_hash = s_pipeline_diagnostic.enabled
                 ? pipeline_key_hash(2, &startup_key, sizeof startup_key) : 0;
    error = nil;
    s_present_pipeline =
        create_render_pipeline_state(
            present_pipeline, startup_hash, 0,
            METAL_PIPELINE_ROLE_STARTUP, &error);
    if (!s_present_pipeline)
        return metal_fail("presentation pipeline creation failed: %s",
                          metal_error_text(error));
    s_pipelines = [NSMutableDictionary new];
    if (!s_pipelines)
        return metal_fail("could not create draw pipeline cache");
    MTLSamplerDescriptor *sampler = [MTLSamplerDescriptor new];
    sampler.minFilter = MTLSamplerMinMagFilterNearest;
    sampler.magFilter = MTLSamplerMinMagFilterNearest;
    sampler.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_sampler = [s_device newSamplerStateWithDescriptor:sampler];
    if (!s_sampler)
        return metal_fail("sampler creation failed");
    sampler.minFilter = MTLSamplerMinMagFilterLinear;
    sampler.magFilter = MTLSamplerMinMagFilterLinear;
    s_present_sampler = [s_device newSamplerStateWithDescriptor:sampler];
    if (!s_present_sampler)
        return metal_fail("presentation sampler creation failed");
    const MTLSamplerAddressMode modes[3] = {
        MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat,
        MTLSamplerAddressModeClampToEdge,
    };
    sampler.lodMinClamp = 0.0f;
    const unsigned anisotropy_values[5] = {1, 2, 4, 8, 16};
    for (unsigned a = 0; a < 5; a++) {
        sampler.maxAnisotropy = anisotropy_values[a];
        for (unsigned u = 0; u < 3; u++) {
            for (unsigned v = 0; v < 3; v++) {
                sampler.sAddressMode = modes[u];
                sampler.tAddressMode = modes[v];
                for (unsigned min_linear = 0; min_linear < 2; min_linear++)
                    for (unsigned mag_linear = 0; mag_linear < 2; mag_linear++)
                        for (unsigned mip_mode = 0; mip_mode < 3; mip_mode++) {
                            sampler.minFilter = min_linear ? MTLSamplerMinMagFilterLinear
                                                           : MTLSamplerMinMagFilterNearest;
                            sampler.magFilter = mag_linear ? MTLSamplerMinMagFilterLinear
                                                           : MTLSamplerMinMagFilterNearest;
                            sampler.mipFilter = mip_mode == 2u ? MTLSamplerMipFilterLinear
                                              : mip_mode == 1u ? MTLSamplerMipFilterNearest
                                                               : MTLSamplerMipFilterNotMipmapped;
                            sampler.lodMaxClamp = mip_mode ? FLT_MAX : 0.0f;
                            s_samplers[a][u][v][min_linear][mag_linear][mip_mode] =
                                [s_device newSamplerStateWithDescriptor:sampler];
                            if (!s_samplers[a][u][v][min_linear][mag_linear][mip_mode])
                                return metal_fail("extended anisotropic sampler creation failed");
                        }
            }
        }
    }
    return 1;
}

static int start_with_regions(void *guest_memory, size_t low_size,
                              uint32_t contig_va, size_t contig_size)
{
    const uint64_t guest_limit = UINT64_C(1) << 32;
    uint64_t contig_end, top;
    if (contig_size > guest_limit || (uint64_t)contig_va > guest_limit - contig_size)
        return metal_fail("guest memory regions do not match the executor");
    contig_end = (uint64_t)contig_va + contig_size;
    top = contig_size ? contig_end : low_size;
    if (!guest_memory || !low_size || ((uintptr_t)guest_memory & 3u) ||
        (uintptr_t)guest_memory != (uintptr_t)xbox_GetMemoryOffset() ||
        low_size > guest_limit ||
        (contig_size && (uint64_t)low_size > contig_va) ||
        top - 1u > UINTPTR_MAX - (uintptr_t)guest_memory)
        return metal_fail("guest memory regions do not match the executor");
    s_guest = (uintptr_t)guest_memory;
    s_guest_regions[0] = (MetalGuestRegion){0, low_size};
    s_guest_regions[1] = (MetalGuestRegion){contig_va, contig_end};
    {
        const char *pipeline_timing = getenv("BW_METAL_PIPELINE_TIMING");
        pipeline_diagnostic_reset(0);
        if (pipeline_timing && strcmp(pipeline_timing, "1"))
            return metal_fail("BW_METAL_PIPELINE_TIMING must be 1 when set");
        pipeline_diagnostic_reset(pipeline_timing != NULL);
    }
    {
        const char *vfx = getenv("BW_VFX");
        const char *af = getenv("BW_VFX_AF");
        unsigned anisotropy = 1;
        if (bw_edition_at_least(BW_ED_REMASTERED) && !(vfx && !strcmp(vfx, "0"))) {
            anisotropy = bw_fix_enabled(BW_FIX_VFX_ANISO) ? 16u : 1u;
            if (af && *af) {
                char *end = NULL;
                unsigned long parsed;
                if (!strcasecmp(af, "off"))
                    anisotropy = 1;
                else if (!strcasecmp(af, "on"))
                    anisotropy = 16;
                else {
                    parsed = strtoul(af, &end, 10);
                    if (!end || *end || parsed > UINT_MAX ||
                        !valid_anisotropy((unsigned)parsed))
                        return metal_fail("BW_VFX_AF must be off, on, 1, 2, 4, 8 or 16");
                    anisotropy = (unsigned)parsed;
                }
            }
        }
        atomic_store_explicit(&s_requested_anisotropy, anisotropy, memory_order_release);
        s_active_anisotropy = anisotropy;
    }
    {
        const char *water = getenv("BW_METAL_WATER");
        BwMetalWaterPreset preset = BW_METAL_WATER_RETAIL;
        if (water) {
            if (!strcasecmp(water, "retail"))
                preset = BW_METAL_WATER_RETAIL;
            else if (!strcasecmp(water, "realistic"))
                preset = BW_METAL_WATER_REALISTIC;
            else
                return metal_fail("BW_METAL_WATER must be retail or realistic");
        }
        if (preset == BW_METAL_WATER_REALISTIC &&
            !bw_edition_at_least(BW_ED_REMASTERED))
            return metal_fail("BW_METAL_WATER=realistic requires Remastered");
        atomic_store_explicit(&s_requested_water_preset, preset, memory_order_release);
        s_active_water_preset = preset;
    }
    {
        const char *single = getenv("BW_OCEAN_REFLECT_SINGLE");
        s_water_reflect_single_enabled =
            bw_edition_at_least(BW_ED_REMASTERED) &&
            bw_fix_enabled(BW_FIX_OCEAN_REFLECT_SINGLE) &&
            !(single && single[0] == '0');
    }
    {
        const char *benchmark = getenv("BW_METAL_WATER_BENCHMARK");
        s_water_benchmark_enabled = 0;
        if (benchmark) {
            if (strcmp(benchmark, "1"))
                return metal_fail("BW_METAL_WATER_BENCHMARK must be 1 when set");
            if (!bw_edition_at_least(BW_ED_REMASTERED))
                return metal_fail("BW_METAL_WATER_BENCHMARK requires Remastered");
            const char *diagnostic = getenv("BW_NATIVE_DIAGNOSTIC");
            if (!diagnostic || strcmp(diagnostic, "1"))
                return metal_fail("BW_METAL_WATER_BENCHMARK requires the private diagnostic runner");
            const char *audit = getenv("BW_GPU_AUDIT");
            if (!audit || strcmp(audit, "1"))
                return metal_fail("BW_METAL_WATER_BENCHMARK requires BW_GPU_AUDIT=1");
            if (s_active_water_preset != BW_METAL_WATER_RETAIL)
                return metal_fail("BW_METAL_WATER_BENCHMARK requires the retail starting preset");
            s_water_benchmark_enabled = 1;
        }
    }
    {
        const char *capture = getenv("BW_METAL_WATER_CAPTURE_TRIGGER");
        s_water_capture_enabled = 0;
        s_water_capture_trigger[0] = '\0';
        if (capture) {
            const char *diagnostic = getenv("BW_NATIVE_DIAGNOSTIC");
            const char *audit = getenv("BW_GPU_AUDIT");
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *flips = getenv("RECOMP_FB_DUMP_FLIPS");
            if (!*capture || capture[0] != '/' ||
                strlen(capture) >= sizeof s_water_capture_trigger)
                return metal_fail("BW_METAL_WATER_CAPTURE_TRIGGER must be a bounded absolute path");
            if (!bw_edition_at_least(BW_ED_REMASTERED))
                return metal_fail("private water capture requires Remastered");
            if (!diagnostic || strcmp(diagnostic, "1"))
                return metal_fail("private water capture requires the private diagnostic runner");
            if (!audit || strcmp(audit, "1"))
                return metal_fail("private water capture requires BW_GPU_AUDIT=1");
            if (s_water_benchmark_enabled)
                return metal_fail("private water capture cannot run with the water benchmark");
            if (!dump || !*dump || dump[0] != '/' ||
                !flips || strcmp(flips, capture))
                return metal_fail("private water capture requires matching framebuffer dump paths");
            memcpy(s_water_capture_trigger, capture, strlen(capture) + 1u);
            s_water_capture_preset = s_active_water_preset;
            s_water_capture_enabled = 1;
        }
    }
    s_pending_command = nil;
    s_encoding_submission = nil;
    s_inflight_submissions = [NSMutableArray new];
    s_free_submissions = [NSMutableArray new];
    if (!s_inflight_submissions || !s_free_submissions)
        return metal_fail("could not allocate GPU submission queues");
    s_draw_encoder = nil;
    s_draw_color = nil;
    s_draw_depth = nil;
    s_command_submissions = 0;
    s_draw_encoder_creations = 0;
    s_vertex_upload_allocations = 0;
    s_texture_uploads = 0;
    s_surface_seed_count = 0;
    s_surface_cache_reuses = 0;
    s_present_cache_reuses = 0;
    s_present_guest_reseeds = 0;
    s_realistic_water_batches = 0;
    s_realistic_water_fallback_batches = 0;
    s_realistic_water_reported = 0;
    s_realistic_water_fallback_reported = 0;
    s_water_reflect_single_failed = 0;
    s_water_reflect_single_reported = 0;
    s_water_reflect_active = 0;
    s_water_reflect_primed = 0;
    s_water_reflect_view = BW_X_WATER_VIEW_UNKNOWN;
    s_water_reflect_depth = nil;
    s_water_reflect_source = nil;
    s_water_reflect_prime_state = nil;
    s_water_reflect_test_state = nil;
    s_water_reflect_copies = 0;
    s_water_reflect_prime_draws = 0;
    s_water_reflect_filtered_draws = 0;
#if defined(BW_METAL_TESTING)
    s_water_reflect_test_failure = 0;
    s_test_async_display = 0;
    s_test_defer_retirement = 0;
    s_test_fail_next_submission = 0;
    s_test_fail_next_pipeline_allocation = 0;
#endif
    s_water_benchmark_started = 0;
    s_water_benchmark_complete = 0;
    s_water_benchmark_failed = 0;
    s_water_benchmark_block = 0;
    s_water_benchmark_frames = 0;
    s_water_benchmark_sampling = 0;
    memset(s_water_benchmark_samples, 0, sizeof s_water_benchmark_samples);
    memset(s_water_benchmark_sample_count, 0, sizeof s_water_benchmark_sample_count);
    s_water_capture_complete = 0;
    s_water_capture_trigger_active = 0;
    s_water_capture_supported_frames = 0;
    s_water_capture_images = 0;
    s_frame_supported_water_batches = 0;
    s_frame_water_gpu_ns = 0;
    s_frame_water_gpu_valid = 0;
    s_frame_water_gpu_invalid = 0;
    s_pending_supported_water = 0;
    s_present_surface = nil;
    memset(&s_present_framebuffer, 0, sizeof s_present_framebuffer);
    s_present_resolve = nil;
    s_present_gamma = nil;
    s_present_gamma_valid = 0;
    s_presented_frames = 0;
    s_dropped_drawables = 0;
    s_surface_generation = 0;
    s_async_display_frames = 0;
    s_guest_publications = 0;
    s_guest_publication_bytes = 0;
    s_queue_saturation_waits = 0;
    s_queue_saturation_wait_ns = 0;
    s_queue_depth_high_water = 0;
    s_frame_time_count = 0;
    s_gpu_command_time_count = 0;
    s_command_wait_time_count = 0;
    s_submission_latency_time_count = 0;
    s_frame_gpu_command_ns = 0;
    s_frame_command_wait_ns = 0;
    s_frame_gpu_command_valid = 0;
    s_present_command_gpu_ns = 0;
    s_offscreen_command_gpu_ns = 0;
    s_present_command_count = 0;
    s_offscreen_command_count = 0;
    memset(s_command_role_surfaces, 0, sizeof s_command_role_surfaces);
    memset(s_command_reason_timings, 0, sizeof s_command_reason_timings);
    s_command_reason_timing_overflows = 0;
    s_feedback_checks = 0;
    s_gpu_private_feedback_resolves = 0;
    s_feedback_fallbacks = 0;
    s_feedback_publications = 0;
    s_feedback_publication_ns = 0;
    s_quincunx_preparations = 0;
    s_quincunx_preparation_bytes = 0;
    s_quincunx_preparation_ns = 0;
    s_gpu_command_timestamp_failures = 0;
    s_last_flip_ns = 0;
    const char *audit = getenv("BW_GPU_AUDIT");
    const char *frame_timing = getenv("BW_METAL_FRAME_TIMING");
    s_frame_timing_enabled =
        (audit && !strcmp(audit, "1")) ||
        (frame_timing && !strcmp(frame_timing, "1"));
    const char *approx = getenv("BW_METAL_QUINCUNX_APPROX");
    s_quincunx_approx = approx && !strcmp(approx, "1");
    if (!configure_render_scale()) {
        bw_metal_backend_stop();
        return 0;
    }
    if (!initialize_metal()) {
        bw_metal_backend_stop();
        return 0;
    }
    bw_backend_register_with_present_ops(&s_backend, metal_prepare_present,
                                         metal_publish_snapshot);
    bw_backend_ext_register(&s_extended_backend);
    s_frame = 0;
    bw_ab_frame(0);
    s_registered = 1;
    if (s_quincunx_approx)
        fprintf(stderr, "[GPU-METAL] WARNING: BW_METAL_QUINCUNX_APPROX=1 uses a legacy "
                        "Gaussian approximation, not verified NV2A quincunx filtering\n");
    if (s_water_benchmark_enabled)
        fprintf(stderr,
                "[GPU-METAL] diagnostic water benchmark armed; Retail remains active "
                "until a supported marked ocean frame arrives\n");
    if (s_water_capture_enabled)
        fprintf(stderr,
                "[GPU-METAL] private water capture armed for four bounded "
                "supported-ocean frames\n");
    return 1;
}

int bw_metal_backend_start(void)
{
    @autoreleasepool {
        void *base, *contig = NULL;
        uint32_t contig_va = 0;
        size_t contig_size = 0, low_size;
        if (s_registered)
            return metal_fail("backend is already registered");
        atomic_store_explicit(&s_error_ready, 0, memory_order_release);
        base = xbox_GetMemoryBase();
        low_size = xbox_GetMappedSize();
        if (!base || !bw_xbox_contiguous_memory_region(&contig, &contig_va, &contig_size) ||
            !contig || !contig_size ||
            (uintptr_t)base > UINTPTR_MAX - contig_va ||
            (uintptr_t)contig != (uintptr_t)base + contig_va)
            return metal_fail("guest memory or contiguous window is unavailable");
        return start_with_regions(base, low_size, contig_va, contig_size);
    }
}

int bw_metal_backend_start_for_test(void *guest_memory, size_t guest_size)
{
    @autoreleasepool {
        if (s_registered)
            return metal_fail("backend is already registered");
        atomic_store_explicit(&s_error_ready, 0, memory_order_release);
        return start_with_regions(guest_memory, guest_size, 0, 0);
    }
}

void bw_metal_backend_stop(void)
{
    (void)flush_commands();
    pipeline_diagnostic_report();
    if (s_frame_timing_enabled)
        report_frame_times("all", s_frame_times, s_frame_time_count);
    size_t trailing = s_frame_time_count < 300u ? s_frame_time_count : 300u;
    if (s_frame_timing_enabled && trailing && trailing != s_frame_time_count)
        report_frame_times("trailing 300",
                           s_frame_times + s_frame_time_count - trailing, trailing);
    if (s_frame_timing_enabled)
        report_duration_times("GPU command", "all",
                              s_gpu_command_times, s_gpu_command_time_count);
    trailing = s_gpu_command_time_count < 300u ? s_gpu_command_time_count : 300u;
    if (s_frame_timing_enabled && trailing &&
        trailing != s_gpu_command_time_count)
        report_duration_times("GPU command", "trailing 300",
                              s_gpu_command_times +
                                  s_gpu_command_time_count - trailing,
                              trailing);
    if (s_frame_timing_enabled)
        report_duration_times("command wait", "all",
                              s_command_wait_times, s_command_wait_time_count);
    trailing = s_command_wait_time_count < 300u ? s_command_wait_time_count : 300u;
    if (s_frame_timing_enabled && trailing &&
        trailing != s_command_wait_time_count)
        report_duration_times("command wait", "trailing 300",
                              s_command_wait_times +
                                  s_command_wait_time_count - trailing,
                              trailing);
    if (s_frame_timing_enabled)
        report_duration_times("submission latency", "all",
                              s_submission_latency_times,
                              s_submission_latency_time_count);
    trailing = s_submission_latency_time_count < 300u
             ? s_submission_latency_time_count : 300u;
    if (s_frame_timing_enabled && trailing &&
        trailing != s_submission_latency_time_count)
        report_duration_times(
            "submission latency", "trailing 300",
            s_submission_latency_times +
                s_submission_latency_time_count - trailing,
            trailing);
    if (s_frame_timing_enabled && s_gpu_command_timestamp_failures)
        fprintf(stderr,
                "[GPU-METAL] GPU command timing omitted %llu commands with "
                "unavailable timestamps\n",
                (unsigned long long)s_gpu_command_timestamp_failures);
    if (s_frame_timing_enabled)
        fprintf(stderr,
                "[GPU-METAL] async ownership: %llu display-only frames, "
                "%llu guest publications (%llu bytes), queue high-water %u/%u, "
                "%llu saturation waits (%.2f ms total)\n",
                (unsigned long long)s_async_display_frames,
                (unsigned long long)s_guest_publications,
                (unsigned long long)s_guest_publication_bytes,
                s_queue_depth_high_water, METAL_MAX_IN_FLIGHT,
                (unsigned long long)s_queue_saturation_waits,
                (double)s_queue_saturation_wait_ns / 1.0e6);
    if (s_realistic_water_batches || s_realistic_water_fallback_batches)
        fprintf(stderr,
                "[GPU-METAL] realistic water summary: %llu enhanced batches, "
                "%llu retail fallbacks\n",
                (unsigned long long)s_realistic_water_batches,
                (unsigned long long)s_realistic_water_fallback_batches);
    if (s_water_reflect_copies || s_water_reflect_prime_draws ||
        s_water_reflect_filtered_draws)
        fprintf(stderr,
                "[GPU-METAL] distant-water-line summary: %llu depth copies, "
                "%llu lit primes, %llu filtered reflection draws\n",
                (unsigned long long)s_water_reflect_copies,
                (unsigned long long)s_water_reflect_prime_draws,
                (unsigned long long)s_water_reflect_filtered_draws);
    if (s_registered && s_water_benchmark_enabled &&
        !s_water_benchmark_complete && !s_water_benchmark_failed)
        fprintf(stderr,
                "[GPU-METAL] water benchmark incomplete: block %u, %s frame %u; "
                "retail samples %zu, realistic samples %zu\n",
                s_water_benchmark_block + 1u,
                s_water_benchmark_sampling ? "measured" : "warm-up",
                s_water_benchmark_frames,
                s_water_benchmark_sample_count[BW_METAL_WATER_RETAIL],
                s_water_benchmark_sample_count[BW_METAL_WATER_REALISTIC]);
    if (s_water_capture_trigger_active &&
        unlink(s_water_capture_trigger) != 0)
        metal_fail("could not remove the private water capture trigger: %s",
                   strerror(errno));
    if (s_registered && s_water_capture_enabled &&
        !s_water_capture_complete)
        fprintf(stderr,
                "[GPU-METAL] water capture incomplete: %u/%u images, "
                "%u supported frames\n",
                s_water_capture_images, METAL_WATER_CAPTURE_IMAGES,
                s_water_capture_supported_frames);
    if (s_registered) {
        bw_backend_ext_unregister_if_current(&s_extended_backend);
        bw_backend_unregister_if_current(&s_backend);
    }
    s_registered = 0;
    s_dirty = 0;
    s_quincunx_approx = 0;
    s_quincunx_reported = 0;
    s_realistic_water_batches = 0;
    s_realistic_water_fallback_batches = 0;
    s_realistic_water_reported = 0;
    s_realistic_water_fallback_reported = 0;
    s_water_reflect_single_enabled = 0;
    s_water_reflect_single_failed = 0;
    s_water_reflect_single_reported = 0;
    s_water_reflect_active = 0;
    s_water_reflect_primed = 0;
    s_water_reflect_view = BW_X_WATER_VIEW_UNKNOWN;
    s_water_reflect_depth = nil;
    s_water_reflect_source = nil;
    s_water_reflect_prime_state = nil;
    s_water_reflect_test_state = nil;
    s_water_reflect_copies = 0;
    s_water_reflect_prime_draws = 0;
    s_water_reflect_filtered_draws = 0;
    s_water_benchmark_enabled = 0;
    s_water_benchmark_started = 0;
    s_water_benchmark_complete = 0;
    s_water_benchmark_failed = 0;
    s_water_benchmark_block = 0;
    s_water_benchmark_frames = 0;
    s_water_benchmark_sampling = 0;
    memset(s_water_benchmark_samples, 0, sizeof s_water_benchmark_samples);
    memset(s_water_benchmark_sample_count, 0, sizeof s_water_benchmark_sample_count);
    s_water_capture_enabled = 0;
    s_water_capture_complete = 0;
    s_water_capture_trigger_active = 0;
    s_water_capture_supported_frames = 0;
    s_water_capture_images = 0;
    s_water_capture_preset = BW_METAL_WATER_RETAIL;
    s_water_capture_trigger[0] = '\0';
    s_frame_supported_water_batches = 0;
    s_frame_water_gpu_ns = 0;
    s_frame_water_gpu_valid = 0;
    s_frame_water_gpu_invalid = 0;
    s_pending_supported_water = 0;
    s_current_format = 0;
    s_framebuffer_callback = NULL;
    s_surface = nil;
    s_surface_entry = nil;
    s_present_surface = nil;
    memset(&s_present_framebuffer, 0, sizeof s_present_framebuffer);
    s_present_resolve = nil;
    s_present_gamma = nil;
    s_present_gamma_valid = 0;
    s_present_sampler = nil;
    s_present_resolve_pipeline = nil;
    s_present_pipeline = nil;
    s_scale_pipeline = nil;
    s_box_pipeline = nil;
    s_pending_command = nil;
    s_encoding_submission = nil;
    s_inflight_submissions = nil;
    s_free_submissions = nil;
    s_draw_encoder = nil;
    s_draw_color = nil;
    s_draw_depth = nil;
    s_inactive_texture = nil;
    s_inactive_cube_texture = nil;
    s_sampler = nil;
    for (unsigned a = 0; a < 5; a++)
        for (unsigned u = 0; u < 3; u++)
            for (unsigned v = 0; v < 3; v++)
                for (unsigned min_linear = 0; min_linear < 2; min_linear++)
                    for (unsigned mag_linear = 0; mag_linear < 2; mag_linear++)
                        for (unsigned mip_mode = 0; mip_mode < 3; mip_mode++)
                            s_samplers[a][u][v][min_linear][mag_linear][mip_mode] = nil;
    s_active_anisotropy = 1;
    atomic_store_explicit(&s_requested_anisotropy, 1, memory_order_release);
    s_active_water_preset = BW_METAL_WATER_RETAIL;
    atomic_store_explicit(&s_requested_water_preset,
                          BW_METAL_WATER_RETAIL, memory_order_release);
    s_pipelines = nil;
    s_clear_pipelines = nil;
    s_color_surfaces = nil;
    s_textures = nil;
    s_depth_surfaces = nil;
    s_depth_states = nil;
    for (unsigned i = 0; i < 4; i++)
        s_clear_depth_states[i] = nil;
    s_vertex_function = nil;
    s_color_function = nil;
    s_texture_function = nil;
    s_extended_vertex_function = nil;
    s_extended_fragment_function = nil;
    s_realistic_water_fragment_function = nil;
    s_clear_vertex_function = nil;
    s_clear_color_function = nil;
    s_clear_depth_function = nil;
    s_present_vertex_function = nil;
    s_present_resolve_function = nil;
    s_present_fragment_function = nil;
    s_scale_function = nil;
    s_box_function = nil;
    s_shader_library = nil;
    s_queue = nil;
    s_device = nil;
    s_present_layer = nil;
    atomic_store_explicit(&s_present_width, 0, memory_order_release);
    atomic_store_explicit(&s_present_height, 0, memory_order_release);
    s_guest = 0;
    memset(s_guest_regions, 0, sizeof s_guest_regions);
    memset(&s_current, 0, sizeof s_current);
}

#ifdef BW_METAL_TESTING
void bw_metal_test_set_async_display(int enabled, int defer_retirement)
{
    s_test_async_display = enabled != 0;
    s_test_defer_retirement = defer_retirement != 0;
}

void bw_metal_test_fail_next_submission(void)
{
    s_test_fail_next_submission = 1;
}

void bw_metal_test_async_counts(size_t *depth, unsigned *high_water,
                                uint64_t *saturation_waits,
                                uint64_t *publications,
                                uint64_t *display_frames)
{
    if (depth)
        *depth = s_inflight_submissions.count;
    if (high_water)
        *high_water = s_queue_depth_high_water;
    if (saturation_waits)
        *saturation_waits = s_queue_saturation_waits;
    if (publications)
        *publications = s_guest_publications;
    if (display_frames)
        *display_frames = s_async_display_frames;
}

int bw_metal_test_publish_snapshot(uint32_t color_va, uint32_t pitch)
{
    return metal_publish_snapshot(color_va, pitch);
}

int bw_metal_test_drain_submissions(void)
{
    s_test_defer_retirement = 0;
    return flush_commands();
}

int bw_metal_test_prepare_surface(const Nv2aSurface *surface)
{
    @autoreleasepool {
        return prepare_surface(surface);
    }
}

int bw_metal_test_prepare_present_surface(const Nv2aSurface *surface, uint32_t format)
{
    @autoreleasepool {
        if (!writeback())
            return 0;
        if (!prepare_present_surface_texture(surface, format))
            return 0;
        return flush_commands();
    }
}

uint64_t bw_metal_test_surface_seed_count(void)
{
    return s_surface_seed_count;
}

void bw_metal_test_feedback_counts(uint64_t *checks, uint64_t *gpu_private,
                                   uint64_t *fallbacks, uint64_t *publications,
                                   uint64_t *preparations, uint64_t *bytes)
{
    if (checks)
        *checks = s_feedback_checks;
    if (gpu_private)
        *gpu_private = s_gpu_private_feedback_resolves;
    if (fallbacks)
        *fallbacks = s_feedback_fallbacks;
    if (publications)
        *publications = s_feedback_publications;
    if (preparations)
        *preparations = s_quincunx_preparations;
    if (bytes)
        *bytes = s_quincunx_preparation_bytes;
}

unsigned bw_metal_test_sampler_anisotropy(int min_linear, unsigned mip_mode)
{
    return sampler_anisotropy(min_linear, mip_mode);
}

void bw_metal_test_apply_pending_render_settings(void)
{
    apply_pending_render_settings();
}

unsigned bw_metal_test_active_water_preset(void)
{
    return s_active_water_preset;
}

void bw_metal_test_realistic_water_counts(uint64_t *enhanced, uint64_t *fallback)
{
    if (enhanced)
        *enhanced = s_realistic_water_batches;
    if (fallback)
        *fallback = s_realistic_water_fallback_batches;
}

void bw_metal_test_water_reflect_counts(uint64_t *copies, uint64_t *primes,
                                        uint64_t *filtered)
{
    if (copies)
        *copies = s_water_reflect_copies;
    if (primes)
        *primes = s_water_reflect_prime_draws;
    if (filtered)
        *filtered = s_water_reflect_filtered_draws;
}

void bw_metal_test_water_reflect_fail(int fail)
{
    s_water_reflect_test_failure = fail != 0;
}

void bw_metal_test_water_benchmark(int *complete, size_t *retail_samples,
                                   size_t *realistic_samples)
{
    if (complete)
        *complete = s_water_benchmark_complete;
    if (retail_samples)
        *retail_samples =
            s_water_benchmark_sample_count[BW_METAL_WATER_RETAIL];
    if (realistic_samples)
        *realistic_samples =
            s_water_benchmark_sample_count[BW_METAL_WATER_REALISTIC];
}

void bw_metal_test_water_capture(int *complete, unsigned *images,
                                 unsigned *supported_frames)
{
    if (complete)
        *complete = s_water_capture_complete;
    if (images)
        *images = s_water_capture_images;
    if (supported_frames)
        *supported_frames = s_water_capture_supported_frames;
}

void bw_metal_test_pipeline_diagnostic_snapshot(
    BwMetalPipelineDiagnosticSnapshot *snapshot)
{
    if (!snapshot)
        return;
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->enabled = s_pipeline_diagnostic.enabled;
    snapshot->event_count = s_pipeline_diagnostic.event_count;
    snapshot->event_capacity = METAL_PIPELINE_EVENT_CAPACITY;
    snapshot->events_dropped = s_pipeline_diagnostic.events_dropped;
    snapshot->cache_hits = s_pipeline_diagnostic.cache_hits;
    snapshot->cache_misses = s_pipeline_diagnostic.cache_misses;
    snapshot->whole_cache_resets =
        s_pipeline_diagnostic.whole_cache_resets;
    snapshot->cache_entry_high_water =
        s_pipeline_diagnostic.cache_entry_high_water;
    snapshot->library_artifact_available =
        BW_METAL_OFFLINE_SHADER_LIBRARIES;
    snapshot->library_artifact_loaded =
        s_pipeline_diagnostic.library_artifact_loaded;
    snapshot->library_load_attempts =
        s_pipeline_diagnostic.library_load.attempts;
    snapshot->library_load_timed =
        s_pipeline_diagnostic.library_load.timed;
    snapshot->library_fallbacks =
        s_pipeline_diagnostic.library_fallbacks;
    snapshot->library_unavailable =
        s_pipeline_diagnostic.library_unavailable;
    snapshot->library_rejections =
        s_pipeline_diagnostic.library_rejections;
    snapshot->library_attempts = s_pipeline_diagnostic.library.attempts;
    snapshot->library_timed = s_pipeline_diagnostic.library.timed;
    snapshot->specialization_attempts =
        s_pipeline_diagnostic.specialization.attempts;
    snapshot->specialization_timed =
        s_pipeline_diagnostic.specialization.timed;
    snapshot->pipeline_attempts = s_pipeline_diagnostic.pipeline.attempts;
    snapshot->pipeline_timed = s_pipeline_diagnostic.pipeline.timed;
    snapshot->timing_unavailable =
        s_pipeline_diagnostic.timing_unavailable;
    snapshot->timing_non_monotonic =
        s_pipeline_diagnostic.timing_non_monotonic;
    snapshot->wall_timing_unavailable =
        s_pipeline_diagnostic.wall_timing_unavailable;
    snapshot->wall_timing_non_monotonic =
        s_pipeline_diagnostic.wall_timing_non_monotonic;
    snapshot->allocation_failures =
        s_pipeline_diagnostic.allocation_failures;
    snapshot->metal_failures = s_pipeline_diagnostic.metal_failures;
    snapshot->wall_frame_intervals =
        s_pipeline_diagnostic.wall_frame_intervals;
    snapshot->wall_frame_max_ns =
        s_pipeline_diagnostic.wall_frame_max_ns;
    snapshot->wall_frame_max_frame =
        s_pipeline_diagnostic.wall_frame_max_frame;
    for (size_t i = 0; i < s_pipeline_diagnostic.event_count; i++) {
        const MetalPipelineEvent *event = &s_pipeline_diagnostic.events[i];
        if (event->wall_timing_status == METAL_PIPELINE_TIMING_OK &&
            event->frame == s_pipeline_diagnostic.wall_frame_max_frame &&
            event->wall_frame_ns == s_pipeline_diagnostic.wall_frame_max_ns)
            snapshot->events_on_wall_max++;
    }
}

void bw_metal_test_pipeline_diagnostic_clear(void)
{
    int enabled = s_pipeline_diagnostic.enabled;
    pipeline_diagnostic_reset(enabled);
}

void bw_metal_test_pipeline_diagnostic_inject_cache_miss(uint64_t key_hash,
                                                         uint32_t frame)
{
    if (!s_pipeline_diagnostic.enabled)
        return;
    s_pipeline_diagnostic.cache_misses++;
    pipeline_diagnostic_record_instant(
        METAL_PIPELINE_EVENT_CACHE_MISS, key_hash, frame,
        METAL_PIPELINE_ROLE_RENDER, METAL_PIPELINE_STATUS_OK);
}

void bw_metal_test_fail_next_pipeline_allocation(void)
{
    s_test_fail_next_pipeline_allocation = 1;
}

void bw_metal_test_shader_library_failures(int offline_library,
                                           int source_library)
{
    s_test_fail_offline_library = offline_library != 0;
    s_test_fail_source_library = source_library != 0;
}

const char *bw_metal_test_shader_source_sha256(void)
{
    return s_metal_shader_source_sha256;
}

int bw_metal_test_pipeline_diagnostic_format_event(char *buffer, size_t size)
{
    MetalPipelineEvent event = {
        .key_hash = UINT64_C(0x0123456789abcdef),
        .duration_ns = 1234,
        .wall_frame_ns = 16666667,
        .frame = 42,
        .kind = METAL_PIPELINE_EVENT_FUNCTION_SPECIALIZATION,
        .role = METAL_PIPELINE_ROLE_RENDER,
        .status = METAL_PIPELINE_STATUS_OK,
        .timing_status = METAL_PIPELINE_TIMING_OK,
        .wall_timing_status = METAL_PIPELINE_TIMING_OK,
    };
    uint64_t saved_max_ns = s_pipeline_diagnostic.wall_frame_max_ns;
    uint32_t saved_max_frame = s_pipeline_diagnostic.wall_frame_max_frame;
    s_pipeline_diagnostic.wall_frame_max_ns = event.wall_frame_ns;
    s_pipeline_diagnostic.wall_frame_max_frame = event.frame;
    int result = pipeline_diagnostic_format_event(&event, 7, buffer, size);
    s_pipeline_diagnostic.wall_frame_max_ns = saved_max_ns;
    s_pipeline_diagnostic.wall_frame_max_frame = saved_max_frame;
    return result;
}

int bw_metal_test_pipeline_diagnostic_format_summary(char *buffer, size_t size)
{
    return pipeline_diagnostic_format_summary(buffer, size);
}
#endif
