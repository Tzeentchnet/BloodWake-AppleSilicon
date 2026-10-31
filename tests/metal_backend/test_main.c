#include <stddef.h>
#include <stdint.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "bw_metal_backend.h"
#include "bw_backend_sync.h"
#include "bw_gpu_audit.h"
#include "d3d/d3d8_swizzle.h"

#define WIDTH 16u
#define HEIGHT 12u
#define PITCH 80u
#define AA_WIDTH (WIDTH * 2u)
#define AA_PITCH (AA_WIDTH * 4u + 16u)
#define COLOR_AT 0x10000u
#define POS_AT 0x20000u
#define DIFF_AT 0x21000u
#define UV_AT 0x22000u
#define TEX_AT 0x30000u
#define ZETA_AT 0x38000u
#define ZETA2_AT 0x39000u
#define ZETA3_AT 0x3A000u
#define MEM_BYTES 0x40000u

static _Alignas(16) uint8_t s_mem[MEM_BYTES];
static uint8_t *s_guest_base = s_mem;
static void *s_runtime_base, *s_runtime_contig;
static size_t s_runtime_low_size, s_runtime_contig_size;
static uint32_t s_runtime_contig_va;
static uint32_t s_contig_high;
static uint32_t s_ab_frame;
static uint32_t s_fence_va;
static int s_fence_register_enabled;
static volatile uint32_t s_fence_register;
static int s_failed;

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_guest_base; }
void *xbox_GetMemoryBase(void) { return s_runtime_base; }
size_t xbox_GetMappedSize(void) { return s_runtime_low_size; }
int bw_xbox_contiguous_memory_region(void **host, uint32_t *guest_va, size_t *bytes)
{
    if (!s_runtime_contig)
        return 0;
    *host = s_runtime_contig;
    *guest_va = s_runtime_contig_va;
    *bytes = s_runtime_contig_size;
    return 1;
}

enum { EVENT_SYNC = 1, EVENT_PRESENT, EVENT_FLIP, EVENT_DESCRIPTOR };
static int s_events[32], s_event_count, s_capture_present;
static uint32_t s_present_va, s_present_pitch, s_present_pixel, s_present_aa;
static uint32_t s_present_probe_pixel, s_framebuffer_probe_pixel;
static unsigned s_probe_x, s_probe_y;
static int s_capture_probe;
static int s_present_count;
static BwMetalFramebuffer s_framebuffer_info;
static unsigned s_framebuffer_info_count;
static uint32_t s_framebuffer_info_pixel;
static int s_framebuffer_info_accept = 1;

static void event(int value)
{
    if (s_event_count < (int)(sizeof s_events / sizeof s_events[0]))
        s_events[s_event_count++] = value;
}

static int capture_framebuffer_info(const BwMetalFramebuffer *framebuffer)
{
    s_framebuffer_info = *framebuffer;
    s_framebuffer_info_count++;
    memcpy(&s_framebuffer_info_pixel, (void *)((uintptr_t)s_guest_base + framebuffer->guest_va),
           sizeof s_framebuffer_info_pixel);
    if (s_capture_probe) {
        if (framebuffer->width <= s_probe_x || framebuffer->height <= s_probe_y) {
            fprintf(stderr, "Framebuffer probe is outside the descriptor\n");
            s_failed = 1;
            s_capture_probe = 0;
        } else {
            memcpy(&s_framebuffer_probe_pixel,
                   (void *)((uintptr_t)s_guest_base + framebuffer->guest_va +
                            (size_t)s_probe_y * framebuffer->pitch + s_probe_x * 4u),
                   sizeof s_framebuffer_probe_pixel);
        }
    }
    event(EVENT_DESCRIPTOR);
    return s_framebuffer_info_accept;
}

uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return s_contig_high; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { s_present_aa = sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return s_fence_va; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    if (s_capture_present) {
        event(EVENT_PRESENT);
        s_present_count++;
        s_present_va = va;
        s_present_pitch = pitch;
        memcpy(&s_present_pixel, (void *)((uintptr_t)s_guest_base + va), sizeof s_present_pixel);
        if (s_capture_probe)
            memcpy(&s_present_probe_pixel,
                   (void *)((uintptr_t)s_guest_base + va +
                            (size_t)s_probe_y * pitch + s_probe_x * 4u),
                   sizeof s_present_probe_pixel);
    }
}
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    return s_fence_register_enabled && offset == 0x400B10u ? &s_fence_register : NULL;
}
int bw_ab_off(const char *fix) { (void)fix; return 0; }
void bw_ab_frame(uint32_t frame) { s_ab_frame = frame; }
int bw_audit_on(void) { return 0; }
void bw_audit_method(uint32_t subch, uint32_t method, uint32_t param)
{ (void)subch; (void)method; (void)param; }
void bw_audit_draw(const BwAuditDraw *draw) { (void)draw; }
void bw_audit_clear(uint32_t flags, uint32_t width, uint32_t height, const uint32_t *rect)
{ (void)flags; (void)width; (void)height; (void)rect; }
void bw_audit_flip(uint32_t offset, uint32_t format) { (void)offset; (void)format; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern int bw_metal_test_read_depth(uint32_t va, uint32_t x, uint32_t y,
                                    float *z, uint8_t *stencil);
extern int bw_metal_test_read_depth_surface(uint32_t va, float *z, uint8_t *stencil,
                                            size_t count);
extern void bw_metal_test_draw_extended(const Nv2aSurface *surface, const BwXBatch *batch);
extern size_t bw_metal_test_color_surface_count(void);
extern uint64_t bw_metal_test_command_submission_count(void);
extern uint64_t bw_metal_test_draw_encoder_creation_count(void);
extern uint64_t bw_metal_test_vertex_upload_allocation_count(void);
extern uint64_t bw_metal_test_texture_upload_count(void);

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        s_failed = 1; \
    } \
} while (0)

static void method(uint32_t method_number, uint32_t value)
{
    nv2a_pb_exec_method(0, method_number, value);
}

static void method_float(uint32_t method_number, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    method(method_number, bits);
}

static void put32(uint32_t address, uint32_t value)
{
    memcpy((void *)((uintptr_t)s_guest_base + address), &value, sizeof value);
}

static void semaphore_release_test(void)
{
    const uint32_t target = 0x1000u, initial = 0xA1B2C3D4u, marker = 0xABCDEF03u;
    uint32_t actual;

    put32(target, initial);
    nv2a_pb_set_semaphore_target(0);
    nv2a_pb_exec_method(0, 0x1D70u, 0x11111111u);
    memcpy(&actual, s_mem + target, sizeof actual);
    CHECK(actual == initial);

    nv2a_pb_set_semaphore_target(target);
    nv2a_pb_exec_method(1, 0x1D70u, 0x22222222u);
    memcpy(&actual, s_mem + target, sizeof actual);
    CHECK(actual == initial);
    nv2a_pb_exec_method(0, 0x1D70u, 0x12345678u);
    memcpy(&actual, s_mem + target, sizeof actual);
    CHECK(actual == 0x12345678u);

    nv2a_pb_set_semaphore_target(0);
    nv2a_pb_exec_method(0, 0x1D70u, 0x33333333u);
    memcpy(&actual, s_mem + target, sizeof actual);
    CHECK(actual == 0x12345678u);

    s_fence_register_enabled = 1;
    s_fence_va = 0x3000u;
    put32(s_fence_va, 0x1Au);
    nv2a_pb_exec_method(5, 0x0310u, marker);
    CHECK(s_fence_register == ((marker & ~0x7Cu) | (0x1Au << 2)));
    nv2a_pb_set_semaphore_target(target);
    nv2a_pb_exec_method(5, 0x0310u, marker);
    CHECK(s_fence_register == marker);
    nv2a_pb_set_semaphore_target(0);
}

static atomic_int s_sem_ready, s_sem_stop;

static void *publish_semaphore_targets(void *unused)
{
    (void)unused;
    nv2a_pb_set_semaphore_target(0x1000u);
    atomic_store(&s_sem_ready, 1);
    while (!atomic_load(&s_sem_stop)) {
        nv2a_pb_set_semaphore_target(0);
        nv2a_pb_set_semaphore_target(0x1000u);
        nv2a_pb_set_semaphore_target(0x2000u);
    }
    return NULL;
}

static void semaphore_concurrent_test(void)
{
    const uint32_t marker = 0xABCDEF03u, release = 0x12345678u;
    const uint32_t mirrored = (marker & ~0x7Cu) | (0x1Au << 2);
    pthread_t publisher;
    uint32_t first, second;

    s_fence_register_enabled = 1;
    s_fence_va = 0x3000u;
    put32(s_fence_va, 0x1Au);
    if (pthread_create(&publisher, NULL, publish_semaphore_targets, NULL) != 0) {
        CHECK(0 && "cannot start semaphore target publisher");
        return;
    }
    while (!atomic_load(&s_sem_ready))
        sched_yield();
    for (unsigned i = 0; i < 50000; i++) {
        nv2a_pb_exec_method(0, 0x1D70u, release);
        nv2a_pb_exec_method(5, 0x0310u, marker);
        if (s_fence_register != marker && s_fence_register != mirrored) {
            CHECK(0 && "semaphore marker used an inconsistent target");
            break;
        }
    }
    atomic_store(&s_sem_stop, 1);
    pthread_join(publisher, NULL);
    nv2a_pb_set_semaphore_target(0);
    memcpy(&first, s_mem + 0x1000u, sizeof first);
    memcpy(&second, s_mem + 0x2000u, sizeof second);
    CHECK((first == release || second == release) &&
          (first == 0 || first == release) && (second == 0 || second == release));
}

static void put_float(uint32_t address, float value)
{
    memcpy((void *)((uintptr_t)s_guest_base + address), &value, sizeof value);
}

static uint32_t surface_pixel(uint32_t address, uint32_t pitch, unsigned x, unsigned y)
{
    uint32_t value;
    memcpy(&value, s_mem + address + y * pitch + x * 4u, sizeof value);
    return value;
}

static uint32_t pixel(unsigned x, unsigned y)
{
    return surface_pixel(COLOR_AT, PITCH, x, y);
}

static uint32_t aa_pixel(unsigned x, unsigned y)
{
    uint32_t value;
    memcpy(&value, s_mem + COLOR_AT + y * AA_PITCH + x * 4u, sizeof value);
    return value;
}

static void setup_xrgb_aa_surface(void)
{
    method(0x0208, 0x1124);
    method(0x020C, (AA_PITCH << 16) | AA_PITCH);
    method(0x0210, COLOR_AT);
}

static void setup_xrgb_single_surface(uint32_t address)
{
    method(0x0208, 0x0104);
    method(0x020C, PITCH);
    method(0x0210, address);
}

static void setup_surface(int fixed_function)
{
    static const float identity[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1
    };
    method(0x0208, 0x0008);
    method(0x0200, WIDTH << 16);
    method(0x0204, HEIGHT << 16);
    method(0x020C, PITCH);
    method(0x0210, COLOR_AT);
    if (fixed_function)
        for (unsigned i = 0; i < 16; i++)
            method_float(0x0680 + 4u * i, identity[i]);
    method(0x1E94, 0);
    method_float(0x0394, 0.0f);
    method_float(0x0398, 1.0f);
    method(0x1720, POS_AT);
    method(0x1760, 2 | (3 << 4) | (12 << 8));
    method(0x172C, DIFF_AT);
    method(0x176C, 0 | (4 << 4) | (4 << 8));
}

static void vertex(unsigned n, float x, float y, uint32_t color, float u, float v)
{
    put_float(POS_AT + n * 12u, x);
    put_float(POS_AT + n * 12u + 4u, y);
    put_float(POS_AT + n * 12u + 8u, 0.5f);
    put32(DIFF_AT + n * 4u, color);
    put_float(UV_AT + n * 8u, u);
    put_float(UV_AT + n * 8u + 4u, v);
}

static void draw(unsigned count)
{
    method(0x17FC, 5);
    method(0x1810, (count - 1u) << 24);
    method(0x17FC, 0);
}

static void clear(uint32_t color)
{
    method(0x1D90, color);
    method(0x1D94, 0xF0);
}

static void flip(void)
{
    method(0x0130, 0);
}

static void triangle(uint32_t color)
{
    vertex(0, 2, 2, color, 0, 0);
    vertex(1, 14, 2, color, 0, 0);
    vertex(2, 2, 10, color, 0, 0);
}

static void triangle_z(uint32_t color, float z)
{
    triangle(color);
    for (unsigned i = 0; i < 3; i++)
        put_float(POS_AT + i * 12u + 8u, z);
}

static void setup_texture(uint32_t address, uint32_t filter)
{
    method(0x1B00, address);
    method(0x1B04, 0x00101200);
    method(0x1B08, 0x303);
    method(0x1B10, 8u << 16);
    method(0x1B14, filter);
    method(0x1B1C, (2u << 16) | 2u);
    method(0x1744, UV_AT);
    method(0x1784, 2 | (2 << 4) | (8 << 8));
}

static void quad(void)
{
    const uint32_t white = 0xFFFFFFFFu;
    vertex(0, 2, 2, white, -1, -1);
    vertex(1, 14, 2, white, 3, -1);
    vertex(2, 2, 10, white, -1, 3);
    vertex(3, 14, 2, white, 3, -1);
    vertex(4, 14, 10, white, 3, 3);
    vertex(5, 2, 10, white, -1, 3);
}

static void expect_no_error(void)
{
    const char *error = bw_metal_backend_error();
    if (error) {
        fprintf(stderr, "Unexpected Metal error: %s\n", error);
        s_failed = 1;
    }
}

static void expect_error(const char *text, const uint8_t *before)
{
    const char *error = bw_metal_backend_error();
    if (!error || !strstr(error, text)) {
        fprintf(stderr, "Expected Metal error containing '%s'; got '%s'\n",
                text, error ? error : "(none)");
        s_failed = 1;
    }
    flip();
    CHECK(memcmp(before, s_mem + COLOR_AT, PITCH * HEIGHT) == 0);
}

static void visual_test(void)
{
    clear(0x7F102030u);
    triangle(0xC0CE3721u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0x7F102030u);
    CHECK(pixel(4, 3) == 0xC0CE3721u);
    CHECK(pixel(13, 9) == 0x7F102030u);
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = WIDTH * 4; x < PITCH; x++)
            CHECK(s_mem[COLOR_AT + y * PITCH + x] == 0xA5);

    put32(TEX_AT, 0xFFFF0000u);
    put32(TEX_AT + 4, 0xFF00FF00u);
    put32(TEX_AT + 8, 0xFF0000FFu);
    put32(TEX_AT + 12, 0xFFFFFFFFu);
    setup_texture(TEX_AT, 0x01010000u);
    quad();
    clear(0xFF010203u);
    draw(6);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0xFF010203u);
    CHECK(pixel(4, 3) == 0xFFFF0000u);
    CHECK(pixel(11, 3) == 0xFF00FF00u);
    CHECK(pixel(4, 8) == 0xFF0000FFu);
    CHECK(pixel(11, 8) == 0xFFFFFFFFu);
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = WIDTH * 4; x < PITCH; x++)
            CHECK(s_mem[COLOR_AT + y * PITCH + x] == 0xA5);
}

static void seed_order_test(void)
{
    put32(COLOR_AT, 0xFF124689u);
    triangle(0xFFEE1100u);
    draw(3);
    triangle(0xFF11CC44u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0xFF124689u);
    CHECK(pixel(4, 3) == 0xFF11CC44u);
    CHECK(pixel(13, 9) == 0xA5A5A5A5u);
}

static void command_batching_test(void)
{
    uint64_t submissions = bw_metal_test_command_submission_count();
    uint64_t encoders = bw_metal_test_draw_encoder_creation_count();
    uint64_t uploads = bw_metal_test_vertex_upload_allocation_count();
    clear(0xFF102030u);
    triangle(0xFFEE1100u);
    draw(3);
    triangle(0xFF11CC44u);
    draw(3);
    CHECK(bw_metal_test_command_submission_count() == submissions);
    CHECK(bw_metal_test_draw_encoder_creation_count() == encoders + 1u);
    CHECK(bw_metal_test_vertex_upload_allocation_count() == uploads + 1u);
    flip();
    expect_no_error();
    CHECK(bw_metal_test_command_submission_count() == submissions + 1u);
    CHECK(pixel(4, 3) == 0xFF11CC44u);
}

static void vertex4(unsigned n, float x, float y, float w, uint32_t color)
{
    put_float(POS_AT + n * 16u, x * w);
    put_float(POS_AT + n * 16u + 4u, y * w);
    put_float(POS_AT + n * 16u + 8u, 0.5f * w);
    put_float(POS_AT + n * 16u + 12u, w);
    put32(DIFF_AT + n * 4u, color);
}

static int color_near(uint32_t actual, uint32_t expected, int tolerance)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        int delta = (int)((actual >> shift) & 255u) - (int)((expected >> shift) & 255u);
        if (delta < -tolerance || delta > tolerance)
            return 0;
    }
    return 1;
}

static void check_pixel_near(uint32_t expected, int tolerance, const char *label,
                             uint32_t value)
{
    uint32_t actual = pixel(4, 3);
    if (!color_near(actual, expected, tolerance)) {
        fprintf(stderr, "%s 0x%08x: expected %08x, got %08x\n",
                label, value, expected, actual);
        s_failed = 1;
    }
}

static void blend_state(uint32_t src, uint32_t dst, uint32_t equation, uint32_t constant)
{
    method(0x0304, 1);
    method(0x0344, src);
    method(0x0348, dst);
    method(0x034C, constant);
    method(0x0350, equation);
}

static float reference_factor(uint32_t factor, unsigned channel,
                              const float src[4], const float dst[4], const float constant[4])
{
    switch (factor) {
    case 0: return 0;
    case 1: return 1;
    case 0x300: return src[channel];
    case 0x301: return 1 - src[channel];
    case 0x302: return src[3];
    case 0x303: return 1 - src[3];
    case 0x304: return dst[3];
    case 0x305: return 1 - dst[3];
    case 0x306: return dst[channel];
    case 0x307: return 1 - dst[channel];
    case 0x308: return channel == 3 ? 1 : fminf(src[3], 1 - dst[3]);
    case 0x8001: return constant[channel];
    case 0x8002: return 1 - constant[channel];
    case 0x8003: return constant[3];
    case 0x8004: return 1 - constant[3];
    default: abort();
    }
}

static uint32_t reference_blend(uint32_t source, uint32_t destination, uint32_t constant,
                                uint32_t src_factor, uint32_t dst_factor, uint32_t equation)
{
    const unsigned shifts[4] = {16, 8, 0, 24};
    float src[4], dst[4], color[4];
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; i++) {
        src[i] = ((source >> shifts[i]) & 255u) / 255.0f;
        dst[i] = ((destination >> shifts[i]) & 255u) / 255.0f;
        color[i] = ((constant >> shifts[i]) & 255u) / 255.0f;
    }
    for (unsigned i = 0; i < 4; i++) {
        float s = src[i] * reference_factor(src_factor, i, src, dst, color);
        float d = dst[i] * reference_factor(dst_factor, i, src, dst, color);
        float value;
        switch (equation) {
        case 0:
        case 0x8006: value = s + d; break;
        case 0x8007: value = fminf(src[i], dst[i]); break;
        case 0x8008: value = fmaxf(src[i], dst[i]); break;
        case 0x800A: value = s - d; break;
        case 0x800B: value = d - s; break;
        default: abort();
        }
        value = fmaxf(0, fminf(1, value));
        result |= (uint32_t)lroundf(value * 255) << shifts[i];
    }
    return result;
}

static void blend_test(void)
{
    static const uint32_t factors[] = {
        0, 1, 0x300, 0x301, 0x302, 0x303, 0x304, 0x305, 0x306, 0x307,
        0x308, 0x8001, 0x8002, 0x8003, 0x8004
    };
    static const uint32_t equations[] = {0, 0x8006, 0x8007, 0x8008, 0x800A, 0x800B};
    const uint32_t source = 0x8064C832u, destination = 0xB0402080u, constant = 0x604080C0u;
    for (unsigned i = 0; i < sizeof factors / sizeof factors[0]; i++) {
        blend_state(factors[i], 0, 0x8006, constant);
        clear(destination);
        triangle(source);
        draw(3);
        flip();
        expect_no_error();
        check_pixel_near(reference_blend(source, destination, constant, factors[i], 0, 0x8006),
                         1, "source blend factor", factors[i]);
        if (factors[i] == 0x308)
            continue;
        blend_state(0, factors[i], 0x8006, constant);
        clear(destination);
        draw(3);
        flip();
        expect_no_error();
        check_pixel_near(reference_blend(source, destination, constant, 0, factors[i], 0x8006),
                         1, "destination blend factor", factors[i]);
    }
    for (unsigned i = 0; i < sizeof equations / sizeof equations[0]; i++) {
        blend_state(0x302, 0x303, equations[i], constant);
        clear(destination);
        draw(3);
        flip();
        expect_no_error();
        check_pixel_near(reference_blend(source, destination, constant, 0x302, 0x303, equations[i]),
                         1, "blend equation", equations[i]);
    }
    blend_state(0x8001, 0x8004, 0x8006, constant);
    clear(destination);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(reference_blend(source, destination, constant, 0x8001, 0x8004, 0x8006),
                     1, "mixed constant factors", constant);
    method(0x0304, 0);
    method(0x0344, 0xFFFF);
    method(0x0350, 0xFFFF);
    clear(destination);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(source, 0, "disabled blend state", 0);
}

static void color_mask_test(void)
{
    const uint32_t source = 0x12345678u, destination = 0xA1B2C3D4u;
    const uint32_t nv_bits[] = {0x00010000u, 0x00000100u, 0x00000001u, 0x01000000u};
    const unsigned shifts[] = {16, 8, 0, 24};
    for (unsigned mask = 0; mask < 16; mask++) {
        uint32_t nv_mask = 0, expected = destination;
        for (unsigned i = 0; i < 4; i++) {
            if (mask & (1u << i)) {
                nv_mask |= nv_bits[i];
                expected = (expected & ~(255u << shifts[i])) | (source & (255u << shifts[i]));
            }
        }
        method(0x0358, nv_mask);
        clear(destination);
        triangle(source);
        draw(3);
        flip();
        expect_no_error();
        check_pixel_near(expected, 0, "colour mask", nv_mask);
    }
    method(0x0358, 0x00010001u);
    blend_state(0x302, 0x303, 0x8006, 0);
    clear(destination);
    triangle(source);
    draw(3);
    flip();
    expect_no_error();
    uint32_t blended = reference_blend(source, destination, 0, 0x302, 0x303, 0x8006);
    check_pixel_near((destination & 0xFF00FF00u) | (blended & 0x00FF00FFu),
                     1, "blended colour mask", 0x00010001u);
}

static void pipeline_cache_test(void)
{
    const uint32_t factors[] = {0, 1, 0x300, 0x301, 0x302, 0x303, 0x304, 0x305, 0x306};
    const uint32_t nv_bits[] = {0x00010000u, 0x00000100u, 0x00000001u, 0x01000000u};
    const uint32_t source = 0x8064C832u, destination = 0xB0402080u;
    triangle(source);
    for (unsigned i = 0; i < 129; i++) {
        unsigned mask = (i + 15u) % 16u;
        uint32_t nv_mask = 0;
        for (unsigned j = 0; j < 4; j++)
            if (mask & (1u << j))
                nv_mask |= nv_bits[j];
        method(0x0358, nv_mask);
        blend_state(factors[i / 16u], 1, 0x8006, 0);
        clear(destination);
        draw(3);
    }
    method(0x0358, 0x01010101u);
    blend_state(0, 1, 0x8006, 0);
    clear(destination);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(destination, 0, "pipeline cache eviction", 0);
}

static void cull_test(void)
{
    const uint32_t source = 0xFFABCDEFu, destination = 0xFF123456u;
    const uint32_t faces[] = {0x404, 0x405, 0x408};
    method(0x0308, 1);
    for (uint32_t front = 0x900; front <= 0x901; front++) {
        method(0x03A0, front);
        for (unsigned f = 0; f < 3; f++) {
            method(0x039C, faces[f]);
            for (unsigned ccw = 0; ccw < 2; ccw++) {
                vertex(0, 2, 2, source, 0, 0);
                vertex(1, ccw ? 2 : 14, ccw ? 10 : 2, source, 0, 0);
                vertex(2, ccw ? 14 : 2, ccw ? 2 : 10, source, 0, 0);
                int is_front = (front == 0x901) == (ccw != 0);
                int visible = faces[f] != 0x408 &&
                              (faces[f] == 0x404 ? !is_front : is_front);
                clear(destination);
                draw(3);
                flip();
                expect_no_error();
                check_pixel_near(visible ? source : destination, 0, "cull state",
                                 (front << 16) | (faces[f] << 1) | ccw);
            }
        }
    }
    method(0x0308, 0);
    method(0x039C, 0xFFFF);
    method(0x03A0, 0xFFFF);
    clear(destination);
    triangle(source);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(source, 0, "disabled cull state", 0);
}

static int expected_alpha_pass(uint32_t function, uint32_t reference)
{
    switch (function) {
    case 0x200: return 0;
    case 0x201: return 128 < reference;
    case 0x202: return 128 == reference;
    case 0x203: return 128 <= reference;
    case 0x204: return 128 > reference;
    case 0x205: return 128 != reference;
    case 0x206: return 128 >= reference;
    case 0x207: return 1;
    default: abort();
    }
}

static void alpha_test(void)
{
    const uint32_t source = 0x80A0C0E0u, destination = 0xFF123456u;
    method(0x0300, 1);
    for (uint32_t function = 0x200; function <= 0x207; function++) {
        for (uint32_t reference = 127; reference <= 129; reference++) {
            method(0x033C, function);
            method(0x0340, reference);
            clear(destination);
            triangle(source);
            draw(3);
            flip();
            expect_no_error();
            check_pixel_near(expected_alpha_pass(function, reference) ? source : destination,
                             0, "alpha comparison", (function << 16) | reference);
        }
    }
    vertex(0, 2, 2, 0x80FFFFFFu, 0, 0);
    vertex(1, 14, 2, 0x82FFFFFFu, 0, 0);
    vertex(2, 2, 10, 0x80FFFFFFu, 0, 0);
    method(0x0340, 128);
    for (uint32_t function = 0x202; function <= 0x205; function++) {
        method(0x033C, function);
        clear(destination);
        draw(3);
        flip();
        expect_no_error();
        check_pixel_near(function == 0x203 || function == 0x205 ? destination : 0x80FFFFFFu,
                         1, "interpolated alpha comparison", function);
    }
    method(0x0300, 0);
    method(0x033C, 0xFFFF);
    clear(destination);
    triangle(source);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(source, 0, "disabled alpha state", 0);
    method(0x0300, 1);
    for (unsigned i = 0; i < 4; i++)
        put32(TEX_AT + 4u * i, 0x40FFFFFFu);
    setup_texture(TEX_AT, 0x01010000u);
    quad();
    for (unsigned i = 0; i < 6; i++)
        put32(DIFF_AT + 4u * i, 0x80FFFFFFu);
    method(0x033C, 0x204);
    for (unsigned pass = 0; pass < 2; pass++) {
        method(0x0340, pass ? 20u : 40u);
        clear(destination);
        draw(6);
        flip();
        expect_no_error();
        check_pixel_near(pass ? 0x20FFFFFFu : destination, 1, "textured alpha", pass);
    }
    method(0x0358, 0x00010101u);
    blend_state(0x302, 0x303, 0x8006, 0);
    for (unsigned pass = 0; pass < 2; pass++) {
        method(0x0340, pass ? 20u : 40u);
        clear(destination);
        draw(6);
        flip();
        expect_no_error();
        uint32_t blended = reference_blend(0x20FFFFFFu, destination, 0, 0x302, 0x303, 0x8006);
        uint32_t expected = pass ? (destination & 0xFF000000u) | (blended & 0x00FFFFFFu)
                                 : destination;
        check_pixel_near(expected, 1, "textured alpha with blending", pass);
    }
}

static void perspective_test(void)
{
    method(0x1760, 2 | (4 << 4) | (16 << 8));
    vertex4(0, 2, 2, 1, 0xFFFF0000u);
    vertex4(1, 14, 2, 2, 0xFF00FF00u);
    vertex4(2, 2, 10, 1, 0xFF0000FFu);
    clear(0xFF000000u);
    draw(3);
    flip();
    expect_no_error();
    /* At pixel centre (4.5, 3.5), perspective weights are 29:5:9. */
    CHECK(color_near(pixel(4, 3), 0xFFAC1E35u, 2));
}

static void full_rect_test(void)
{
    method(0x1D98, (WIDTH - 1u) << 16);
    method(0x1D9C, (HEIGHT - 1u) << 16);
    clear(0xFF214365u);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0xFF214365u);
    CHECK(pixel(WIDTH - 1u, HEIGHT - 1u) == 0xFF214365u);
}

static void set_clear_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    method(0x1D98, (x1 << 16) | x0);
    method(0x1D9C, (y1 << 16) | y0);
}

static void seed_color_reference(uint32_t expected[HEIGHT][WIDTH])
{
    memset(s_mem + COLOR_AT, 0xA5, PITCH * HEIGHT);
    for (uint32_t y = 0; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++) {
            expected[y][x] = 0x80000000u | ((x * 11u + 3u) << 16) |
                             ((y * 17u + 5u) << 8) | (x + y * WIDTH);
            put32(COLOR_AT + y * PITCH + x * 4u, expected[y][x]);
        }
}

static void update_color_reference(uint32_t expected[HEIGHT][WIDTH], uint32_t flags,
                                   uint32_t color, uint32_t x0, uint32_t y0,
                                   uint32_t x1, uint32_t y1)
{
    uint32_t mask = 0;
    if (flags & 0x10u) mask |= 0x00FF0000u;
    if (flags & 0x20u) mask |= 0x0000FF00u;
    if (flags & 0x40u) mask |= 0x000000FFu;
    if (flags & 0x80u) mask |= 0xFF000000u;
    for (uint32_t y = 0; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++)
            if (x >= x0 && x < x1 && y >= y0 && y < y1)
                expected[y][x] = (expected[y][x] & ~mask) | (color & mask);
}

static void check_color_reference(uint32_t expected[HEIGHT][WIDTH])
{
    unsigned mismatches = 0, padding_mismatches = 0;
    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++)
            if (pixel(x, y) != expected[y][x])
                mismatches++;
        for (uint32_t x = WIDTH * 4u; x < PITCH; x++)
            if (s_mem[COLOR_AT + y * PITCH + x] != 0xA5u)
                padding_mismatches++;
    }
    if (mismatches || padding_mismatches)
        fprintf(stderr, "Color reference mismatches: %u pixels, %u padding bytes\n",
                mismatches, padding_mismatches);
    CHECK(mismatches == 0 && padding_mismatches == 0);
}

static void partial_color_test(void)
{
    uint32_t expected[HEIGHT][WIDTH];
    seed_color_reference(expected);
    method(0x0300, 1);
    method(0x0304, 1);
    method(0x0308, 1);
    method(0x033C, 0x0200u);
    method(0x0358, 0);
    method(0x039C, 0x0408u);
    set_clear_rect(2, 3, 9, 8);
    s_capture_present = 1;
    clear(0x7F112233u);
    update_color_reference(expected, 0xF0u, 0x7F112233u, 2, 3, 10, 9);
    flip();
    expect_no_error();
    check_color_reference(expected);
    CHECK(s_present_count == 1 && s_present_pixel == expected[0][0]);
}

static void partial_color_masks_test(void)
{
    uint32_t expected[HEIGHT][WIDTH];
    const uint32_t color = 0x193B5D7Fu;
    method(0x0358, 0);
    for (uint32_t full = 0; full < 2; full++)
        for (uint32_t mask = 1; mask < 16; mask++) {
            seed_color_reference(expected);
            set_clear_rect(full ? 0u : 2u, full ? 0u : 3u,
                           full ? WIDTH - 1u : 9u, full ? HEIGHT - 1u : 8u);
            method(0x1D90, color);
            method(0x1D94, mask << 4);
            update_color_reference(expected, mask << 4, color,
                                   full ? 0u : 2u, full ? 0u : 3u,
                                   full ? WIDTH : 10u, full ? HEIGHT : 9u);
            flip();
            expect_no_error();
            check_color_reference(expected);
        }
}

static void partial_color_default_rect_test(void)
{
    uint32_t expected[HEIGHT][WIDTH];
    seed_color_reference(expected);
    method(0x1D90, 0x193B5D7Fu);
    method(0x1D94, 0x10u);
    update_color_reference(expected, 0x10u, 0x193B5D7Fu, 0, 0, WIDTH, HEIGHT);
    flip();
    expect_no_error();
    check_color_reference(expected);
}

static void partial_clip_test(void)
{
    uint32_t expected[HEIGHT][WIDTH];
    seed_color_reference(expected);
    set_clear_rect(WIDTH - 2u, HEIGHT - 2u, 0xFFFFu, 0xFFFFu);
    clear(0xFF102030u);
    update_color_reference(expected, 0xF0u, 0xFF102030u,
                           WIDTH - 2u, HEIGHT - 2u, WIDTH, HEIGHT);
    flip();
    expect_no_error();
    check_color_reference(expected);

    set_clear_rect(0, 0, 0xFFFFu, 0xFFFFu);
    clear(0x80406080u);
    update_color_reference(expected, 0xF0u, 0x80406080u, 0, 0, WIDTH, HEIGHT);
    flip();
    expect_no_error();
    check_color_reference(expected);

    set_clear_rect(WIDTH - 1u, HEIGHT - 1u, WIDTH - 1u, HEIGHT - 1u);
    clear(0xC0ABCDEFu);
    update_color_reference(expected, 0xF0u, 0xC0ABCDEFu,
                           WIDTH - 1u, HEIGHT - 1u, WIDTH, HEIGHT);
    flip();
    expect_no_error();
    check_color_reference(expected);
}

static void partial_empty_test(void)
{
    uint32_t expected[HEIGHT][WIDTH];
    const uint32_t rects[][4] = {
        {WIDTH + 1u, 1u, WIDTH + 5u, 3u},
        {1u, HEIGHT + 1u, 3u, HEIGHT + 5u},
        {8u, 1u, 3u, 4u},
        {1u, 8u, 4u, 3u},
    };
    seed_color_reference(expected);
    for (unsigned i = 0; i < sizeof rects / sizeof rects[0]; i++) {
        set_clear_rect(rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
        clear(0xFF102030u);
        flip();
        expect_no_error();
        check_color_reference(expected);
    }
}

static void setup_depth(uint32_t address)
{
    method(0x0208, 0x0128u);     /* pitch layout, Z24S8, X8R8G8B8 */
    method(0x020C, (PITCH << 16) | PITCH);
    method(0x0214, address);
}

static void depth_clear(uint32_t flags, uint32_t value)
{
    method(0x1D8C, value);
    method(0x1D94, flags);
}

static void seed_depth_reference(uint32_t address, uint32_t z[HEIGHT][WIDTH],
                                 uint8_t stencil[HEIGHT][WIDTH])
{
    memset(s_mem + address, 0xA5, PITCH * HEIGHT);
    for (uint32_t y = 0; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t i = y * WIDTH + x;
            z[y][x] = 0x102030u + i * 0x101u;
            stencil[y][x] = (uint8_t)(0xA5u ^ i);
            put32(address + y * PITCH + x * 4u, (z[y][x] << 8) | stencil[y][x]);
        }
}

static void update_depth_reference(uint32_t z[HEIGHT][WIDTH], uint8_t stencil[HEIGHT][WIDTH],
                                   uint32_t flags, uint32_t value, uint32_t x0, uint32_t y0,
                                   uint32_t x1, uint32_t y1)
{
    for (uint32_t y = 0; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++)
            if (x >= x0 && x < x1 && y >= y0 && y < y1) {
                if (flags & 1u) z[y][x] = value >> 8;
                if (flags & 2u) stencil[y][x] = (uint8_t)value;
            }
}

static void check_depth_reference(uint32_t address, uint32_t z[HEIGHT][WIDTH],
                                  uint8_t stencil[HEIGHT][WIDTH])
{
    float actual_z[HEIGHT][WIDTH];
    uint8_t actual_stencil[HEIGHT][WIDTH];
    unsigned mismatches = 0;
    if (!bw_metal_test_read_depth_surface(address, &actual_z[0][0], &actual_stencil[0][0],
                                          WIDTH * HEIGHT)) {
        CHECK(0 && "depth surface readback");
        return;
    }
    for (uint32_t y = 0; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++)
            if (fabsf(actual_z[y][x] - (float)z[y][x] / 16777215.0f) >= 0.0000001f ||
                actual_stencil[y][x] != stencil[y][x])
                mismatches++;
    if (mismatches)
        fprintf(stderr, "Depth/stencil reference mismatches: %u pixels\n", mismatches);
    CHECK(mismatches == 0);
}

static void partial_depth_test(void)
{
    uint32_t color[HEIGHT][WIDTH], z[HEIGHT][WIDTH];
    uint8_t stencil[HEIGHT][WIDTH], original_guest[PITCH * HEIGHT];
    seed_color_reference(color);
    seed_depth_reference(ZETA_AT, z, stencil);
    memcpy(original_guest, s_mem + ZETA_AT, sizeof original_guest);
    setup_depth(ZETA_AT);
    method(0x030C, 1);
    method(0x032C, 1);
    method(0x0358, 0);
    method(0x035C, 0);
    method(0x0360, 0);

    set_clear_rect(2, 3, 11, 8);
    depth_clear(1, 0x23456711u);
    update_depth_reference(z, stencil, 1, 0x23456711u, 2, 3, 12, 9);
    expect_no_error();
    check_depth_reference(ZETA_AT, z, stencil);

    set_clear_rect(4, 1, 15, 6);
    depth_clear(2, 0x876543C4u);
    update_depth_reference(z, stencil, 2, 0x876543C4u, 4, 1, 16, 7);
    expect_no_error();
    check_depth_reference(ZETA_AT, z, stencil);

    set_clear_rect(0, 0, 7, 5);
    method(0x1D90, 0x40102030u);
    depth_clear(0x11u, 0x334455AAu);
    update_color_reference(color, 0x10u, 0x40102030u, 0, 0, 8, 6);
    update_depth_reference(z, stencil, 1, 0x334455AAu, 0, 0, 8, 6);
    expect_no_error();
    check_depth_reference(ZETA_AT, z, stencil);

    set_clear_rect(6, 4, 10, 9);
    method(0x1D90, 0xA1B2C3D4u);
    depth_clear(0x22u, 0x66778899u);
    update_color_reference(color, 0x20u, 0xA1B2C3D4u, 6, 4, 11, 10);
    update_depth_reference(z, stencil, 2, 0x66778899u, 6, 4, 11, 10);
    expect_no_error();
    check_depth_reference(ZETA_AT, z, stencil);

    set_clear_rect(5, 1, 13, 10);
    method(0x1D90, 0xCC345678u);
    depth_clear(0xC3u, 0x22446688u);
    update_color_reference(color, 0xC0u, 0xCC345678u, 5, 1, 14, 11);
    update_depth_reference(z, stencil, 3, 0x22446688u, 5, 1, 14, 11);
    expect_no_error();
    check_depth_reference(ZETA_AT, z, stencil);

    set_clear_rect(10, 8, 0xFFFFu, 0xFFFFu);
    method(0x1D90, 0xFF214365u);
    depth_clear(0xF3u, 0x789ABC65u);
    update_color_reference(color, 0xF0u, 0xFF214365u, 10, 8, WIDTH, HEIGHT);
    update_depth_reference(z, stencil, 3, 0x789ABC65u, 10, 8, WIDTH, HEIGHT);
    flip();
    expect_no_error();
    check_color_reference(color);
    check_depth_reference(ZETA_AT, z, stencil);
    CHECK(memcmp(original_guest, s_mem + ZETA_AT, sizeof original_guest) == 0);
}

static void partial_depth_seed_test(void)
{
    uint32_t z[HEIGHT][WIDTH];
    uint8_t stencil[HEIGHT][WIDTH];
    seed_depth_reference(ZETA_AT, z, stencil);
    setup_depth(ZETA_AT);
    depth_clear(1, 0x12345600u);
    update_depth_reference(z, stencil, 1, 0x12345600u, 0, 0, WIDTH, HEIGHT);
    set_clear_rect(1, 2, 5, 6);
    depth_clear(2, 0xFFFFFF42u);
    update_depth_reference(z, stencil, 2, 0xFFFFFF42u, 1, 2, 6, 7);
    expect_no_error();
    check_depth_reference(ZETA_AT, z, stencil);

    seed_depth_reference(ZETA2_AT, z, stencil);
    setup_depth(ZETA2_AT);
    set_clear_rect(0, 0, WIDTH - 1u, HEIGHT - 1u);
    depth_clear(2, 0x11111131u);
    update_depth_reference(z, stencil, 2, 0x11111131u, 0, 0, WIDTH, HEIGHT);
    set_clear_rect(3, 1, 13, 9);
    depth_clear(1, 0xABCDEF00u);
    update_depth_reference(z, stencil, 1, 0xABCDEF00u, 3, 1, 14, 10);
    expect_no_error();
    check_depth_reference(ZETA2_AT, z, stencil);
}

static void partial_depth_empty_test(void)
{
    uint32_t color[HEIGHT][WIDTH], z[HEIGHT][WIDTH];
    uint8_t stencil[HEIGHT][WIDTH];
    seed_color_reference(color);
    seed_depth_reference(ZETA_AT, z, stencil);
    setup_depth(ZETA_AT);
    set_clear_rect(WIDTH + 1u, 0, WIDTH + 4u, 4);
    depth_clear(3, 0xABCDEF55u);
    flip();
    expect_no_error();
    check_color_reference(color);
    method(0x1D90, 0xFF010203u);
    depth_clear(0xF3u, 0xABCDEF55u);
    float depth;
    uint8_t st;
    CHECK(!bw_metal_test_read_depth(ZETA_AT, 0, 0, &depth, &st));
    flip();
    expect_no_error();
    check_color_reference(color);

    set_clear_rect(0, 0, WIDTH - 1u, HEIGHT - 1u);
    depth_clear(3, 0x12345678u);
    update_depth_reference(z, stencil, 3, 0x12345678u, 0, 0, WIDTH, HEIGHT);
    expect_no_error();
    set_clear_rect(8, 5, 3, 7);
    depth_clear(0xF3u, 0xABCDEF55u);
    flip();
    expect_no_error();
    check_color_reference(color);
    check_depth_reference(ZETA_AT, z, stencil);
}

static void partial_depth_unbound_test(void)
{
    uint32_t color[HEIGHT][WIDTH];
    seed_color_reference(color);
    setup_depth(0);
    set_clear_rect(2, 3, 10, 8);
    method(0x1D90, 0xFF214365u);
    depth_clear(0xF3u, 0xABCDEF55u);
    update_color_reference(color, 0xF0u, 0xFF214365u, 2, 3, 11, 9);
    flip();
    expect_no_error();
    check_color_reference(color);
}

static void partial_depth_flip_only_test(void)
{
    uint32_t color[HEIGHT][WIDTH], z[HEIGHT][WIDTH];
    uint8_t stencil[HEIGHT][WIDTH];
    seed_color_reference(color);
    seed_depth_reference(ZETA_AT, z, stencil);
    setup_depth(ZETA_AT);
    set_clear_rect(2, 3, 10, 8);
    depth_clear(1, 0x12345678u);
    update_depth_reference(z, stencil, 1, 0x12345678u, 2, 3, 11, 9);
    color[0][0] = 0xA1B2C3D4u;
    put32(COLOR_AT, color[0][0]);
    s_capture_present = 1;
    flip();
    expect_no_error();
    check_color_reference(color);
    check_depth_reference(ZETA_AT, z, stencil);
    CHECK(s_present_count == 1 && s_present_pixel == color[0][0]);
}

static void partial_depth_unbound_flip_only_test(void)
{
    uint32_t color[HEIGHT][WIDTH];
    seed_color_reference(color);
    setup_depth(0);
    set_clear_rect(2, 3, 10, 8);
    depth_clear(3, 0x12345678u);
    flip();
    expect_no_error();
    check_color_reference(color);
}

static void partial_depth_rebind_test(void)
{
    uint32_t color[HEIGHT][WIDTH], z[HEIGHT][WIDTH];
    uint8_t stencil[HEIGHT][WIDTH];
    const uint32_t other = COLOR_AT + PITCH * HEIGHT;
    seed_color_reference(color);
    memset(s_mem + other, 0x6B, PITCH * HEIGHT);
    seed_depth_reference(ZETA_AT, z, stencil);
    clear(0xFF214365u);
    update_color_reference(color, 0xF0u, 0xFF214365u, 0, 0, WIDTH, HEIGHT);
    method(0x0210, other);
    setup_depth(ZETA_AT);
    set_clear_rect(2, 3, 10, 8);
    depth_clear(1, 0x12345678u);
    update_depth_reference(z, stencil, 1, 0x12345678u, 2, 3, 11, 9);
    flip();
    expect_no_error();
    check_color_reference(color);
    check_depth_reference(ZETA_AT, z, stencil);
    for (unsigned i = 0; i < PITCH * HEIGHT; i++)
        CHECK(s_mem[other + i] == 0x6Bu);
}

static void partial_mask_full_depth_test(void)
{
    uint32_t color[HEIGHT][WIDTH], z[HEIGHT][WIDTH];
    uint8_t stencil[HEIGHT][WIDTH];
    seed_color_reference(color);
    seed_depth_reference(ZETA_AT, z, stencil);
    setup_depth(ZETA_AT);
    set_clear_rect(0, 0, WIDTH - 1u, HEIGHT - 1u);
    method(0x1D90, 0xA1B2C3D4u);
    depth_clear(0xA1u, 0x654321FFu);
    update_color_reference(color, 0xA0u, 0xA1B2C3D4u, 0, 0, WIDTH, HEIGHT);
    update_depth_reference(z, stencil, 1, 0x654321FFu, 0, 0, WIDTH, HEIGHT);
    flip();
    expect_no_error();
    check_color_reference(color);
    check_depth_reference(ZETA_AT, z, stencil);
}

static void partial_restart_test(void)
{
    uint32_t color[HEIGHT][WIDTH], z[HEIGHT][WIDTH];
    uint8_t stencil[HEIGHT][WIDTH];
    for (unsigned pass = 0; pass < 2; pass++) {
        seed_color_reference(color);
        seed_depth_reference(ZETA_AT, z, stencil);
        setup_depth(ZETA_AT);
        set_clear_rect(1, 2, 12, 9);
        method(0x1D90, 0x12345678u);
        depth_clear(0x53u, 0x789ABC65u);
        update_color_reference(color, 0x50u, 0x12345678u, 1, 2, 13, 10);
        update_depth_reference(z, stencil, 3, 0x789ABC65u, 1, 2, 13, 10);
        flip();
        expect_no_error();
        check_color_reference(color);
        check_depth_reference(ZETA_AT, z, stencil);
        if (!pass) {
            bw_metal_backend_stop();
            float depth;
            uint8_t st;
            CHECK(!bw_metal_test_read_depth(ZETA_AT, 0, 0, &depth, &st));
            CHECK(bw_metal_backend_start_for_test(s_mem, sizeof s_mem));
            expect_no_error();
        }
    }
}

static void check_depth(uint32_t address, uint32_t value, uint8_t stencil)
{
    const uint32_t locations[][2] = {{0, 0}, {WIDTH - 1u, HEIGHT - 1u}, {4, 3}};
    for (unsigned i = 0; i < sizeof locations / sizeof locations[0]; i++) {
        float actual = -1;
        uint8_t actual_stencil = 0;
        CHECK(bw_metal_test_read_depth(address, locations[i][0], locations[i][1],
                                       &actual, &actual_stencil));
        CHECK(fabsf(actual - (float)value / 16777215.0f) < 0.000001f);
        CHECK(actual_stencil == stencil);
    }
}

static void depth_clear_test(void)
{
    float z;
    uint8_t stencil;
    memset(s_mem + ZETA_AT, 0xA5, PITCH * HEIGHT);
    setup_depth(ZETA_AT);
    depth_clear(3, 0x4567895Au);
    expect_no_error();
    check_depth(ZETA_AT, 0x456789u, 0x5Au);
    CHECK(pixel(0, 0) == 0xA5A5A5A5u);

    depth_clear(1, 0xABCDEF11u);
    expect_no_error();
    check_depth(ZETA_AT, 0xABCDEFu, 0x5Au);
    depth_clear(2, 0x111111C4u);
    expect_no_error();
    check_depth(ZETA_AT, 0xABCDEFu, 0xC4u);

    setup_depth(ZETA2_AT);
    depth_clear(1, 0x12345600u);
    CHECK(!bw_metal_test_read_depth(ZETA2_AT, 0, 0, &z, &stencil));
    depth_clear(2, 0x65432137u);
    expect_no_error();
    check_depth(ZETA2_AT, 0x123456u, 0x37u);
    check_depth(ZETA_AT, 0xABCDEFu, 0xC4u);

    setup_depth(ZETA3_AT);
    depth_clear(2, 0xFFFFFF42u);
    CHECK(!bw_metal_test_read_depth(ZETA3_AT, 0, 0, &z, &stencil));
    depth_clear(1, 0x00000000u);
    check_depth(ZETA3_AT, 0, 0x42u);
    depth_clear(1, 0xFFFFFF00u);
    check_depth(ZETA3_AT, 0xFFFFFFu, 0x42u);

    setup_depth(ZETA_AT);
    method(0x1D90, 0xFF214365u);
    depth_clear(0xF3u, 0x789ABC65u);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0xFF214365u);
    CHECK(pixel(WIDTH - 1u, HEIGHT - 1u) == 0xFF214365u);
    check_depth(ZETA_AT, 0x789ABCu, 0x65u);
    check_depth(ZETA2_AT, 0x123456u, 0x37u);
    for (unsigned i = 0; i < PITCH * HEIGHT; i++)
        CHECK(s_mem[ZETA_AT + i] == 0xA5u);

    bw_metal_backend_stop();
    CHECK(!bw_metal_test_read_depth(ZETA_AT, 0, 0, &z, &stencil));
    CHECK(bw_metal_backend_start_for_test(s_mem, sizeof s_mem));
    CHECK(!bw_metal_test_read_depth(ZETA_AT, 0, 0, &z, &stencil));
    expect_no_error();
}

static void depth_draw_test(void)
{
    float z;
    uint8_t stencil;
    setup_depth(ZETA_AT);
    depth_clear(3, 0xC000003Cu);
    clear(0xFF010203u);
    method(0x030C, 1);
    method(0x0354, 0x201u);
    method(0x035C, 1);

    triangle_z(0xFF804020u, 0.5f);
    draw(3);
    triangle_z(0xFF20A060u, 0.8f);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0xFF010203u);
    CHECK(pixel(4, 3) == 0xFF804020u);
    CHECK(bw_metal_test_read_depth(ZETA_AT, 4, 3, &z, &stencil));
    CHECK(fabsf(z - 0.5f) < 0.000001f && stencil == 0x3Cu);
    CHECK(bw_metal_test_read_depth(ZETA_AT, 0, 0, &z, &stencil));
    CHECK(fabsf(z - (float)0xC00000u / 16777215.0f) < 0.000001f &&
          stencil == 0x3Cu);
}

static void polygon_offset_test(int mode)
{
    const int biased = mode == 1;
    float z;
    uint8_t stencil;
    setup_depth(ZETA_AT);
    depth_clear(3, 0xC000003Cu);
    clear(0xFF010203u);
    method(0x030C, 1);
    method(0x0354, 0x201u);
    method(0x035C, 1);
    triangle_z(0xFF804020u, 0.5f);
    draw(3);
    method(0x0338, mode ? 1u : 0u);
    method_float(0x0384, mode == 2 ? 0.0f : -0.25f);
    method_float(0x0388, mode == 2 ? 0.0f : -1.0f);
    triangle_z(0xFF20A060u, 0.5f);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == (biased ? 0xFF20A060u : 0xFF804020u));
    CHECK(bw_metal_test_read_depth(ZETA_AT, 4, 3, &z, &stencil));
    CHECK((biased ? z < 0.5f : fabsf(z - 0.5f) < 0.000001f) &&
          stencil == 0x3Cu);
}

static void stencil_draw_test(void)
{
    float z;
    uint8_t stencil;
    setup_depth(ZETA_AT);
    depth_clear(3, 0x8000003Cu);
    clear(0xFF010203u);
    method(0x030C, 0);
    method(0x035C, 0);
    method(0x032C, 1);
    method(0x0360, 0x0Fu);
    method(0x0364, 0x207u);
    method(0x0368, 0x5Au);
    method(0x036C, 0xFFu);
    method(0x0370, 0x1E00u);
    method(0x0374, 0x1E00u);
    method(0x0378, 0x1E01u);
    triangle(0xFF804020u);
    draw(3);

    method(0x0360, 0);
    method(0x0364, 0x202u);
    method(0x0368, 0x0Au);
    method(0x036C, 0x0Fu);
    method(0x0378, 0x1E00u);
    triangle(0xFF20A060u);
    draw(3);
    method(0x0368, 0x0Bu);
    triangle(0xFF2040A0u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0xFF010203u);
    CHECK(pixel(4, 3) == 0xFF20A060u);
    CHECK(bw_metal_test_read_depth(ZETA_AT, 4, 3, &z, &stencil));
    CHECK(fabsf(z - (float)0x800000u / 16777215.0f) < 0.000001f &&
          stencil == 0x3Au);
    CHECK(bw_metal_test_read_depth(ZETA_AT, 0, 0, &z, &stencil));
    CHECK(stencil == 0x3Cu);
}

static void depth_unbound_test(void)
{
    uint8_t before[PITCH * HEIGHT];
    memcpy(before, s_mem + COLOR_AT, sizeof before);
    setup_depth(0);
    depth_clear(3, 0x12345678u);
    expect_no_error();
    CHECK(memcmp(before, s_mem + COLOR_AT, sizeof before) == 0);
    method(0x1D90, 0xFF214365u);
    depth_clear(0xF3u, 0xABCDEF55u);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0xFF214365u);
}

static void depth_failure_test(const char *which)
{
    uint8_t before[PITCH * HEIGHT];
    memcpy(before, s_mem + COLOR_AT, sizeof before);
    setup_depth((!strcmp(which, "depth-address") ||
                 !strcmp(which, "depth-invalid-combined") ||
                 !strcmp(which, "depth-invalid-partial")) ? MEM_BYTES - 4u
                : (!strcmp(which, "depth-color-alias") ||
                   !strcmp(which, "depth-pending-color-alias")) ? COLOR_AT
                : ZETA_AT);
    if (!strcmp(which, "depth-format"))
        method(0x0208, 0x0118u);
    else if (!strcmp(which, "depth-pitch"))
        method(0x020C, ((WIDTH * 4u - 4u) << 16) | PITCH);
    else if (!strcmp(which, "depth-pending-color-alias")) {
        clear(0xFF010203u);
        method(0x0210, COLOR_AT + PITCH * HEIGHT);
    } else if (!strcmp(which, "depth-unknown-flags")) {
        depth_clear(0x04u, 0xABCDEF55u);
        expect_error("clear flags", before);
        return;
    } else if (!strcmp(which, "depth-invalid-combined") ||
               !strcmp(which, "depth-invalid-partial")) {
        if (!strcmp(which, "depth-invalid-partial"))
            set_clear_rect(2, 3, 9, 8);
        method(0x1D90, 0xFF010203u);
        depth_clear(0xF3u, 0xABCDEF55u);
        expect_error("depth/stencil surface guest-memory span", before);
        return;
    } else if (!strcmp(which, "depth-cache-limit")) {
        for (unsigned i = 0; i < 32; i++) {
            method(0x0214, 0x26000u + i * 0x400u);
            depth_clear(3, 0x12345678u);
            expect_no_error();
        }
        method(0x0214, 0x26000u + 32u * 0x400u);
    } else if (!strcmp(which, "depth-overlap") || !strcmp(which, "depth-reinterpret") ||
               !strcmp(which, "depth-texture") || !strcmp(which, "depth-color-after") ||
               !strcmp(which, "depth-color-after-empty")) {
        depth_clear(3, 0x12345678u);
        expect_no_error();
        if (!strcmp(which, "depth-overlap"))
            method(0x0214, ZETA_AT + 4u);
        else if (!strcmp(which, "depth-reinterpret"))
            method(0x020C, ((PITCH + 4u) << 16) | PITCH);
        else if (!strcmp(which, "depth-color-after") || !strcmp(which, "depth-color-after-empty")) {
            method(0x0210, ZETA_AT);
            if (!strcmp(which, "depth-color-after-empty"))
                set_clear_rect(WIDTH + 1u, 0, WIDTH + 3u, 3u);
            clear(0xFF010203u);
            expect_error("colour surface aliases a cleared depth/stencil", before);
            return;
        } else {
            setup_texture(ZETA_AT, 0x01010000u);
            quad();
            draw(6);
            expect_error("sampling a depth/stencil surface", before);
            return;
        }
    }
    depth_clear(3, 0xABCDEF55u);
    expect_error(!strcmp(which, "depth-format") ? "depth/stencil surface format"
                 : (!strcmp(which, "depth-address") || !strcmp(which, "depth-pitch"))
                     ? "depth/stencil surface guest-memory span"
                 : !strcmp(which, "depth-color-alias") ? "aliases the colour surface"
                 : !strcmp(which, "depth-pending-color-alias") ? "aliases a pending colour"
                 : !strcmp(which, "depth-overlap") ? "overlapping depth/stencil"
                 : !strcmp(which, "depth-reinterpret") ? "reinterpreting a depth/stencil"
                 : "depth/stencil surface cache is full", before);
}

static void extent_boundary_test(void)
{
    method(0x0200, 1u << 16);
    method(0x0204, 16384u << 16);
    method(0x020C, 4u);
    clear(0xFF214365u);
    flip();
    expect_no_error();
    uint32_t first, last;
    memcpy(&first, s_mem + COLOR_AT, sizeof first);
    memcpy(&last, s_mem + COLOR_AT + (16384u - 1u) * 4u, sizeof last);
    CHECK(first == 0xFF214365u && last == 0xFF214365u);
}

static void texture_boundary_test(void)
{
    for (unsigned y = 0; y < 16384u; y++)
        put32(TEX_AT + y * 4u, y < 8192u ? 0xFFFF0000u : 0xFF0000FFu);
    setup_texture(TEX_AT, 0x01010000u);
    method(0x1B10, 4u << 16);
    method(0x1B1C, (1u << 16) | 16384u);
    quad();
    for (unsigned i = 0; i < 6; i++) {
        int bottom = i == 2 || i == 4 || i == 5;
        put_float(UV_AT + i * 8u, 0.5f);
        put_float(UV_AT + i * 8u + 4u, bottom ? 16384.0f : 0.0f);
    }
    clear(0xFF010203u);
    draw(6);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == 0xFFFF0000u);
    CHECK(pixel(4, 8) == 0xFF0000FFu);
}

static void lifecycle_test(void)
{
    CHECK(!bw_metal_backend_start_for_test(s_mem, sizeof s_mem));
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), "already registered"));
    bw_metal_backend_stop();
    CHECK(bw_metal_backend_start_for_test(s_mem, sizeof s_mem));
    expect_no_error();
    clear(0xFF304050u);
    flip();
    expect_no_error();
    CHECK(pixel(3, 3) == 0xFF304050u);
    bw_metal_backend_stop();
    CHECK(bw_metal_backend_start_for_test(s_mem, sizeof s_mem));
    expect_no_error();
    clear(0xFF607080u);
    flip();
    expect_no_error();
    CHECK(pixel(3, 3) == 0xFF607080u);
}

static void reset_capture(void)
{
    s_event_count = 0;
    s_present_count = 0;
    s_present_va = s_present_pitch = s_present_pixel = s_present_aa = 0;
    s_present_probe_pixel = s_framebuffer_probe_pixel = 0;
    s_capture_probe = 0;
    s_capture_present = 1;
}

static void snapshot_test(int immediate, int preflip_remap, int rebind_aa)
{
    reset_capture();
    clear(0xFF214365u);
    if (preflip_remap)
        s_contig_high = COLOR_AT + 1;
    if (rebind_aa)
        method(0x0208, 0x2008);
    method(0x0210, COLOR_AT + PITCH * HEIGHT);
    method(0x020C, 0);
    if (immediate)
        method(0x0100, 0x300);
    else
        flip();
    expect_no_error();
    CHECK(s_present_count == 1);
    CHECK(s_present_va == COLOR_AT && s_present_pitch == PITCH && s_present_aa == 1);
    CHECK(s_present_pixel == 0xFF214365u);
    CHECK(pixel(0, 0) == 0xFF214365u);

    s_contig_high = 0;
    if (rebind_aa)
        method(0x0208, 0x0008);
    method(0x0210, COLOR_AT);
    method(0x020C, PITCH);
    clear(0xFF98A7B6u);
    if (immediate)
        method(0x0100, 0x300);
    else
        flip();
    expect_no_error();
    CHECK(s_present_count == 2 && s_present_pixel == 0xFF98A7B6u && s_ab_frame == 2);
}

static void seed_aa_color(uint32_t expected[HEIGHT][AA_WIDTH])
{
    memset(s_mem + COLOR_AT, 0xA5, AA_PITCH * HEIGHT);
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < AA_WIDTH; x++) {
            expected[y][x] = 0xA0000000u | (x << 16) | (y << 8) | (x + y);
            put32(COLOR_AT + y * AA_PITCH + x * 4u, expected[y][x]);
        }
}

static void check_aa_color(uint32_t expected[HEIGHT][AA_WIDTH])
{
    unsigned mismatches = 0;
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < AA_WIDTH; x++)
            if (aa_pixel(x, y) != expected[y][x])
                mismatches++;
        for (unsigned x = AA_WIDTH * 4; x < AA_PITCH; x++)
            if (s_mem[COLOR_AT + y * AA_PITCH + x] != 0xA5)
                mismatches++;
    }
    if (mismatches)
        fprintf(stderr, "AA colour reference mismatches: %u\n", mismatches);
    CHECK(mismatches == 0);
}

static void xrgb_aa_clear_test(void)
{
    uint32_t expected[HEIGHT][AA_WIDTH];
    setup_xrgb_aa_surface();
    seed_aa_color(expected);
    clear(0xD1214365u);
    flip();
    expect_no_error();
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < AA_WIDTH; x++)
            expected[y][x] = 0x00214365u;
    check_aa_color(expected);

    for (uint32_t mask = 1; mask < 16; mask++) {
        seed_aa_color(expected);
        set_clear_rect(3, 2, 7, 6);
        method(0x1D90, 0xE13C5A79u);
        method(0x1D94, mask << 4);
        flip();
        expect_no_error();
        uint32_t bytes = ((mask & 1u) ? 0x00FF0000u : 0u) |
                         ((mask & 2u) ? 0x0000FF00u : 0u) |
                         ((mask & 4u) ? 0x000000FFu : 0u) |
                         ((mask & 8u) ? 0xFF000000u : 0u);
        for (unsigned y = 2; y < 7; y++)
            for (unsigned x = 6; x < 16; x++)
                expected[y][x] = (expected[y][x] & ~bytes) | (0x003C5A79u & bytes);
        check_aa_color(expected);
    }
    seed_aa_color(expected);
    set_clear_rect(WIDTH - 1, HEIGHT - 1, 0xFFFF, 0xFFFF);
    method(0x1D90, 0xE107296Bu);
    method(0x1D94, 0xF0);
    flip();
    expect_no_error();
    expected[HEIGHT - 1][AA_WIDTH - 2] = expected[HEIGHT - 1][AA_WIDTH - 1] = 0x0007296Bu;
    check_aa_color(expected);
}

static void xrgb_aa_depth_test(void)
{
    float z[HEIGHT][AA_WIDTH];
    uint8_t stencil[HEIGHT][AA_WIDTH];
    setup_xrgb_aa_surface();
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < AA_WIDTH; x++)
            put32(ZETA_AT + y * AA_PITCH + x * 4u, ((0x102030u + x + y) << 8) | (x + y));
    method(0x0214, ZETA_AT);
    set_clear_rect(3, 2, 7, 6);
    depth_clear(3, 0x456789A7u);
    expect_no_error();
    CHECK(bw_metal_test_read_depth_surface(ZETA_AT, &z[0][0], &stencil[0][0],
                                           AA_WIDTH * HEIGHT));
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < AA_WIDTH; x++) {
            int inside = x >= 6 && x < 16 && y >= 2 && y < 7;
            uint32_t expected_z = inside ? 0x456789u : 0x102030u + x + y;
            uint8_t expected_stencil = inside ? 0xA7u : (uint8_t)(x + y);
            CHECK(fabsf(z[y][x] - (float)expected_z / 16777215.0f) < 0.0000001f);
            CHECK(stencil[y][x] == expected_stencil);
        }
}

static void xrgb_aa_draw_test(void)
{
    static const struct { uint32_t src, dst, equation, mask; } cases[] = {
        {0x302, 0x303, 0x8006, 0x01010101},
        {0x304, 1,     0x8006, 0x01010101},
        {0x305, 0,     0x8006, 0x01010101},
        {0x308, 1,     0x8006, 0x01010101},
        {1,     0x304, 0x8006, 0x01010101},
        {1,     0x305, 0x8006, 0x01010101},
        {0x302, 0x303, 0x8007, 0x01010101},
        {0x302, 0x303, 0x8008, 0x01010101},
        {0x304, 1,     0x8006, 0x00010101},
    };
    const uint32_t source = 0x8064C832u, destination = 0xB0402080u;
    setup_xrgb_aa_surface();
    triangle(source);
    for (unsigned i = 0; i <= sizeof cases / sizeof cases[0]; i++) {
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = 0; x < AA_WIDTH; x++)
                put32(COLOR_AT + y * AA_PITCH + x * 4u, destination);
        uint32_t expected;
        if (i == 0) {
            method(0x0304, 0);
            method(0x0358, 0x01010101);
            expected = source & 0x00FFFFFFu;
        } else {
            const unsigned j = i - 1;
            blend_state(cases[j].src, cases[j].dst, cases[j].equation, 0);
            method(0x0358, cases[j].mask);
            expected = reference_blend(source, destination & 0x00FFFFFFu, 0,
                                       cases[j].src, cases[j].dst, cases[j].equation) &
                       0x00FFFFFFu;
            if (!(cases[j].mask & 0x01000000u))
                expected |= destination & 0xFF000000u;
        }
        draw(3);
        flip();
        expect_no_error();
        if (!color_near(aa_pixel(8, 3), expected, 1)) {
            fprintf(stderr, "AA draw case %u: expected %08x, got %08x\n",
                    i, expected, aa_pixel(8, 3));
            s_failed = 1;
        }
        CHECK(aa_pixel(0, 0) == destination);
    }
    method(0x0304, 0);
    for (unsigned mask = 0; mask < 16; mask++) {
        uint32_t mask_register = ((mask & 1u) ? 0x00000001u : 0u) |
                                 ((mask & 2u) ? 0x00000100u : 0u) |
                                 ((mask & 4u) ? 0x00010000u : 0u) |
                                 ((mask & 8u) ? 0x01000000u : 0u);
        uint32_t mask_bytes = ((mask & 1u) ? 0x000000FFu : 0u) |
                              ((mask & 2u) ? 0x0000FF00u : 0u) |
                              ((mask & 4u) ? 0x00FF0000u : 0u) |
                              ((mask & 8u) ? 0xFF000000u : 0u);
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = 0; x < AA_WIDTH; x++)
                put32(COLOR_AT + y * AA_PITCH + x * 4u, destination);
        method(0x0358, mask_register);
        draw(3);
        flip();
        expect_no_error();
        uint32_t expected = (destination & ~mask_bytes) | (source & mask_bytes & 0x00FFFFFFu);
        CHECK(aa_pixel(8, 3) == expected);
    }

    method(0x0358, 0x01010101);
    method(0x0208, 0x0108);
    method(0x020C, PITCH);
    clear(0xFF010203u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == source);
    setup_xrgb_aa_surface();
    clear(0xFF010203u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(aa_pixel(8, 3) == (source & 0x00FFFFFFu));
}

static void seed_single_color(uint32_t expected[HEIGHT][WIDTH])
{
    memset(s_mem + COLOR_AT, 0xA5, PITCH * HEIGHT);
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < WIDTH; x++) {
            expected[y][x] = 0xA0000000u | (x << 16) | (y << 8) | (x + y);
            put32(COLOR_AT + y * PITCH + x * 4u, expected[y][x]);
        }
}

static void check_single_color(uint32_t expected[HEIGHT][WIDTH])
{
    unsigned mismatches = 0;
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++)
            if (pixel(x, y) != expected[y][x])
                mismatches++;
        for (unsigned x = WIDTH * 4; x < PITCH; x++)
            if (s_mem[COLOR_AT + y * PITCH + x] != 0xA5)
                mismatches++;
    }
    if (mismatches)
        fprintf(stderr, "Single-sample XRGB colour reference mismatches: %u\n", mismatches);
    CHECK(mismatches == 0);
}

static void xrgb_single_clear_test(void)
{
    uint32_t expected[HEIGHT][WIDTH];
    setup_xrgb_single_surface(COLOR_AT);
    seed_single_color(expected);
    clear(0xD1214365u);
    flip();
    expect_no_error();
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < WIDTH; x++)
            expected[y][x] = 0x00214365u;
    check_single_color(expected);

    for (uint32_t mask = 1; mask < 16; mask++) {
        seed_single_color(expected);
        set_clear_rect(3, 2, 7, 6);
        method(0x1D90, 0xE13C5A79u);
        method(0x1D94, mask << 4);
        flip();
        expect_no_error();
        uint32_t bytes = ((mask & 1u) ? 0x00FF0000u : 0u) |
                         ((mask & 2u) ? 0x0000FF00u : 0u) |
                         ((mask & 4u) ? 0x000000FFu : 0u) |
                         ((mask & 8u) ? 0xFF000000u : 0u);
        for (unsigned y = 2; y < 7; y++)
            for (unsigned x = 3; x < 8; x++)
                expected[y][x] = (expected[y][x] & ~bytes) | (0x003C5A79u & bytes);
        check_single_color(expected);
    }

    seed_single_color(expected);
    set_clear_rect(WIDTH - 1, HEIGHT - 1, 0xFFFF, 0xFFFF);
    method(0x1D90, 0xE107296Bu);
    method(0x1D94, 0xF0);
    flip();
    expect_no_error();
    expected[HEIGHT - 1][WIDTH - 1] = 0x0007296Bu;
    check_single_color(expected);
}

static void xrgb_single_depth_clear_test(void)
{
    uint32_t z[HEIGHT][WIDTH];
    uint8_t stencil[HEIGHT][WIDTH];
    setup_xrgb_single_surface(COLOR_AT);
    method(0x0208, 0x0124u);
    method(0x020C, (PITCH << 16) | PITCH);
    method(0x0214, ZETA_AT);
    seed_depth_reference(ZETA_AT, z, stencil);
    set_clear_rect(2, 2, 7, 6);
    depth_clear(3, 0x456789A7u);
    expect_no_error();
    update_depth_reference(z, stencil, 3, 0x456789A7u, 2, 2, 8, 7);
    check_depth_reference(ZETA_AT, z, stencil);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0xA5A5A5A5u);
}

static void xrgb_single_draw_test(void)
{
    static const struct { uint32_t src, dst, equation, mask; } cases[] = {
        {0x302, 0x303, 0x8006, 0x01010101},
        {0x304, 1,     0x8006, 0x01010101},
        {0x305, 0,     0x8006, 0x01010101},
        {0x308, 1,     0x8006, 0x01010101},
        {1,     0x304, 0x8006, 0x01010101},
        {1,     0x305, 0x8006, 0x01010101},
        {0x304, 1,     0x8006, 0x00010101},
    };
    const uint32_t source = 0x8064C832u, destination = 0xB0402080u;
    setup_xrgb_single_surface(COLOR_AT);
    triangle(source);
    for (unsigned i = 0; i <= sizeof cases / sizeof cases[0]; i++) {
        memset(s_mem + COLOR_AT, 0xA5, PITCH * HEIGHT);
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = 0; x < WIDTH; x++)
                put32(COLOR_AT + y * PITCH + x * 4u, destination);
        uint32_t expected;
        if (i == 0) {
            method(0x0304, 0);
            method(0x0358, 0x01010101);
            expected = source & 0x00FFFFFFu;
        } else {
            const unsigned j = i - 1;
            blend_state(cases[j].src, cases[j].dst, cases[j].equation, 0);
            method(0x0358, cases[j].mask);
            expected = reference_blend(source, destination & 0x00FFFFFFu, 0,
                                       cases[j].src, cases[j].dst, cases[j].equation) &
                       0x00FFFFFFu;
            if (!(cases[j].mask & 0x01000000u))
                expected |= destination & 0xFF000000u;
        }
        draw(3);
        flip();
        expect_no_error();
        check_pixel_near(expected, 1, "single-sample XRGB blend", i);
        CHECK(pixel(0, 0) == destination);
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = WIDTH * 4; x < PITCH; x++)
                CHECK(s_mem[COLOR_AT + y * PITCH + x] == 0xA5);
    }
    method(0x0304, 0);
    for (unsigned mask = 0; mask < 16; mask++) {
        uint32_t mask_register = ((mask & 1u) ? 0x00000001u : 0u) |
                                 ((mask & 2u) ? 0x00000100u : 0u) |
                                 ((mask & 4u) ? 0x00010000u : 0u) |
                                 ((mask & 8u) ? 0x01000000u : 0u);
        uint32_t mask_bytes = ((mask & 1u) ? 0x000000FFu : 0u) |
                              ((mask & 2u) ? 0x0000FF00u : 0u) |
                              ((mask & 4u) ? 0x00FF0000u : 0u) |
                              ((mask & 8u) ? 0xFF000000u : 0u);
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = 0; x < WIDTH; x++)
                put32(COLOR_AT + y * PITCH + x * 4u, destination);
        method(0x0358, mask_register);
        draw(3);
        flip();
        expect_no_error();
        uint32_t expected = (destination & ~mask_bytes) | (source & mask_bytes & 0x00FFFFFFu);
        CHECK(pixel(4, 3) == expected);
    }

    method(0x0358, 0x01010101);
    method(0x0208, 0x0108);
    clear(0xFF010203u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == source);
    setup_xrgb_single_surface(COLOR_AT);
    clear(0xFF010203u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == (source & 0x00FFFFFFu));
}

#define RC_WORD(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                             ((uint32_t)(c) << 8) | (uint32_t)(d))

static void setup_dxt1_texture_stage(unsigned stage, uint32_t address,
                                     uint32_t color0, uint32_t color1, uint32_t selectors)
{
    uint32_t base = 0x1B00u + stage * 0x40u;
    put32(address, color0 | (color1 << 16));
    put32(address + 4, selectors);
    method(base, address);
    method(base + 4, (2u << 24) | (2u << 20) | (1u << 16) | (0x0Cu << 8) | (2u << 4));
    method(base + 8, 0x00030303u);
    method(base + 12, 1u << 30);
    method(base + 20, (2u << 24) | (6u << 16));
    method(0x1744 + stage * 4u, UV_AT);
    method(0x1784 + stage * 4u, 2 | (2 << 4) | (8 << 8));
}

static void setup_dxt1_texture(uint32_t color0, uint32_t color1, uint32_t selectors)
{
    setup_dxt1_texture_stage(0, TEX_AT, color0, color1, selectors);
}

static void setup_dxt1_cube_texture_stage(unsigned stage, uint32_t address,
                                          const uint16_t colors[6])
{
    uint32_t base = 0x1B00u + stage * 0x40u;
    for (unsigned face = 0; face < 6; face++) {
        put32(address + face * 128u, colors[face]);
        put32(address + face * 128u + 4u, 0);
    }
    method(base, address);
    method(base + 4, (2u << 24) | (2u << 20) | (1u << 16) |
                     (0x0Cu << 8) | (2u << 4) | 4u);
    method(base + 8, 0x00030303u);
    method(base + 12, 1u << 30);
    method(base + 20, (2u << 24) | (6u << 16));
}

static void setup_combiner(const Nv2aCombiner *rc)
{
    for (unsigned i = 0; i < 8; i++) {
        method(0x0AC0 + i * 4u, rc->color_icw[i]);
        method(0x0260 + i * 4u, rc->alpha_icw[i]);
        method(0x1E40 + i * 4u, rc->color_ocw[i]);
        method(0x0AA0 + i * 4u, rc->alpha_ocw[i]);
        method(0x0A60 + i * 4u, rc->factor0[i]);
        method(0x0A80 + i * 4u, rc->factor1[i]);
    }
    method(0x0288, rc->final0);
    method(0x028C, rc->final1);
    method(0x1E20, rc->final_c0);
    method(0x1E24, rc->final_c1);
    method(0x1E70, rc->stage_program);
    method(0x1E60, rc->control);
}

static Nv2aCombiner synthetic_combiner(void)
{
    Nv2aCombiner rc = {0};
    rc.color_icw[0] = RC_WORD(4, 8, 0, 0);
    rc.alpha_icw[0] = RC_WORD(0x14, 0x18, 0, 0);
    rc.color_ocw[0] = 12u << 4;
    rc.alpha_ocw[0] = 12u << 4;
    rc.final0 = RC_WORD(0, 0, 0, 12);
    rc.final1 = RC_WORD(0, 0, 0x1C, 0x80);
    rc.stage_program = rc.control = 1;
    return rc;
}

static uint32_t reference_combiner_with_secondary(const Nv2aCombiner *rc, uint32_t vertex,
                                                   uint32_t secondary, uint32_t texel)
{
    float v0[4], v1[4], fog[4] = {0}, t[4][4] = {{0}}, out[4];
    nv2a_rc_unpack(vertex, v0);
    nv2a_rc_unpack(secondary, v1);
    nv2a_rc_unpack(texel, t[0]);
    for (unsigned i = 1; i < 4; i++)
        t[i][3] = 1.0f;
    nv2a_rc_eval(rc, v0, v1, fog, (const float (*)[4])t, out);
    return ((uint32_t)lroundf(out[3] * 255) << 24) |
           ((uint32_t)lroundf(out[0] * 255) << 16) |
           ((uint32_t)lroundf(out[1] * 255) << 8) |
            (uint32_t)lroundf(out[2] * 255);
}

static uint32_t reference_combiner(const Nv2aCombiner *rc, uint32_t vertex, uint32_t texel)
{
    return reference_combiner_with_secondary(rc, vertex, 0, texel);
}

static void extended_basic_test(void)
{
    Nv2aCombiner rc = synthetic_combiner();
    const uint32_t source = 0x8064C832u;
    setup_dxt1_texture(0xF800u, 0x001Fu, 0);
    setup_combiner(&rc);
    triangle(source);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), reference_combiner(&rc, source, 0xFFFF0000u), 1));
    CHECK(pixel(0, 0) == 0xFF102030u);
}

static void extended_texture_cache_test(void)
{
    Nv2aCombiner rc = synthetic_combiner();
    uint64_t uploads = bw_metal_test_texture_upload_count();
    setup_dxt1_texture(0xF800u, 0, 0);
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);

    clear(0xFF102030u);
    draw(3);
    draw(3);
    CHECK(bw_metal_test_texture_upload_count() == uploads + 1u);
    flip();
    expect_no_error();
    check_pixel_near(0xFFFF0000u, 1, "cached texture first frame", 0);

    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(bw_metal_test_texture_upload_count() == uploads + 1u);
    check_pixel_near(0xFFFF0000u, 1, "cached texture unchanged frame", 0);

    put32(TEX_AT, 0x07E0u);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(bw_metal_test_texture_upload_count() == uploads + 2u);
    check_pixel_near(0xFF00FF00u, 1, "cached texture invalidation", 0);
}

static void xrgb_single_extended_test(void)
{
    Nv2aCombiner rc = synthetic_combiner();
    setup_xrgb_single_surface(COLOR_AT);
    setup_dxt1_texture(0xF800u, 0x001Fu, 0);
    setup_combiner(&rc);
    triangle(0x80C86432u);
    clear(0xFF204060u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), 0x00C80000u, 1));

    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < WIDTH; x++)
            put32(COLOR_AT + y * PITCH + x * 4u, 0xA0204060u);
    blend_state(0x304, 1, 0x8006, 0);
    method(0x0358, 0x00010101u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == 0xA0204060u);

    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < WIDTH; x++)
            put32(COLOR_AT + y * PITCH + x * 4u, 0xA0204060u);
    blend_state(0x305, 0, 0x8006, 0);
    method(0x0358, 0x01010101u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), 0x00C80000u, 1));
    CHECK(pixel(0, 0) == 0xA0204060u);
}

static void xrgb_single_rejection_test(const char *which)
{
    uint8_t before[PITCH * HEIGHT];
    const char *expected = "unsupported colour surface";
    memcpy(before, s_mem + COLOR_AT, sizeof before);
    reset_capture();
    setup_xrgb_single_surface(COLOR_AT);
    if (!strcmp(which, "xrgb-single-reject-default-layout"))
        method(0x0208, 0x0004);
    else if (!strcmp(which, "xrgb-single-reject-swizzled"))
        method(0x0208, 0x0204);
    else if (!strcmp(which, "xrgb-single-reject-fixed-one"))
        method(0x0208, 0x0105);
    else if (!strcmp(which, "xrgb-single-reject-2d-aa"))
        method(0x0208, 0x2104);
    else if (!strcmp(which, "xrgb-single-reject-pitch")) {
        method(0x020C, WIDTH * 4u - 4u);
        expected = "surface guest-memory span";
    } else if (!strcmp(which, "xrgb-single-reject-origin")) {
        method(0x0200, (WIDTH << 16) | 1u);
        expected = "surface clip origin";
    } else if (!strcmp(which, "xrgb-single-reject-unmapped")) {
        method(0x0210, MEM_BYTES - 4u);
        expected = "surface guest-memory span";
    } else if (!strcmp(which, "xrgb-single-reject-incomplete-rect")) {
        method(0x1D98, 0);
        expected = "incomplete clear rectangle";
    } else if (!strcmp(which, "xrgb-single-reject-depth")) {
        clear(0xFF102030u);
        triangle(0xFFFFFFFFu);
        method(0x0214, ZETA_AT);
        method(0x030C, 1);
        method(0x0354, 0x207u);
        draw(3);
        expected = "unsupported depth/stencil surface format";
    } else {
        CHECK(0 && "unknown single-sample XRGB rejection test");
        return;
    }
    if (strcmp(which, "xrgb-single-reject-depth"))
        clear(0xFF102030u);
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), expected));
    flip();
    CHECK(s_present_count == 0 && s_framebuffer_info_count == 0 && s_ab_frame == 0);
    CHECK(memcmp(before, s_mem + COLOR_AT, sizeof before) == 0);
}

static void extended_combiner_test(void)
{
    const uint32_t source = 0x8050A0C0u;
    Nv2aCombiner rc = synthetic_combiner();
    setup_dxt1_texture(0xF81Fu, 0x07E0u, 0);
    triangle(source);

    rc.color_icw[0] = RC_WORD(4, 8, 0x24, 0x28);
    rc.alpha_icw[0] = RC_WORD(0x14, 0x18, 0x34, 0x38);
    rc.color_ocw[0] = (12u << 4) | 13u;
    rc.alpha_ocw[0] = (12u << 4) | 13u;
    rc.final0 = RC_WORD(0x14, 12, 13, 0);
    rc.final1 = RC_WORD(0, 0, 0x1D, 0x80);
    setup_combiner(&rc);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), reference_combiner(&rc, source, 0xFFFF00FFu), 1));

    /* Alpha must read the pre-stage R0 blue, not the RGB output just written. */
    rc.color_icw[0] = RC_WORD(4, 0x20, 0, 0);
    rc.alpha_icw[0] = RC_WORD(12, 0x20, 0, 0);
    rc.color_ocw[0] = rc.alpha_ocw[0] = 12u << 4;
    rc.final0 = RC_WORD(0, 0, 0, 12);
    rc.final1 = RC_WORD(0, 0, 0x1C, 0x80);
    setup_combiner(&rc);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), reference_combiner(&rc, source, 0xFFFF00FFu), 1));
    CHECK(color_near(pixel(4, 3), source & 0x00FFFFFFu, 1));

    /* Sum routing and final-stage addition both clamp independently. */
    rc.color_icw[0] = RC_WORD(4, 0x20, 4, 0x20);
    rc.alpha_icw[0] = RC_WORD(0x14, 0x20, 0x14, 0x20);
    rc.color_ocw[0] = rc.alpha_ocw[0] = 12u << 8;
    rc.final0 = RC_WORD(0x14, 12, 0, 8);
    setup_combiner(&rc);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), reference_combiner(&rc, source, 0xFFFF00FFu), 1));

    /* R0 alpha starts at texture zero's alpha before the general stage. */
    rc.color_icw[0] = RC_WORD(4, 0x1C, 0, 0);
    rc.alpha_icw[0] = 0;
    rc.color_ocw[0] = 12u << 4;
    rc.alpha_ocw[0] = 0;
    rc.final0 = RC_WORD(0, 0, 0, 12);
    rc.final1 = RC_WORD(0, 0, 0x1C, 0x80);
    setup_combiner(&rc);
    setup_dxt1_texture(0x001Fu, 0xF800u, 0);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), 0xFF50A0C0u, 1));

    put32(TEX_AT + 4, 0xFFFFFFFFu);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == 0);
}

static void extended_multistage_combiner_test(void)
{
    const uint32_t source = 0x9F4F91BCu, texel = 0xFFFF00FFu;
    Nv2aCombiner rc = synthetic_combiner();
    setup_dxt1_texture(0xF81Fu, 0x07E0u, 0);
    triangle(source);
    rc.color_icw[0] = RC_WORD(4, 8, 0x24, 0x28);
    rc.alpha_icw[0] = RC_WORD(0x14, 0x18, 0x34, 0x38);
    rc.color_ocw[0] = rc.alpha_ocw[0] = (12u << 4) | 13u;
    for (unsigned i = 1; i < 8; i++) {
        if (i & 1u) {
            rc.color_icw[i] = RC_WORD(12, 0x20, 13, 0x20);
            rc.alpha_icw[i] = RC_WORD(12, 0x20, 0x1D, 0x20);
            rc.color_ocw[i] = rc.alpha_ocw[i] = 12u << 8;
        } else {
            rc.color_icw[i] = RC_WORD(12, 0x24, 13, 0x28);
            rc.alpha_icw[i] = RC_WORD(0x1C, 0x34, 0x1D, 0x38);
            rc.color_ocw[i] = rc.alpha_ocw[i] = (13u << 4) | 12u;
        }
    }
    for (unsigned count = 1; count <= 8; count++) {
        rc.control = count;
        setup_combiner(&rc);
        clear(0xFF102030u);
        draw(3);
        flip();
        expect_no_error();
        check_pixel_near(reference_combiner(&rc, source, texel), 1,
                         "multiple combiner stages", count);
    }
}

static void check_extended_combiner(const Nv2aCombiner *rc, uint32_t source,
                                    const char *description, unsigned variant)
{
    setup_combiner(rc);
    triangle(source);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(reference_combiner(rc, source, 0xFFFF00FFu), 1,
                     description, variant);
}

static void extended_combiner_mappings_test(void)
{
    const uint32_t source = 0x7F354A59u;
    const uint32_t output_maps[] = {0, 0x08, 0x10, 0x18, 0x20, 0x30};
    Nv2aCombiner rc = synthetic_combiner();
    setup_dxt1_texture(0xF81Fu, 0x07E0u, 0);

    rc.control = 2;
    rc.color_ocw[0] = rc.alpha_ocw[0] = 12u << 4;
    rc.color_ocw[1] = rc.alpha_ocw[1] = 12u << 4;
    for (unsigned map = 0; map < 8; map++) {
        rc.color_icw[0] = RC_WORD((map << 5) | 4u, 0x20, 0, 0);
        rc.alpha_icw[0] = RC_WORD((map << 5) | 0x14u, 0x20, 0, 0);
        for (unsigned negate = 0; negate < 2; negate++) {
            rc.color_icw[1] = RC_WORD((negate ? 0xE0u : 0xC0u) | 12u, 0x20, 0, 0);
            rc.alpha_icw[1] = RC_WORD((negate ? 0xE0u : 0xC0u) | 0x1Cu, 0x20, 0, 0);
            check_extended_combiner(&rc, source, "general input mapping", map * 2 + negate);
        }
    }

    rc = synthetic_combiner();
    rc.color_icw[0] = RC_WORD(4, 0x20, 0, 0);
    rc.alpha_icw[0] = RC_WORD(0x14, 0x20, 0, 0);
    for (unsigned map = 0; map < 8; map++) {
        rc.final0 = RC_WORD(0, 0, 0, (map << 5) | 12u);
        rc.final1 = RC_WORD(0, 0, (map << 5) | 0x1Cu, 0x80);
        check_extended_combiner(&rc, source, "final input mapping", map);
    }

    rc = synthetic_combiner();
    rc.control = 2;
    rc.color_icw[0] = RC_WORD(4, 0x20, 0, 0);
    rc.alpha_icw[0] = RC_WORD(0x14, 0x20, 0, 0);
    rc.color_ocw[1] = rc.alpha_ocw[1] = 12u << 4;
    for (unsigned map = 0; map < sizeof output_maps / sizeof output_maps[0]; map++) {
        rc.color_ocw[0] = rc.alpha_ocw[0] = (12u << 4) | (output_maps[map] << 12);
        for (unsigned negate = 0; negate < 2; negate++) {
            rc.color_icw[1] = RC_WORD((negate ? 0xE0u : 0xC0u) | 12u, 0x20, 0, 0);
            rc.alpha_icw[1] = RC_WORD((negate ? 0xE0u : 0xC0u) | 0x1Cu, 0x20, 0, 0);
            check_extended_combiner(&rc, source, "output mapping", map * 2 + negate);
        }
    }
}

static void extended_combiner_operations_test(void)
{
    const uint32_t source = 0x7F354A59u;
    const uint32_t mux_sources[] = {0x00354A59u, 0x7F354A59u,
                                    0x80354A59u, 0xFF354A59u};
    Nv2aCombiner rc = synthetic_combiner();
    setup_dxt1_texture(0xF81Fu, 0x07E0u, 0);

    rc.color_icw[0] = RC_WORD(4, 8, 0x24, 8);
    rc.alpha_icw[0] = RC_WORD(0x14, 0x20, 0, 0);
    rc.final0 = RC_WORD(0, 0, 13, 12);
    for (unsigned flags = 0; flags < 4; flags++) {
        rc.color_ocw[0] = (12u << 4) | 13u | (flags << 12);
        check_extended_combiner(&rc, source, "dot product", flags);
    }

    rc = synthetic_combiner();
    rc.control = 2;
    rc.color_icw[0] = RC_WORD(4, 0x20, 0, 0);
    rc.alpha_icw[0] = RC_WORD(0x14, 0x20, 0, 0);
    rc.color_icw[1] = RC_WORD(4, 0x20, 0x24, 0x20);
    rc.alpha_icw[1] = RC_WORD(0x14, 0x20, 0x34, 0x20);
    rc.color_ocw[1] = rc.alpha_ocw[1] = (12u << 8) | (4u << 12);
    for (unsigned select = 0; select < 2; select++) {
        rc.control = 2u | (select << 8);
        for (unsigned value = 0; value < sizeof mux_sources / sizeof mux_sources[0]; value++)
            check_extended_combiner(&rc, mux_sources[value], "mux", select * 4 + value);
    }

    rc = synthetic_combiner();
    rc.color_icw[0] = RC_WORD(4, 0x20, 0x24, 0x20);
    rc.color_ocw[0] = (12u << 4) | 13u | (0xC0u << 12);
    rc.alpha_ocw[0] = 0;
    for (unsigned dest = 12; dest <= 13; dest++) {
        rc.final1 = RC_WORD(0, 0, 0x10u | dest, 0x80);
        check_extended_combiner(&rc, source, "blue to alpha", dest);
    }
    rc.alpha_ocw[0] = 12u << 4;
    rc.final1 = RC_WORD(0, 0, 0x1C, 0x80);
    check_extended_combiner(&rc, source, "alpha overrides blue to alpha", 0);
}

static void extended_combiner_constants_test(void)
{
    const uint32_t source = 0x7F354A59u;
    Nv2aCombiner rc = synthetic_combiner();
    setup_dxt1_texture(0xF81Fu, 0x07E0u, 0);

    rc.control = 2;
    rc.factor0[0] = 0xC0804020u;
    rc.factor1[0] = 0x8040A060u;
    rc.factor0[1] = 0x4070C0D0u;
    rc.factor1[1] = 0xE0D05090u;
    rc.color_icw[1] = RC_WORD(1, 2, 0, 0);
    rc.alpha_icw[1] = RC_WORD(0x11, 0x12, 0, 0);
    rc.color_ocw[1] = rc.alpha_ocw[1] = 12u << 4;
    for (unsigned flags = 0; flags < 4; flags++) {
        rc.control = 2u | ((flags & 1u) << 12) | ((flags & 2u) << 15);
        check_extended_combiner(&rc, source, "per-stage constants", flags);
    }

    rc = synthetic_combiner();
    rc.color_icw[0] = RC_WORD(4, 0x20, 0, 0);
    rc.alpha_icw[0] = RC_WORD(0x14, 0x20, 0, 0);
    rc.final_c0 = 0x80904020u;
    rc.final_c1 = 0x6040A070u;
    rc.final0 = RC_WORD(0x11, 1, 2, 12);
    rc.final1 = RC_WORD(0, 0, 0x12, 0x80);
    check_extended_combiner(&rc, source, "final constants", 0);

    for (unsigned flags = 0; flags < 8; flags++) {
        rc.final0 = RC_WORD(0, 0, 0, 14);
        rc.final1 = RC_WORD(0, 0, 0x1C, flags << 5);
        check_extended_combiner(&rc, source, "final sum", flags);
    }
    rc.final0 = RC_WORD(0, 0, 0, 15);
    rc.final1 = RC_WORD(1, 2, 0x1C, 0x80);
    check_extended_combiner(&rc, source, "final product", 0);
    rc.final1 = RC_WORD(14, 1, 0x1C, 0x80);
    check_extended_combiner(&rc, source, "sum feeding final product", 0);

    rc.final0 = rc.final1 = 0;
    check_extended_combiner(&rc, source, "no final combiner", 0);
}

static void extended_combiner_secondary_test(void)
{
    const uint32_t source = 0x7F354A59u, secondary = 0xA04060C0u;
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {
        .vertices = vertices, .count = 3, .state = &state, .rc = &rc,
        .tex = {{.offset = TEX_AT, .width = 4, .height = 4, .color = 0x0C,
                 .addr_u = 3, .addr_v = 3, .levels = 1,
                 .filter = (2u << 24) | (6u << 16)}},
        .tex_valid = {1}, .tex_normalized = {1},
    };
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    setup_dxt1_texture(0xF81Fu, 0x07E0u, 0);
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = vertices[i].tc[0][3] = 1.0f;
        nv2a_rc_unpack(source, vertices[i].d0);
        nv2a_rc_unpack(secondary, vertices[i].d1);
    }

    rc.color_icw[0] = RC_WORD(5, 0x20, 0, 0);
    rc.alpha_icw[0] = RC_WORD(0x15, 0x20, 0, 0);
    for (unsigned flags = 0; flags < 8; flags++) {
        rc.final0 = RC_WORD(0, 0, 0, 14);
        rc.final1 = RC_WORD(0, 0, 0x1C, flags << 5);
        clear(0xFF102030u);
        bw_metal_test_draw_extended(&surface, &batch);
        flip();
        expect_no_error();
        check_pixel_near(reference_combiner_with_secondary(&rc, source, secondary, 0xFFFF00FFu),
                         1, "secondary and final sum", flags);
    }
}

static void extended_fog_test(void)
{
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {
        .vertices = vertices, .count = 3, .state = &state, .rc = &rc,
        .fogc = {0xE8 / 255.0f, 0x80 / 255.0f, 0x49 / 255.0f, 0.0f},
    };
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    rc.stage_program = 0;
    rc.final0 = RC_WORD(0, 0, 0, 3);
    rc.final1 = RC_WORD(0, 0, 0x13, 0x80);
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        vertices[i].fog = 0.75f;
        vertices[i].pad[0] = 0.25f;
    }
    method(0x02A4, 1);
    method(0x029C, 0x2601);
    method(0x02A8, 0xFF4980E8u);
    method_float(0x09C0, 1.0f);
    method_float(0x09C4, 1.0f);
    clear(0xFF102030u);
    bw_metal_test_draw_extended(&surface, &batch);
    flip();
    expect_no_error();
    check_pixel_near(0x40E88049u, 1, "extended fog", 0);
}

static void extended_passthrough_stage_test(void)
{
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {.vertices = vertices, .count = 3, .state = &state, .rc = &rc};
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    setup_dxt1_texture_stage(0, TEX_AT, 0xF800u, 0, 0);
    setup_dxt1_texture_stage(1, TEX_AT + 8u, 0x07E0u, 0, 0);
    for (unsigned stage = 0; stage < 2; stage++) {
        batch.tex[stage] = (Nv2aTexture){
            .offset = TEX_AT + stage * 8u, .width = 4, .height = 4, .color = 0x0Cu,
            .addr_u = 3, .addr_v = 3, .levels = 1,
            .filter = (2u << 24) | (6u << 16),
        };
        batch.tex_valid[stage] = batch.tex_normalized[stage] = 1;
    }
    rc.stage_program = 0x00021021u;
    rc.final0 = RC_WORD(0, 0, 0, 10);
    rc.final1 = RC_WORD(0, 0, 0x1A, 0x80);
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        vertices[i].tc[0][3] = vertices[i].tc[1][3] = 1.0f;
        vertices[i].tc[2][0] = 1.5f;
        vertices[i].tc[2][1] = 0.5f;
        vertices[i].tc[2][2] = -0.5f;
        vertices[i].tc[2][3] = 0.25f;
    }
    clear(0xFF102030u);
    bw_metal_test_draw_extended(&surface, &batch);
    flip();
    expect_no_error();
    check_pixel_near(0x40FF8000u, 1, "pass-through texture stage", 0);
}

static void extended_texture_stages_test(void)
{
    const uint32_t colors[4] = {0xFFFF00FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF0000FFu};
    const uint32_t bc1[4] = {0xF81Fu, 0x07E0u, 0xF800u, 0x001Fu};
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {.vertices = vertices, .count = 3, .state = &state, .rc = &rc};
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    for (unsigned stage = 0; stage < 4; stage++) {
        uint32_t address = TEX_AT + stage * 8u;
        setup_dxt1_texture_stage(stage, address, bc1[stage], 0, 0);
        batch.tex[stage] = (Nv2aTexture){
            .offset = address, .width = 4, .height = 4, .color = 0x0C,
            .addr_u = 3, .addr_v = 3, .levels = 1, .filter = (2u << 24) | (6u << 16),
        };
        batch.tex_valid[stage] = batch.tex_normalized[stage] = 1;
    }
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        nv2a_rc_unpack(0x7F354A59u, vertices[i].d0);
        for (unsigned stage = 0; stage < 4; stage++)
            vertices[i].tc[stage][3] = 1.0f;
    }

    for (unsigned mask = 0; mask < 16; mask++) {
        rc.stage_program = 0;
        for (unsigned stage = 0; stage < 4; stage++) {
            batch.tex_valid[stage] = !!(mask & (1u << stage));
            for (unsigned i = 0; i < 3; i++)
                vertices[i].tc[stage][3] = batch.tex_valid[stage] ? 1.0f : 0.0f;
            if (batch.tex_valid[stage])
                rc.stage_program |= 1u << (stage * 5u);
        }
        for (unsigned stage = 0; stage < 4; stage++) {
            uint32_t expected = (mask & (1u << stage)) ? colors[stage] : 0xFF000000u;
            rc.final0 = RC_WORD(0, 0, 0, 8u + stage);
            rc.final1 = RC_WORD(0, 0, 0x18u + stage, 0x80);
            clear(0xFF102030u);
            bw_metal_test_draw_extended(&surface, &batch);
            flip();
            expect_no_error();
            check_pixel_near(expected, 1, "texture stages", mask * 4u + stage);
        }
    }

    rc.stage_program = 1u | (1u << 5) | (1u << 10) | (1u << 15);
    for (unsigned stage = 1; stage < 4; stage++)
        setup_dxt1_texture_stage(stage, TEX_AT + stage * 8u, 0xF800u, 0x07E0u, 0x44444444u);
    for (unsigned stage = 1; stage < 4; stage++) {
        rc.final0 = RC_WORD(0, 0, 0, 8u + stage);
        rc.final1 = RC_WORD(0, 0, 0x18u + stage, 0x80);
        for (unsigned sample = 0; sample < 2; sample++) {
            for (unsigned i = 0; i < 3; i++) {
                for (unsigned coord = 1; coord < 4; coord++) {
                    vertices[i].tc[coord][0] = 0.125f;
                    vertices[i].tc[coord][1] = 0.125f;
                    vertices[i].tc[coord][3] = 1.0f;
                }
                if (sample) {
                    vertices[i].tc[stage][0] = 0.75f;
                    vertices[i].tc[stage][1] = 0.25f;
                    vertices[i].tc[stage][3] = 2.0f;
                }
            }
            clear(0xFF102030u);
            bw_metal_test_draw_extended(&surface, &batch);
            flip();
            expect_no_error();
            check_pixel_near(sample ? 0xFF00FF00u : 0xFFFF0000u, 1,
                             "stage projective coordinates", stage * 2u + sample);
        }
    }

    setup_dxt1_texture_stage(1, TEX_AT + 8u, 0xF800u, 0x07E0u, 0x44444444u);
    rc.stage_program = 1u << 5;
    rc.final0 = RC_WORD(0, 0, 0, 9);
    rc.final1 = RC_WORD(0, 0, 0x19, 0x80);
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    method(0x1748, UV_AT + 0x100u);
    method(0x1788, 2 | (4 << 4) | (16 << 8));
    for (unsigned sample = 0; sample < 2; sample++) {
        for (unsigned i = 0; i < 3; i++) {
            put_float(UV_AT + 0x100u + i * 16u, sample ? 0.75f : 0.125f);
            put_float(UV_AT + 0x104u + i * 16u, sample ? 0.25f : 0.125f);
            put_float(UV_AT + 0x108u + i * 16u, 0.0f);
            put_float(UV_AT + 0x10Cu + i * 16u, sample ? 2.0f : 1.0f);
        }
        clear(0xFF102030u);
        draw(3);
        flip();
        expect_no_error();
        check_pixel_near(sample ? 0xFF00FF00u : 0xFFFF0000u, 1,
                         "executor projective texture stage", sample);
    }
    rc.stage_program = 0;
    rc.final0 = RC_WORD(0, 0, 0, 8);
    rc.final1 = RC_WORD(0, 0, 0x18, 0x80);
    setup_combiner(&rc);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xFF000000u, 1, "executor inactive texture stages", 0);
}

static void setup_dxt_alpha_texture_stage(unsigned stage, uint32_t address, uint32_t format)
{
    for (unsigned block = 0; block < 4; block++) {
        uint8_t *data = s_mem + address + block * 16u;
        if (format == 0x0Eu) {
            put32(address + block * 16u, 0x76543210u);
            put32(address + block * 16u + 4u, 0xFEDCBA98u);
        } else {
            uint64_t selectors = 0;
            data[0] = block & 1u ? 30u : 230u;
            data[1] = block & 1u ? 220u : 40u;
            for (unsigned i = 0; i < 16; i++)
                selectors |= (uint64_t)(i & 7u) << (3u * i);
            for (unsigned i = 0; i < 6; i++)
                data[2 + i] = (uint8_t)(selectors >> (8u * i));
        }
        uint32_t color0 = block & 1u ? 0xF800u : 0x07E0u;
        uint32_t color1 = block & 1u ? 0x001Fu : 0xF800u;
        put32(address + block * 16u + 8u, color0 | (color1 << 16));
        put32(address + block * 16u + 12u, 0xE4E4E4E4u);
    }
    uint32_t base = 0x1B00u + stage * 0x40u;
    method(base, address);
    method(base + 4, (3u << 24) | (3u << 20) | (1u << 16) |
                      (format << 8) | (2u << 4));
    method(base + 8, 0x00030303u);
    method(base + 12, 1u << 30);
    method(base + 20, (2u << 24) | (6u << 16));
}

static void setup_swizzled_texture_stage(unsigned stage, uint32_t address, uint32_t format,
                                         uint32_t base_width, uint32_t base_height,
                                         uint32_t levels, int solid_levels)
{
    const uint32_t rgb565[4] = {0xF800u, 0x07E0u, 0x001Fu, 0xFFFFu};
    const uint32_t argb[4] = {0x20FF0000u, 0x8000FF00u, 0xC00000FFu, 0xFFFFFFFFu};
    uint32_t width = base_width, height = base_height, cursor = address;
    uint32_t bpp = format == 0x05u ? 2u : 4u, log_width = 0, log_height = 0;
    for (uint32_t n = base_width; n > 1u; n >>= 1) log_width++;
    for (uint32_t n = base_height; n > 1u; n >>= 1) log_height++;
    for (uint32_t level = 0; level < levels; level++) {
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t offset = cursor + swizzle_offset(x, y, width, height) * bpp;
                if (bpp == 2u) {
                    uint16_t value = solid_levels ? (uint16_t)rgb565[level]
                        : (uint16_t)((((x * 3u + y * 5u) & 31u) << 11) |
                                     (((x * 7u + y * 9u) & 63u) << 5) |
                                     ((x * 11u + y * 2u) & 31u));
                    memcpy(s_mem + offset, &value, sizeof value);
                } else {
                    uint32_t value = solid_levels ? argb[level]
                        : (((x * 19u + y * 13u + 17u) & 255u) << 24) |
                          (((x * 29u + y * 3u + 11u) & 255u) << 16) |
                          (((x * 7u + y * 31u + 23u) & 255u) << 8) |
                           ((x * 37u + y * 5u + 41u) & 255u);
                    put32(offset, value);
                }
            }
        }
        cursor += width * height * bpp;
        width = width > 1u ? width / 2u : 1u;
        height = height > 1u ? height / 2u : 1u;
    }
    uint32_t base = 0x1B00u + stage * 0x40u;
    method(base, address);
    method(base + 4u, (log_height << 24) | (log_width << 20) | (levels << 16) |
                      (format << 8) | (2u << 4));
    method(base + 8u, 0x00030303u);
    method(base + 12u, (1u << 30) | ((levels - 1u) << 6));
    method(base + 20u, (2u << 24) | (6u << 16));
}

static uint32_t average_argb(uint32_t a, uint32_t b)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        result |= ((((a >> shift) & 255u) + ((b >> shift) & 255u) + 1u) / 2u) << shift;
    return result;
}

static void extended_dot_reflection_test(void)
{
    static const uint16_t diffuse_faces[6] = {
        0xF800u, 0x07E0u, 0x001Fu, 0xFFFFu, 0xFFE0u, 0x07FFu
    };
    static const uint16_t specular_faces[6] = {
        0xF81Fu, 0x07FFu, 0xFFE0u, 0x001Fu, 0xF800u, 0x07E0u
    };
    Nv2aCombiner rc = synthetic_combiner();
    rc.stage_program = 1u | (0x11u << 5) | (0x0Bu << 10) | (0x0Cu << 15);
    rc.final0 = RC_WORD(0, 0, 0, 10);
    rc.final1 = RC_WORD(0, 0, 0x1A, 0x80);

    setup_dxt1_texture_stage(0, TEX_AT, 0xF800u, 0, 0);
    setup_dxt1_texture_stage(1, TEX_AT + 0x100u, 0xFFFFu, 0, 0);
    setup_dxt1_cube_texture_stage(2, TEX_AT + 0x1000u, diffuse_faces);
    setup_dxt1_cube_texture_stage(3, TEX_AT + 0x2000u, specular_faces);
    setup_combiner(&rc);
    method(0x1E74, 0);
    method(0x1E78, 0);
    triangle(0xFFFFFFFFu);

    const float coordinates[3][4] = {
        {0.0f, 1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
    };
    for (unsigned stage = 1; stage < 4; stage++) {
        uint32_t address = UV_AT + stage * 0x100u;
        method(0x1744 + stage * 4u, address);
        method(0x1784 + stage * 4u, 2 | (4 << 4) | (16 << 8));
        for (unsigned vertex_index = 0; vertex_index < 3; vertex_index++)
            for (unsigned component = 0; component < 4; component++)
                put_float(address + vertex_index * 16u + component * 4u,
                          coordinates[stage - 1u][component]);
    }

    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xFF0000FFu, 1, "diffuse cube +Y", 0);

    method(0x1E78, 1u << 16);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xFF00FF00u, 1, "dot prior-stage input", 0);

    method(0x1E78, 0);
    method(0x1E74, 0x111u);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xFF00FF00u, 1, "dot RGB mapping", 0);

    method(0x1E74, 0);
    method(0x0288, RC_WORD(0, 0, 0, 11));
    method(0x028C, RC_WORD(0, 0, 0x1B, 0x80));
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xFFFFFF00u, 1, "specular reflected cube +Y", 0);

    method(0x1E74, 0x111u);
    method(0x1E78, 0x00210000u);
    method(0x0288, RC_WORD(0, 0, 0, 10));
    method(0x028C, RC_WORD(0, 0, 0x1A, 0x80));
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xFF00FF00u, 1, "retail diffuse dot state", 0);

    method(0x0288, RC_WORD(0, 0, 0, 11));
    method(0x028C, RC_WORD(0, 0, 0x1B, 0x80));
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xFFFF00FFu, 1, "retail specular dot state", 0);
}

static void extended_swizzled_test(void)
{
    const uint32_t formats[3] = {0x05u, 0x06u, 0x07u};
    const uint32_t widths[3] = {8u, 4u, 8u}, heights[3] = {4u, 8u, 8u};
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {.vertices = vertices, .count = 3, .state = &state, .rc = &rc};
    uint32_t expected[3][64];
    rc.stage_program = 1u | (1u << 5) | (1u << 10) | (1u << 15);
    for (unsigned stage = 0; stage < 3; stage++) {
        uint32_t address = TEX_AT + stage * 256u;
        setup_swizzled_texture_stage(stage, address, formats[stage],
                                     widths[stage], heights[stage], 1, 0);
        batch.tex[stage] = (Nv2aTexture){
            .offset = address, .width = widths[stage], .height = heights[stage],
            .color = formats[stage], .addr_u = 3, .addr_v = 3, .levels = 1,
            .filter = (2u << 24) | (6u << 16),
        };
        batch.tex_valid[stage] = batch.tex_normalized[stage] = 1;
        CHECK(nv2a_backend_decode_texture(&batch.tex[stage], expected[stage]));
    }
    setup_dxt1_texture_stage(3, TEX_AT + 768u, 0xF81Fu, 0, 0);
    batch.tex[3] = (Nv2aTexture){
        .offset = TEX_AT + 768u, .width = 4, .height = 4, .color = 0x0C,
        .addr_u = 3, .addr_v = 3, .levels = 1, .filter = (2u << 24) | (6u << 16),
    };
    batch.tex_valid[3] = batch.tex_normalized[3] = 1;
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        for (unsigned stage = 0; stage < 4; stage++)
            vertices[i].tc[stage][3] = 1.0f;
    }
    triangle(0xFFFFFFFFu);
    for (unsigned stage = 0; stage < 3; stage++) {
        rc.final0 = RC_WORD(0, 0, 0, 8u + stage);
        rc.final1 = RC_WORD(0, 0, 0x18u + stage, 0x80);
        setup_combiner(&rc);
        uint32_t uv = UV_AT + 0x100u + stage * 0x40u;
        method(0x1744u + stage * 4u, uv);
        method(0x1784u + stage * 4u, 2 | (2 << 4) | (8 << 8));
        for (unsigned sample = 0; sample < widths[stage] * heights[stage]; sample++) {
            uint32_t x = sample % widths[stage];
            uint32_t y = sample / widths[stage];
            float u = (x + 0.5f) / widths[stage];
            float v = (y + 0.5f) / heights[stage];
            for (unsigned i = 0; i < 3; i++) {
                vertices[i].tc[stage][0] = u;
                vertices[i].tc[stage][1] = v;
                put_float(uv + i * 8u, u);
                put_float(uv + i * 8u + 4u, v);
            }
            clear(0xFF102030u);
            bw_metal_test_draw_extended(&surface, &batch);
            flip();
            expect_no_error();
            check_pixel_near(expected[stage][y * widths[stage] + x], 1,
                             "swizzled texture decode", stage * 64u + sample);
            if (sample < 2) {
                clear(0xFF102030u);
                draw(3);
                flip();
                expect_no_error();
                check_pixel_near(expected[stage][y * widths[stage] + x], 1,
                                 "executor swizzled texture", stage * 64u + sample);
            }
        }
        for (unsigned i = 0; i < 3; i++) {
            vertices[i].tc[stage][0] = 1.0f / widths[stage];
            vertices[i].tc[stage][1] = 0.5f / heights[stage];
        }
        clear(0xFF102030u);
        bw_metal_test_draw_extended(&surface, &batch);
        flip();
        expect_no_error();
        check_pixel_near(average_argb(expected[stage][0], expected[stage][1]), 2,
                         "swizzled bilinear filtering", stage);
    }
}

static void extended_swizzled_mips_test(void)
{
    const uint32_t formats[3] = {0x05u, 0x06u, 0x07u};
    const uint32_t widths[3] = {8u, 4u, 8u}, heights[3] = {4u, 8u, 8u};
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    const float scales[5] = {1.0f, 2.0f, 4.0f, 8.0f, 2.828427125f};
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {.vertices = vertices, .count = 3, .state = &state, .rc = &rc};
    uint32_t expected[3][4];
    rc.stage_program = 1u | (1u << 5) | (1u << 10);
    for (unsigned stage = 0; stage < 3; stage++) {
        uint32_t address = TEX_AT + stage * 512u;
        uint32_t w = widths[stage], h = heights[stage];
        uint32_t bpp = formats[stage] == 0x05u ? 2u : 4u;
        setup_swizzled_texture_stage(stage, address, formats[stage], w, h, 4, 1);
        batch.tex[stage] = (Nv2aTexture){
            .offset = address, .width = w, .height = h, .color = formats[stage],
            .addr_u = 3, .addr_v = 3, .levels = 4, .filter = (2u << 24) | (6u << 16),
        };
        batch.tex_valid[stage] = batch.tex_normalized[stage] = 1;
        for (unsigned level = 0; level < 4; level++) {
            uint32_t pixels[64];
            Nv2aTexture probe = batch.tex[stage];
            probe.offset = address;
            probe.width = w;
            probe.height = h;
            probe.levels = 1;
            CHECK(nv2a_backend_decode_texture(&probe, pixels));
            expected[stage][level] = pixels[0];
            address += w * h * bpp;
            w = w > 1u ? w / 2u : 1u;
            h = h > 1u ? h / 2u : 1u;
        }
    }
    triangle(0xFFFFFFFFu);
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        for (unsigned stage = 0; stage < 3; stage++)
            vertices[i].tc[stage][3] = 1.0f;
    }
    for (unsigned sample = 0; sample < 5; sample++) {
        for (unsigned stage = 0; stage < 3; stage++) {
            uint32_t uv = UV_AT + 0x100u + stage * 0x40u;
            method(0x1744u + stage * 4u, uv);
            method(0x1784u + stage * 4u, 2 | (2 << 4) | (8 << 8));
            for (unsigned i = 0; i < 3; i++) {
                float u = (xy[i][0] - 2.0f) * scales[sample] / widths[stage];
                float v = (xy[i][1] - 2.0f) * scales[sample] / heights[stage];
                vertices[i].tc[stage][0] = u;
                vertices[i].tc[stage][1] = v;
                put_float(uv + i * 8u, u);
                put_float(uv + i * 8u + 4u, v);
            }
        }
        for (unsigned stage = 0; stage < 3; stage++) {
            uint32_t value = sample == 4u
                           ? average_argb(expected[stage][1], expected[stage][2])
                           : expected[stage][sample];
            rc.final0 = RC_WORD(0, 0, 0, 8u + stage);
            rc.final1 = RC_WORD(0, 0, 0x18u + stage, 0x80);
            clear(0xFF102030u);
            bw_metal_test_draw_extended(&surface, &batch);
            flip();
            expect_no_error();
            check_pixel_near(value, 2, "swizzled mip selection", sample * 3u + stage);
            setup_combiner(&rc);
            clear(0xFF102030u);
            draw(3);
            flip();
            expect_no_error();
            check_pixel_near(value, 2, "executor swizzled mips", sample * 3u + stage);
        }
    }

    setup_swizzled_texture_stage(2, TEX_AT + 1024u, 0x07u, 1, 1, 1, 0);
    batch.tex[2].width = batch.tex[2].height = batch.tex[2].levels = 1;
    rc.stage_program = 1u << 10;
    rc.final0 = RC_WORD(0, 0, 0, 10);
    rc.final1 = RC_WORD(0, 0, 0x1A, 0x80);
    uint32_t one_pixel;
    CHECK(nv2a_backend_decode_texture(&batch.tex[2], &one_pixel));
    clear(0xFF102030u);
    bw_metal_test_draw_extended(&surface, &batch);
    flip();
    expect_no_error();
    check_pixel_near(one_pixel, 1, "one-pixel swizzled texture", 0);
    setup_combiner(&rc);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(one_pixel, 1, "executor one-pixel swizzled texture", 0);

    setup_swizzled_texture_stage(0, TEX_AT + 1538u, 0x05u, 1, 1, 1, 0);
    batch.tex[0].offset = TEX_AT + 1538u;
    batch.tex[0].width = batch.tex[0].height = batch.tex[0].levels = 1;
    rc.stage_program |= 1u;
    rc.final0 = RC_WORD(0, 0, 0, 8);
    rc.final1 = RC_WORD(0, 0, 0x18, 0x80);
    CHECK(nv2a_backend_decode_texture(&batch.tex[0], &one_pixel));
    clear(0xFF102030u);
    bw_metal_test_draw_extended(&surface, &batch);
    flip();
    expect_no_error();
    check_pixel_near(one_pixel, 1, "two-byte aligned RGB565", 0);
}

static unsigned reference_address(int coordinate, unsigned size, unsigned mode)
{
    int n = (int)size;
    if (mode == 1u)
        return (unsigned)((coordinate % n + n) % n);
    if (mode == 2u) {
        int value = (coordinate % (2 * n) + 2 * n) % (2 * n);
        return (unsigned)(value < n ? value : 2 * n - 1 - value);
    }
    return coordinate < 0 ? 0u : (coordinate >= n ? size - 1u : (unsigned)coordinate);
}

static void extended_texture_address_test(void)
{
    const int coords[][2] = {{-9, 6}, {-1, -1}, {0, 0}, {3, 3}, {4, 4}, {5, -4}, {8, 9}};
    const unsigned modes[] = {1u, 2u, 3u, 5u};
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {.vertices = vertices, .count = 3, .state = &state, .rc = &rc};
    uint32_t expected[4][16];
    rc.stage_program = 1u | (1u << 5) | (1u << 10) | (1u << 15);
    triangle(0xFFFFFFFFu);
    for (unsigned stage = 0; stage < 4; stage++) {
        uint32_t address = TEX_AT + stage * 256u;
        if (stage == 3u) {
            setup_dxt1_texture_stage(stage, address, 0xF800u, 0x001Fu, 0xE4E4E4E4u);
        } else {
            setup_swizzled_texture_stage(stage, address, 0x06u, 4, 4, 1, 0);
            for (unsigned i = 0; i < 16; i++) {
                uint32_t value;
                memcpy(&value, s_mem + address + i * 4u, sizeof value);
                put32(address + i * 4u, value ^ (stage * 0x11220408u));
            }
        }
        batch.tex[stage] = (Nv2aTexture){
            .offset = address, .width = 4, .height = 4, .color = stage == 3u ? 0x0Cu : 0x06u,
            .addr_u = 3, .addr_v = 3, .levels = 1, .filter = (2u << 24) | (6u << 16),
        };
        batch.tex_valid[stage] = batch.tex_normalized[stage] = 1;
        CHECK(nv2a_backend_decode_texture(&batch.tex[stage], expected[stage]));
        method(0x1744u + stage * 4u, UV_AT + 0x100u + stage * 0x40u);
        method(0x1784u + stage * 4u, 2 | (2 << 4) | (8 << 8));
    }
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        for (unsigned stage = 0; stage < 4; stage++)
            vertices[i].tc[stage][3] = 1.0f;
    }
    for (unsigned pass = 0; pass < 4; pass++) {
        for (unsigned stage = 0; stage < 4; stage++) {
            unsigned pair = pass * 4u + stage;
            unsigned u = modes[pair % 4u], v = modes[pair / 4u];
            batch.tex[stage].addr_u = u;
            batch.tex[stage].addr_v = v;
            method(0x1B08u + stage * 0x40u, (3u << 16) | (v << 8) | u);
        }
        for (unsigned sample = 0; sample < sizeof coords / sizeof coords[0] + 1u; sample++) {
            int x = sample == 7u ? -1 : coords[sample][0];
            int y = sample == 7u ? 1 : coords[sample][1];
            float u = sample == 7u ? 0.0f : (x + 0.5f) / 4.0f;
            float v = (y + 0.5f) / 4.0f;
            for (unsigned stage = 0; stage < 4; stage++) {
                uint32_t uv = UV_AT + 0x100u + stage * 0x40u;
                for (unsigned i = 0; i < 3; i++) {
                    vertices[i].tc[stage][0] = u;
                    vertices[i].tc[stage][1] = v;
                    put_float(uv + i * 8u, u);
                    put_float(uv + i * 8u + 4u, v);
                }
            }
            for (unsigned stage = 0; stage < 4; stage++) {
                unsigned col = reference_address(x, 4u, batch.tex[stage].addr_u);
                unsigned row = reference_address(y, 4u, batch.tex[stage].addr_v);
                uint32_t value = expected[stage][row * 4u + col];
                if (sample == 7u) {
                    unsigned next = reference_address(0, 4u, batch.tex[stage].addr_u);
                    value = average_argb(value, expected[stage][row * 4u + next]);
                }
                rc.final0 = RC_WORD(0, 0, 0, 8u + stage);
                rc.final1 = RC_WORD(0, 0, 0x18u + stage, 0x80);
                clear(0xFF102030u);
                bw_metal_test_draw_extended(&surface, &batch);
                flip();
                expect_no_error();
                check_pixel_near(value, 2, "texture address mode", pass * 32u + sample * 4u + stage);
                setup_combiner(&rc);
                clear(0xFF102030u);
                draw(3);
                flip();
                expect_no_error();
                check_pixel_near(value, 2, "executor texture address mode",
                                 pass * 32u + sample * 4u + stage);
            }
        }
    }
    setup_swizzled_texture_stage(3, TEX_AT + 768u, 0x06u, 4, 4, 3, 1);
    batch.tex[3].color = 0x06u;
    batch.tex[3].levels = 3;
    batch.tex[3].addr_u = 1;
    batch.tex[3].addr_v = 2;
    method(0x1BC8, 0x00030201u);
    rc.stage_program = 1u << 15;
    rc.final0 = RC_WORD(0, 0, 0, 11);
    rc.final1 = RC_WORD(0, 0, 0x1B, 0x80);
    for (unsigned i = 0; i < 3; i++) {
        float u = (xy[i][0] - 2.0f) / 2.0f;
        float v = (xy[i][1] - 2.0f) / 2.0f;
        vertices[i].tc[3][0] = u;
        vertices[i].tc[3][1] = v;
        put_float(UV_AT + 0x1C0u + i * 8u, u);
        put_float(UV_AT + 0x1C0u + i * 8u + 4u, v);
    }
    clear(0xFF102030u);
    bw_metal_test_draw_extended(&surface, &batch);
    flip();
    expect_no_error();
    check_pixel_near(0x8000FF00u, 2, "mipped wrap and mirror", 0);
    setup_combiner(&rc);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0x8000FF00u, 2, "executor mipped wrap and mirror", 0);
}

static void setup_dxt_mip_texture_stage(unsigned stage, uint32_t address,
                                        uint32_t format, uint32_t min_filter,
                                        uint32_t base_width, uint32_t base_height)
{
    const uint32_t colors[4] = {0xF800u, 0x07E0u, 0x001Fu, 0xFFFFu};
    uint32_t cursor = address, width = base_width, height = base_height;
    uint32_t log_width = 0, log_height = 0;
    uint32_t block_bytes = format == 0x0Cu ? 8u : 16u;
    for (uint32_t n = base_width; n > 1u; n >>= 1) log_width++;
    for (uint32_t n = base_height; n > 1u; n >>= 1) log_height++;
    for (unsigned level = 0; level < 4; level++) {
        unsigned blocks = ((width + 3u) / 4u) * ((height + 3u) / 4u);
        for (unsigned block = 0; block < blocks; block++) {
            uint32_t color_at = cursor;
            if (format == 0x0Eu) {
                put32(cursor, 0xFFFFFFFFu);
                put32(cursor + 4u, 0xFFFFFFFFu);
                color_at += 8u;
            } else if (format == 0x0Fu) {
                put32(cursor, 0x0000FFFFu);
                put32(cursor + 4u, 0);
                color_at += 8u;
            }
            put32(color_at, colors[level]);
            put32(color_at + 4u, 0);
            cursor += block_bytes;
        }
        width = width > 1u ? width / 2u : 1u;
        height = height > 1u ? height / 2u : 1u;
    }
    uint32_t base = 0x1B00u + stage * 0x40u;
    method(base, address);
    method(base + 4u, (log_height << 24) | (log_width << 20) | (4u << 16) |
                      (format << 8) | (2u << 4));
    method(base + 8u, 0x00030303u);
    method(base + 12u, (1u << 30) | (3u << 6));
    method(base + 20u, (2u << 24) | (min_filter << 16));
}

static void extended_dxt_mips_test(void)
{
    const uint32_t formats[4] = {0x0Cu, 0x0Eu, 0x0Fu, 0x0Cu};
    const uint32_t widths[4] = {8u, 8u, 4u, 8u};
    const uint32_t heights[4] = {8u, 4u, 8u, 8u};
    const uint32_t colors[5] = {
        0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu, 0xFF008080u
    };
    const float scales[5] = {1.0f, 2.0f, 4.0f, 8.0f, 2.828427125f};
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {.vertices = vertices, .count = 3, .state = &state, .rc = &rc};

    rc.stage_program = 1u | (1u << 5) | (1u << 10) | (1u << 15);
    for (unsigned stage = 0; stage < 4; stage++) {
        uint32_t address = TEX_AT + stage * 256u;
        uint32_t min_filter = stage & 1u ? 6u : 2u;
        setup_dxt_mip_texture_stage(stage, address, formats[stage], min_filter,
                                    widths[stage], heights[stage]);
        batch.tex[stage] = (Nv2aTexture){
            .offset = address, .width = widths[stage], .height = heights[stage],
            .color = formats[stage],
            .addr_u = 3, .addr_v = 3, .levels = 4,
            .filter = (2u << 24) | (min_filter << 16),
        };
        batch.tex_valid[stage] = batch.tex_normalized[stage] = 1;
    }
    triangle(0xFFFFFFFFu);
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        for (unsigned stage = 0; stage < 4; stage++)
            vertices[i].tc[stage][3] = 2.0f;
    }
    for (unsigned pass = 0; pass < 3; pass++) {
        uint32_t levels = pass == 2 ? 3u : 4u;
        for (unsigned stage = 0; stage < 4; stage++) {
            uint32_t min_filter = (stage + pass) & 1u ? 6u : 2u;
            uint32_t base = 0x1B00u + stage * 0x40u;
            uint32_t log_width = widths[stage] == 8u ? 3u : 2u;
            uint32_t log_height = heights[stage] == 8u ? 3u : 2u;
            method(base + 4u, (log_height << 24) | (log_width << 20) | (levels << 16) |
                              (formats[stage] << 8) | (2u << 4));
            method(base + 20u, (2u << 24) | (min_filter << 16));
            batch.tex[stage].levels = levels;
            batch.tex[stage].filter = (2u << 24) | (min_filter << 16);
        }
        for (unsigned sample = 0; sample < 5; sample++) {
            for (unsigned stage = 0; stage < 4; stage++) {
                uint32_t uv = UV_AT + 0x100u + stage * 0x40u;
                method(0x1744u + stage * 4u, uv);
                method(0x1784u + stage * 4u, 2 | (4 << 4) | (16 << 8));
                for (unsigned i = 0; i < 3; i++) {
                    float u = (xy[i][0] - 2.0f) * scales[sample] * 2.0f / widths[stage];
                    float v = (xy[i][1] - 2.0f) * scales[sample] * 2.0f / heights[stage];
                    vertices[i].tc[stage][0] = u;
                    vertices[i].tc[stage][1] = v;
                    put_float(uv + i * 16u, u);
                    put_float(uv + i * 16u + 4u, v);
                    put_float(uv + i * 16u + 8u, 0.0f);
                    put_float(uv + i * 16u + 12u, 2.0f);
                }
            }
            for (unsigned stage = 0; stage < 4; stage++) {
                unsigned index = ((stage + pass) & 1u) ? sample : 0u;
                if (levels == 3u && index == 3u)
                    index = 2u;
                uint32_t expected = colors[index];
                rc.final0 = RC_WORD(0, 0, 0, 8u + stage);
                rc.final1 = RC_WORD(0, 0, 0x18u + stage, 0x80);
                clear(0xFF102030u);
                bw_metal_test_draw_extended(&surface, &batch);
                flip();
                expect_no_error();
                check_pixel_near(expected, 2, "direct BC mip selection",
                                 pass * 20u + sample * 4u + stage);

                setup_combiner(&rc);
                clear(0xFF102030u);
                draw(3);
                flip();
                expect_no_error();
                check_pixel_near(expected, 2, "executor BC mip selection",
                                 pass * 20u + sample * 4u + stage);
            }
        }
    }
}

static void extended_dxt_nearest_mip_test(void)
{
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {.vertices = vertices, .count = 3, .state = &state, .rc = &rc};

    setup_dxt_mip_texture_stage(0, TEX_AT, 0x0Fu, 4u, 8u, 8u);
    batch.tex[0] = (Nv2aTexture){
        .offset = TEX_AT, .width = 8, .height = 8, .color = 0x0Fu,
        .addr_u = 3, .addr_v = 3, .levels = 4,
        .filter = (2u << 24) | (4u << 16),
    };
    batch.tex_valid[0] = batch.tex_normalized[0] = 1;
    rc.final0 = RC_WORD(0, 0, 0, 8);
    rc.final1 = RC_WORD(0, 0, 0x18, 0x80);
    triangle(0xFFFFFFFFu);
    method(0x1744, UV_AT + 0x100u);
    method(0x1784, 2 | (4 << 4) | (16 << 8));
    for (unsigned i = 0; i < 3; i++) {
        float u = (xy[i][0] - 2.0f);
        float v = (xy[i][1] - 2.0f);
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        vertices[i].tc[0][0] = u;
        vertices[i].tc[0][1] = v;
        vertices[i].tc[0][3] = 2.0f;
        put_float(UV_AT + 0x100u + i * 16u, u);
        put_float(UV_AT + 0x104u + i * 16u, v);
        put_float(UV_AT + 0x108u + i * 16u, 0.0f);
        put_float(UV_AT + 0x10Cu + i * 16u, 2.0f);
    }

    clear(0xFF102030u);
    bw_metal_test_draw_extended(&surface, &batch);
    flip();
    expect_no_error();
    check_pixel_near(0xFF0000FFu, 2, "direct nearest BC mip", 0);

    setup_combiner(&rc);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xFF0000FFu, 2, "executor nearest BC mip", 0);
}

static void extended_dxt_mip_limit_test(void)
{
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {.vertices = vertices, .count = 3, .state = &state, .rc = &rc};
    uint32_t width = 2048, height = 4, cursor = TEX_AT;
    for (unsigned level = 0; level < 12; level++) {
        uint32_t color = level == 11u ? 0xFFFFu : level == 10u ? 0x001Fu : 0xF800u;
        uint32_t blocks = ((width + 3u) / 4u) * ((height + 3u) / 4u);
        for (uint32_t block = 0; block < blocks; block++) {
            put32(cursor, 0x0000FFFFu);
            put32(cursor + 4u, 0);
            put32(cursor + 8u, color);
            put32(cursor + 12u, 0);
            cursor += 16u;
        }
        width = width > 1u ? width / 2u : 1u;
        height = height > 1u ? height / 2u : 1u;
    }
    method(0x1B00, TEX_AT);
    method(0x1B04, (2u << 24) | (11u << 20) | (12u << 16) | (0x0Fu << 8) | (2u << 4));
    method(0x1B08, 0x00030303u);
    method(0x1B0C, (1u << 30) | (11u << 6));
    method(0x1B14, (2u << 24) | (6u << 16));
    batch.tex[0] = (Nv2aTexture){
        .offset = TEX_AT, .width = 2048, .height = 4, .color = 0x0Fu,
        .addr_u = 3, .addr_v = 3, .levels = 12, .filter = (2u << 24) | (6u << 16),
    };
    batch.tex_valid[0] = batch.tex_normalized[0] = 1;
    rc.final0 = RC_WORD(0, 0, 0, 8);
    rc.final1 = RC_WORD(0, 0, 0x18, 0x80);
    for (unsigned sample = 0; sample < 2; sample++) {
        float scale = sample ? 2048.0f : 1024.0f;
        for (unsigned i = 0; i < 3; i++) {
            vertices[i].x = xy[i][0];
            vertices[i].y = xy[i][1];
            vertices[i].z = 0.5f;
            vertices[i].rhw = 1.0f;
            vertices[i].tc[0][0] = (xy[i][0] - 2.0f) * scale / 2048.0f;
            vertices[i].tc[0][1] = (xy[i][1] - 2.0f) * scale / 4.0f;
            vertices[i].tc[0][3] = 1.0f;
        }
        clear(0xFF102030u);
        bw_metal_test_draw_extended(&surface, &batch);
        flip();
        expect_no_error();
        check_pixel_near(sample ? 0xFFFFFFFFu : 0xFF0000FFu, 2,
                         "maximum BC mip chain", sample);
    }
}

static void extended_dxt_alpha_test(void)
{
    const Nv2aSurface surface = {COLOR_AT, WIDTH, HEIGHT, PITCH, 4, 1, 1};
    const Nv2aRenderState state = {.color_mask = 0x01010101u, .depth_max = 1.0f};
    BwXVertex vertices[3] = {0};
    Nv2aCombiner rc = synthetic_combiner();
    BwXBatch batch = {.vertices = vertices, .count = 3, .state = &state, .rc = &rc};
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    const unsigned ys[4] = {0, 3, 4, 7};
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        for (unsigned stage = 0; stage < 4; stage++)
            vertices[i].tc[stage][3] = 1.0f;
    }
    for (uint32_t format = 0x0Eu; format <= 0x0Fu; format++) {
        for (unsigned stage = 0; stage < 4; stage++) {
            uint32_t address = TEX_AT + stage * 64u;
            setup_dxt_alpha_texture_stage(stage, address, format);
            memset(batch.tex_valid, 0, sizeof batch.tex_valid);
            batch.tex[stage] = (Nv2aTexture){
                .offset = address, .width = 8, .height = 8, .color = format,
                .addr_u = 3, .addr_v = 3, .levels = 1, .filter = (2u << 24) | (6u << 16),
            };
            batch.tex_valid[stage] = batch.tex_normalized[stage] = 1;
            rc.stage_program = 1u << (stage * 5u);
            rc.final0 = RC_WORD(0, 0, 0, 8u + stage);
            rc.final1 = RC_WORD(0, 0, 0x18u + stage, 0x80);
            for (unsigned row = 0; row < 4; row++) {
                for (unsigned x = 0; x < 8; x++) {
                    uint32_t expected = 0;
                    CHECK(d3d8_dxt_decode_texel(s_mem + address, format, x, ys[row], 8, &expected));
                    for (unsigned i = 0; i < 3; i++) {
                        vertices[i].tc[stage][0] = (x + 0.5f) / 8.0f;
                        vertices[i].tc[stage][1] = (ys[row] + 0.5f) / 8.0f;
                    }
                    clear(0xFF102030u);
                    bw_metal_test_draw_extended(&surface, &batch);
                    flip();
                    expect_no_error();
                    check_pixel_near(expected, format == 0x0Fu ? 2 : 1, "DXT alpha texture",
                                     (format << 24) | (stage << 16) | (ys[row] << 8) | x);
                }
            }
            if (stage == 2) {
                uint32_t expected = 0;
                CHECK(d3d8_dxt_decode_texel(s_mem + address, format, 5, 7, 8, &expected));
                setup_combiner(&rc);
                triangle(0xFFFFFFFFu);
                method(0x1744u + stage * 4u, UV_AT + 0x100u);
                method(0x1784u + stage * 4u, 2 | (2 << 4) | (8 << 8));
                for (unsigned i = 0; i < 3; i++) {
                    put_float(UV_AT + 0x100u + i * 8u, 5.5f / 8.0f);
                    put_float(UV_AT + 0x104u + i * 8u, 7.5f / 8.0f);
                }
                clear(0xFF102030u);
                draw(3);
                flip();
                expect_no_error();
                check_pixel_near(expected, format == 0x0Fu ? 2 : 1,
                                 "executor DXT alpha texture", format);
            }
        }
    }
    const uint32_t formats[4] = {0x0Cu, 0x0Eu, 0x0Fu, 0x0Eu};
    const unsigned xs[4] = {0, 7, 5, 1}, sample_y[4] = {0, 3, 7, 0};
    uint32_t expected[4] = {0};
    setup_dxt1_texture_stage(0, TEX_AT, 0xF800u, 0x001Fu, 0);
    batch.tex[0] = (Nv2aTexture){
        .offset = TEX_AT, .width = 4, .height = 4, .color = 0x0C,
        .addr_u = 3, .addr_v = 3, .levels = 1, .filter = (2u << 24) | (6u << 16),
    };
    for (unsigned stage = 1; stage < 4; stage++) {
        uint32_t address = TEX_AT + stage * 64u;
        setup_dxt_alpha_texture_stage(stage, address, formats[stage]);
        batch.tex[stage] = (Nv2aTexture){
            .offset = address, .width = 8, .height = 8, .color = formats[stage],
            .addr_u = 3, .addr_v = 3, .levels = 1, .filter = (2u << 24) | (6u << 16),
        };
    }
    rc.stage_program = 1u | (1u << 5) | (1u << 10) | (1u << 15);
    for (unsigned stage = 0; stage < 4; stage++) {
        unsigned width = stage ? 8u : 4u;
        batch.tex_valid[stage] = batch.tex_normalized[stage] = 1;
        CHECK(d3d8_dxt_decode_texel(s_mem + batch.tex[stage].offset, formats[stage],
                                    xs[stage], sample_y[stage], width, &expected[stage]));
        for (unsigned i = 0; i < 3; i++) {
            vertices[i].tc[stage][0] = (xs[stage] + 0.5f) / width;
            vertices[i].tc[stage][1] = (sample_y[stage] + 0.5f) / width;
        }
    }
    for (unsigned stage = 0; stage < 4; stage++) {
        rc.final0 = RC_WORD(0, 0, 0, 8u + stage);
        rc.final1 = RC_WORD(0, 0, 0x18u + stage, 0x80);
        clear(0xFF102030u);
        bw_metal_test_draw_extended(&surface, &batch);
        flip();
        expect_no_error();
        check_pixel_near(expected[stage], formats[stage] == 0x0Fu ? 2 : 1,
                         "mixed BC texture stages", stage);

        setup_combiner(&rc);
        triangle(0xFFFFFFFFu);
        for (unsigned coord = 0; coord < 4; coord++) {
            unsigned width = coord ? 8u : 4u;
            uint32_t uv = UV_AT + 0x100u + coord * 0x40u;
            method(0x1744u + coord * 4u, uv);
            method(0x1784u + coord * 4u, 2 | (2 << 4) | (8 << 8));
            for (unsigned i = 0; i < 3; i++) {
                put_float(uv + i * 8u, (xs[coord] + 0.5f) / width);
                put_float(uv + i * 8u + 4u, (sample_y[coord] + 0.5f) / width);
            }
        }
        clear(0xFF102030u);
        draw(3);
        flip();
        expect_no_error();
        check_pixel_near(expected[stage], formats[stage] == 0x0Fu ? 2 : 1,
                         "executor mixed BC texture stages", stage);
    }
}

static void set_triangle_uv(float u, float v)
{
    for (unsigned i = 0; i < 3; i++) {
        put_float(UV_AT + i * 8u, u);
        put_float(UV_AT + i * 8u + 4u, v);
    }
}

static void setup_xrgb_convolution_texture(uint32_t address, uint32_t filter)
{
    method(0x1B00, address);
    method(0x1B04, (1u << 16) | (0x1Eu << 8) | (2u << 4));
    method(0x1B08, 0x00030303u);
    method(0x1B0C, 1u << 30);
    method(0x1B10, AA_PITCH << 16);
    method(0x1B14, filter);
    method(0x1B1C, (AA_WIDTH << 16) | HEIGHT);
    method(0x1744, UV_AT);
    method(0x1784, 2 | (2 << 4) | (8 << 8));
}

static void setup_linear_texture(uint32_t address, uint32_t color, uint32_t width,
                                 uint32_t height, uint32_t pitch, uint32_t filter)
{
    method(0x1B00, address);
    method(0x1B04, (1u << 16) | (color << 8) | (2u << 4) | 0xAu);
    method(0x1B08, 0x00010303u);
    method(0x1B0C, 0x4003FFC0u);
    method(0x1B10, pitch << 16);
    method(0x1B14, filter);
    method(0x1B1C, (width << 16) | height);
    method(0x1744, UV_AT);
    method(0x1784, 2 | (2 << 4) | (8 << 8));
}

static void extended_linear_texture_test(void)
{
    const uint32_t width = 4u, height = 4u, pitch = 24u;
    const uint32_t colors[4][4] = {
        {0x10204080u, 0x20408020u, 0x30602040u, 0x40804020u},
        {0x506080A0u, 0x6080A060u, 0x70A06080u, 0x80C08040u},
        {0x90402060u, 0xA0604080u, 0xB08060A0u, 0xC0A08060u},
        {0xD02060A0u, 0xE04080C0u, 0xF060A080u, 0x8080C0A0u},
    };
    Nv2aCombiner rc = synthetic_combiner();
    memset(s_mem + TEX_AT, 0xA5, pitch * height);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            put32(TEX_AT + y * pitch + x * 4u, colors[y][x]);
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);

    setup_linear_texture(TEX_AT, 0x1Eu, width, height, pitch, 0x01032000u);
    set_triangle_uv(1.5f, 2.5f);
    clear(0xFF010203u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == (colors[2][1] | 0xFF000000u));

    setup_linear_texture(TEX_AT, 0x12u, width, height, pitch, 0x02022000u);
    set_triangle_uv(1.0f, 1.0f);
    clear(0xFF010203u);
    draw(3);
    flip();
    expect_no_error();
    uint32_t top = average_argb(colors[0][0], colors[0][1]);
    uint32_t bottom = average_argb(colors[1][0], colors[1][1]);
    check_pixel_near(average_argb(top, bottom), 2, "pitch-linear bilinear texture", 0);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = width * 4u; x < pitch; x++)
            CHECK(s_mem[TEX_AT + y * pitch + x] == 0xA5u);
}

static void extended_bc1_inactive_kernel_test(int immediate)
{
    const uint32_t other_at = COLOR_AT + 0x2000u;
    Nv2aCombiner rc = synthetic_combiner();
    static const struct {
        uint32_t kernel, min;
        int minifying;
        float u;
        uint32_t expected;
    } cases[] = {
        {1, 2, 0, 0.3750f, 0x000000FFu}, {2, 2, 0, 0.3750f, 0x000000FFu},
        {1, 6, 0, -2.000f, 0x00FF0000u}, {2, 6, 0, -2.000f, 0x00FF0000u},
        {1, 2, 0, 0.1875f, 0x00BF0040u}, {2, 2, 0, 0.1875f, 0x00BF0040u},
        {1, 6, 1, -1.0625f, 0x00BF0040u}, {2, 6, 1, -1.0625f, 0x00BF0040u},
    };
    reset_capture();
    memset(s_mem + other_at, 0x6B, PITCH * HEIGHT);
    setup_xrgb_single_surface(COLOR_AT);
    setup_dxt1_texture(0xF800u, 0x001Fu, 1u << 2);
    setup_combiner(&rc);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        method(0x1B14, (2u << 24) | (cases[i].min << 16) | (cases[i].kernel << 13));
        triangle(0xFFFFFFFFu);
        set_triangle_uv(cases[i].u, 0.125f);
        if (cases[i].minifying)
            put_float(UV_AT + 8u, cases[i].u + 6.0f);
        clear(0xFF214365u);
        draw(3);
        setup_xrgb_single_surface(other_at);
        if (immediate)
            method(0x0100, 0x300);
        else
            flip();
        expect_no_error();
        CHECK(pixel(4, 3) == cases[i].expected);
        CHECK(pixel(0, 0) == 0x00214365u);
        CHECK(surface_pixel(other_at, PITCH, 0, 0) == 0x6B6B6B6Bu);
        CHECK(s_framebuffer_info_count == i + 1u && s_present_count == (int)i + 1);
        CHECK(s_framebuffer_info.guest_va == COLOR_AT && s_present_va == COLOR_AT);
        CHECK(s_framebuffer_info_pixel == 0x00214365u && s_present_pixel == 0x00214365u);
        CHECK(s_event_count == (int)(2u * (i + 1u)) &&
              s_events[2u * i] == EVENT_DESCRIPTOR && s_events[2u * i + 1u] == EVENT_PRESENT);
        setup_xrgb_single_surface(COLOR_AT);
    }
}

static void extended_bc1_test(void)
{
    Nv2aCombiner rc = synthetic_combiner();
    setup_dxt1_texture(0xF800u, 0x001Fu, (1u << 2) | (2u << 4) | (3u << 6));
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    put32(TEX_AT + 8, 0xD5A37C91u);

    static const struct { float u; uint32_t expected; } opaque[] = {
        {0.125f, 0xFFFF0000u}, {0.250f, 0xFF800080u},
        {0.375f, 0xFF0000FFu}, {0.625f, 0xFFAA0055u},
        {0.875f, 0xFF5500AAu}, {-2.0f, 0xFFFF0000u},
        {2.0f, 0xFF5500AAu}
    };
    for (unsigned i = 0; i < sizeof opaque / sizeof opaque[0]; i++) {
        set_triangle_uv(opaque[i].u, 0.125f);
        clear(0xFF102030u);
        draw(3);
        flip();
        expect_no_error();
        CHECK(color_near(pixel(4, 3), opaque[i].expected, 1));
    }

    /* A one-level texture cannot select another mip even with mip filtering. */
    method(0x1B14, (2u << 24) | (2u << 16));
    set_triangle_uv(0.250f, 0.125f);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), 0xFF800080u, 1));

    /* Different solid blocks verify row stride and block ordering, not just
     * decoding within a single compressed block. */
    setup_dxt1_texture(0xF800u, 0, 0);
    method(0x1B04, (3u << 24) | (3u << 20) | (1u << 16) | (0x0Cu << 8) | (2u << 4));
    put32(TEX_AT + 8, 0x000007E0u);
    put32(TEX_AT + 12, 0);
    put32(TEX_AT + 16, 0x0000001Fu);
    put32(TEX_AT + 20, 0);
    put32(TEX_AT + 24, 0x0000FFFFu);
    put32(TEX_AT + 28, 0);
    static const struct { float u, v; uint32_t expected; } blocks[] = {
        {0.25f, 0.25f, 0xFFFF0000u}, {0.75f, 0.25f, 0xFF00FF00u},
        {0.25f, 0.75f, 0xFF0000FFu}, {0.75f, 0.75f, 0xFFFFFFFFu}
    };
    for (unsigned i = 0; i < sizeof blocks / sizeof blocks[0]; i++) {
        set_triangle_uv(blocks[i].u, blocks[i].v);
        clear(0xFF102030u);
        draw(3);
        flip();
        expect_no_error();
        CHECK(color_near(pixel(4, 3), blocks[i].expected, 1));
    }

    setup_dxt1_texture(0x001Fu, 0xF800u, (1u << 2) | (2u << 4) | (3u << 6));
    put32(TEX_AT + 8, 0xD5A37C91u);
    static const struct { float u; uint32_t expected; } transparent[] = {
        {0.125f, 0xFF0000FFu}, {0.375f, 0xFFFF0000u},
        {0.625f, 0xFF800080u}, {0.875f, 0x00000000u}
    };
    for (unsigned i = 0; i < sizeof transparent / sizeof transparent[0]; i++) {
        set_triangle_uv(transparent[i].u, 0.125f);
        clear(0xFF102030u);
        draw(3);
        flip();
        expect_no_error();
        CHECK(color_near(pixel(4, 3), transparent[i].expected, 1));
    }
    uint32_t sentinel;
    memcpy(&sentinel, s_mem + TEX_AT + 8, sizeof sentinel);
    CHECK(sentinel == 0xD5A37C91u);
}

static void extended_projective_test(void)
{
    const float xy[3][2] = {{2, 2}, {14, 2}, {2, 10}};
    const float w[3] = {1, 2, 1}, q[3] = {1, 2, 1};
    const float u[3] = {0.125f, 0.375f, 0.125f};
    Nv2aCombiner rc = synthetic_combiner();
    setup_dxt1_texture(0xF800u, 0x001Fu, 1u << 2);
    setup_combiner(&rc);
    method(0x1760, 2 | (4 << 4) | (16 << 8));
    method(0x1784, 2 | (4 << 4) | (16 << 8));
    for (unsigned i = 0; i < 3; i++) {
        put_float(POS_AT + i * 16u, xy[i][0] * w[i]);
        put_float(POS_AT + i * 16u + 4u, xy[i][1] * w[i]);
        put_float(POS_AT + i * 16u + 8u, 0.5f * w[i]);
        put_float(POS_AT + i * 16u + 12u, w[i]);
        put32(DIFF_AT + i * 4u, 0xFFFFFFFFu);
        put_float(UV_AT + i * 16u, u[i] * q[i]);
        put_float(UV_AT + i * 16u + 4u, 0.125f * q[i]);
        put_float(UV_AT + i * 16u + 8u, 0.0f);
        put_float(UV_AT + i * 16u + 12u, q[i]);
    }
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    /* At pixel (4,3), the projected sample is 5/24 of the way from red to blue. */
    CHECK(color_near(pixel(4, 3), 0xFFCA0035u, 2));

    setup_dxt1_texture(0xFFFFu, 0, 0);
    put32(DIFF_AT, 0xFFFF0000u);
    put32(DIFF_AT + 4, 0xFF00FF00u);
    put32(DIFF_AT + 8, 0xFF0000FFu);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), 0xFFAC1E35u, 2));
}

static void extended_projective_divisor_test(unsigned kind)
{
    Nv2aCombiner rc = synthetic_combiner();
    float q = kind == 1u ? -1.0f : kind == 2u ? FLT_MIN / 2.0f : 0.0f;
    float u = kind == 1u ? -0.375f : kind == 2u ? q * 0.125f : 0.125f;
    float v = kind == 1u ? -0.125f : kind == 2u ? q * 0.125f : 0.125f;
    setup_dxt1_texture(0xF800u, 0x001Fu, (1u << 2) | (2u << 4) | (3u << 6));
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    method(0x1784, 2 | (4 << 4) | (16 << 8));
    for (unsigned i = 0; i < 3; i++) {
        put_float(UV_AT + i * 16u, u);
        put_float(UV_AT + i * 16u + 4u, v);
        put_float(UV_AT + i * 16u + 8u, 0.0f);
        put_float(UV_AT + i * 16u + 12u, q);
    }
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), kind == 1u ? 0xFF0000FFu : 0xFFFF0000u, 2));
}

static void extended_nonfinite_texture_coordinate_test(void)
{
    Nv2aCombiner rc = synthetic_combiner();
    setup_dxt1_texture(0xF800u, 0x001Fu, (1u << 2) | (2u << 4) | (3u << 6));
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    method(0x1784, 2 | (4 << 4) | (16 << 8));
    for (unsigned i = 0; i < 3; i++) {
        put_float(UV_AT + i * 16u, 0.375f);
        put_float(UV_AT + i * 16u + 4u, NAN);
        put_float(UV_AT + i * 16u + 8u, 0.0f);
        put_float(UV_AT + i * 16u + 12u, 1.0f);
    }
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xFFFF0000u, 2, "non-finite texture coordinate", 0);
}

static void extended_blend_test(void)
{
    Nv2aCombiner rc = synthetic_combiner();
    setup_dxt1_texture(0xF800u, 0x001Fu, 0);
    setup_combiner(&rc);
    triangle(0x80C86432u);
    method(0x0304, 1);
    method(0x0344, 0x302);
    method(0x0348, 0x303);
    clear(0xFF204060u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), 0xBF742030u, 1));

    clear(0xFF204060u);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == 0xFF204060u);
    method(0x0358, 0x01010000u);
    draw(3);
    flip();
    expect_no_error();
    check_pixel_near(0xBF744060u, 1, "extended color mask", 0);

    method(0x0358, 0x01010101u);
    setup_xrgb_aa_surface();
    method(0x0344, 0x304);
    method(0x0348, 1);
    clear(0xFF204060u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(aa_pixel(8, 3) == 0x00204060u);

    method(0x0344, 0x305);
    method(0x0348, 0);
    clear(0xFF204060u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(aa_pixel(8, 3), 0x00C80000u, 1));
}

static void extended_alpha_pipeline_test(void)
{
    Nv2aCombiner rc = synthetic_combiner();
    triangle(0x8064C832u);
    clear(0xFF102030u);
    draw(3); /* Populate a legacy untextured pipeline before the extended one. */
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == 0x8064C832u);

    setup_dxt1_texture(0xF800u, 0x001Fu, 0);
    setup_combiner(&rc);
    method(0x0300, 1);
    method(0x033C, 0x203);
    method(0x0340, 0x80);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(color_near(pixel(4, 3), 0x80640000u, 1));

    method(0x033C, 0x204);
    clear(0xFF102030u);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(4, 3) == 0xFF102030u);
}

static void extended_rejection_test(const char *which)
{
    const uint32_t format = (2u << 24) | (2u << 20) | (1u << 16) | (0x0Cu << 8) | (2u << 4);
    Nv2aCombiner rc = synthetic_combiner();
    const char *expected = "register-combiner";
    reset_capture();
    setup_dxt1_texture(0xF800u, 0x001Fu, 0);
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    clear(0xFF102030u);

    if (!strcmp(which, "extended-reject-stage-count"))
        method(0x1E60, 9);
    else if (!strcmp(which, "extended-reject-late-stage")) {
        method(0x1E60, 2);
        method(0x0AC4, RC_WORD(6, 8, 0, 0));
    }
    else if (!strcmp(which, "extended-reject-control"))
        method(0x1E60, 0x201);
    else if (!strcmp(which, "extended-reject-rgb-mapping"))
        method(0x1E40, (12u << 4) | (0x28u << 12));
    else if (!strcmp(which, "extended-reject-alpha-mapping"))
        method(0x0AA0, (12u << 4) | (0x38u << 12));
    else if (!strcmp(which, "extended-reject-input"))
        method(0x0AC0, RC_WORD(6, 8, 0, 0));
    else if (!strcmp(which, "extended-reject-destination"))
        method(0x1E40, 4u << 4);
    else if (!strcmp(which, "extended-reject-duplicate"))
        method(0x1E40, (12u << 4) | 12u);
    else if (!strcmp(which, "extended-reject-rgb-flags"))
        method(0x1E40, (12u << 4) | (1u << 20));
    else if (!strcmp(which, "extended-reject-alpha-flags"))
        method(0x0AA0, (12u << 4) | (1u << 18));
    else if (!strcmp(which, "extended-reject-final-special"))
        method(0x0288, RC_WORD(0, 0, 0, 6));
    else if (!strcmp(which, "extended-reject-final-self-reference"))
        method(0x028C, RC_WORD(15, 0, 0x1C, 0x80));
    else if (!strcmp(which, "extended-reject-final-flags"))
        method(0x028C, rc.final1 | 1u);
    else if (!strcmp(which, "extended-reject-program"))
        method(0x1E70, 2);
    else if (!strcmp(which, "extended-reject-later-program"))
        method(0x1E70, 1u | (2u << 10));
    else {
        expected = "extended texture";
        if (!strncmp(which, "extended-reject-stage1-", 23)) {
            setup_dxt1_texture_stage(1, TEX_AT + 8u, 0x07E0u, 0, 0);
            method(0x1E70, 1u | (1u << 5));
            if (!strcmp(which, "extended-reject-stage1-format"))
                method(0x1B44, (format & ~(0xFFu << 8)) | (0x0Du << 8));
            else if (!strcmp(which, "extended-reject-stage1-alias")) {
                expected = "active colour surface";
                method(0x1B40, COLOR_AT);
            } else if (!strcmp(which, "extended-reject-stage1-projective")) {
                expected = "projective divisor";
                method(0x1748, UV_AT + 0x100u);
                method(0x1788, 2 | (4 << 4) | (16 << 8));
                for (unsigned i = 0; i < 3; i++) {
                    put_float(UV_AT + 0x100u + i * 16u, 0.25f);
                    put_float(UV_AT + 0x104u + i * 16u, 0.25f);
                    put_float(UV_AT + 0x108u + i * 16u, 0.0f);
                    put_float(UV_AT + 0x10Cu + i * 16u, INFINITY);
                }
            } else {
                CHECK(0 && "unknown stage-1 rejection test");
                return;
            }
        } else if (!strcmp(which, "extended-reject-format"))
            method(0x1B04, (format & ~(0xFFu << 8)) | (0x0Du << 8));
        else if (!strcmp(which, "extended-reject-dxt3-span") ||
                 !strcmp(which, "extended-reject-dxt5-span")) {
            uint32_t color = strstr(which, "dxt3") ? 0x0Eu : 0x0Fu;
            setup_dxt_alpha_texture_stage(0, TEX_AT, color);
            method(0x1B00, MEM_BYTES - 32u);
        }
        else if (!strcmp(which, "extended-reject-dxt5-stage3-alias")) {
            setup_dxt_alpha_texture_stage(3, TEX_AT, 0x0Fu);
            method(0x1E70, 1u << 15);
            method(0x1BC0, COLOR_AT - 32u);
            expected = "active colour surface";
        }
        else if (!strcmp(which, "extended-reject-cube-span") ||
                 !strcmp(which, "extended-reject-cube-layout")) {
            static const uint16_t faces[6] = {
                0xF800u, 0x07E0u, 0x001Fu, 0xFFFFu, 0xFFE0u, 0x07FFu
            };
            setup_dxt1_cube_texture_stage(2, TEX_AT + 0x1000u, faces);
            method(0x1E70, 1u | (0x11u << 5) | (0x0Bu << 10));
            if (strstr(which, "span")) {
                method(0x1B80, MEM_BYTES - 128u);
                expected = "texture guest-memory span";
            } else
                method(0x1B84, (2u << 24) | (3u << 20) | (1u << 16) |
                               (0x0Cu << 8) | (2u << 4) | 4u);
        }
        else if (!strcmp(which, "extended-reject-mips"))
            method(0x1B04, (format & ~(0xFu << 16)) | (4u << 16));
        else if (!strcmp(which, "extended-reject-mip-limit"))
            method(0x1B04, (2u << 24) | (12u << 20) | (13u << 16) |
                             (0x0Cu << 8) | (2u << 4));
        else if (!strcmp(which, "extended-reject-swizzled-span") ||
                 !strcmp(which, "extended-reject-swizzled-alias")) {
            unsigned stage = strstr(which, "alias") ? 3u : 0u;
            setup_swizzled_texture_stage(stage, TEX_AT, 0x06u, 8, 8, 4, 1);
            method(0x1E70, 1u << (stage * 5u));
            if (stage == 3u) {
                method(0x1BC0, COLOR_AT - 336u);
                expected = "active colour surface";
            } else
                method(0x1B00, MEM_BYTES - 336u);
        }
        else if (!strcmp(which, "extended-reject-swizzled-alignment")) {
            setup_swizzled_texture_stage(0, TEX_AT, 0x05u, 8, 8, 1, 0);
            method(0x1B00, TEX_AT + 1u);
        }
        else if (!strcmp(which, "extended-reject-mip-span") ||
                 !strcmp(which, "extended-reject-mip-stage3-alias")) {
            setup_dxt_mip_texture_stage(3, TEX_AT, 0x0Fu, 6u, 8u, 8u);
            method(0x1E70, 1u << 15);
            if (strstr(which, "alias")) {
                method(0x1BC0, COLOR_AT - 96u);
                expected = "active colour surface";
            } else
                method(0x1BC0, MEM_BYTES - 96u);
        }
        else if (!strcmp(which, "extended-reject-mip-lod-bias") ||
                 !strcmp(which, "extended-reject-mip-lod-min") ||
                 !strcmp(which, "extended-reject-mip-lod-max")) {
            setup_dxt_mip_texture_stage(0, TEX_AT, 0x0Cu, 6u, 8u, 8u);
            expected = "extended texture mip LOD state";
            if (strstr(which, "bias"))
                method(0x1B14, (2u << 24) | (6u << 16) | 1u);
            else if (strstr(which, "-min"))
                method(0x1B0C, (1u << 30) | (1u << 18) | (3u << 6));
            else
                method(0x1B0C, (1u << 30) | (2u << 6));
        }
        else if (!strcmp(which, "extended-reject-dimensions"))
            method(0x1B04, (format & ~(0xFu << 20)) | (1u << 20));
        else if (!strcmp(which, "extended-reject-address"))
            method(0x1B08, 0x00030300u);
        else if (!strcmp(which, "extended-reject-border"))
            method(0x1B08, 0x00030403u);
        else if (!strcmp(which, "extended-reject-filter"))
            method(0x1B14, (3u << 24) | (6u << 16));
        else if (!strncmp(which, "extended-reject-conv-", 21)) {
            uint32_t kernel = strstr(which, "quincunx") ? 1u
                            : strstr(which, "gaussian") ? 2u : 7u;
            uint32_t min = strstr(which, "-conv-mag-") ? 6u : 7u;
            uint32_t mag = strstr(which, "-conv-min-") ? 2u : 4u;
            expected = "unsupported extended texture convolution filter";
            method(0x1B14, (mag << 24) | (min << 16) | (kernel << 13));
        }
        else if (!strcmp(which, "extended-reject-signed"))
            method(0x1B14, (1u << 31) | (2u << 24) | (6u << 16));
        else if (!strcmp(which, "extended-reject-disabled"))
            method(0x1B0C, 0);
        else if (!strcmp(which, "extended-reject-alphakill"))
            method(0x1B0C, (1u << 30) | (1u << 2));
        else if (!strcmp(which, "extended-reject-colorkey"))
            method(0x1B0C, (1u << 30) | 1u);
        else if (!strcmp(which, "extended-reject-anisotropy"))
            method(0x1B0C, (1u << 30) | (1u << 4));
        else if (!strcmp(which, "extended-reject-span"))
            method(0x1B00, MEM_BYTES);
        else if (!strcmp(which, "extended-reject-alignment"))
            method(0x1B00, TEX_AT + 4u);
        else if (!strcmp(which, "extended-reject-linear-pitch") ||
                 !strcmp(which, "extended-reject-linear-span") ||
                 !strcmp(which, "extended-reject-linear-alignment")) {
            setup_linear_texture(TEX_AT, 0x1Eu, 4u, 4u, 24u, 0x01032000u);
            if (strstr(which, "pitch")) {
                method(0x1B10, 12u << 16);
                expected = "extended texture dimensions";
            } else {
                method(0x1B00, strstr(which, "span") ? MEM_BYTES - 16u : TEX_AT + 1u);
                expected = "texture guest-memory span";
            }
        }
        else if (!strcmp(which, "extended-reject-active-alias")) {
            expected = "active colour surface";
            method(0x1B00, COLOR_AT);
        } else if (!strcmp(which, "extended-reject-depth-alias")) {
            expected = "depth/stencil surface";
            setup_depth(ZETA_AT);
            depth_clear(3, 0xFFFFFF00u);
            method(0x1B00, ZETA_AT);
        } else if (!strncmp(which, "extended-reject-projective-", 27)) {
            float q = !strcmp(which, "extended-reject-projective-overflow")
                    ? FLT_MIN : INFINITY;
            expected = "projective divisor";
            method(0x1784, 2 | (4 << 4) | (16 << 8));
            for (unsigned i = 0; i < 3; i++) {
                put_float(UV_AT + i * 16u,
                          !strcmp(which, "extended-reject-projective-overflow") ? 16.0f : 0.25f);
                put_float(UV_AT + i * 16u + 4u, 0.25f);
                put_float(UV_AT + i * 16u + 8u, 0.0f);
                put_float(UV_AT + i * 16u + 12u, q);
            }
        } else {
            CHECK(0 && "unknown extended rejection test");
            return;
        }
    }

    draw(3);
    flip();
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), expected));
    CHECK(s_present_count == 0 && s_framebuffer_info_count == 0 && s_ab_frame == 0);
    CHECK(pixel(4, 3) == 0xA5A5A5A5u);
}

static void framebuffer_info_test(int immediate, int passthrough)
{
    uint32_t selected_va = COLOR_AT;
    reset_capture();
    setup_xrgb_aa_surface();
    put32(COLOR_AT, 0xA1745230u);
    if (passthrough == 1) {
        set_clear_rect(WIDTH, HEIGHT, WIDTH + 1, HEIGHT + 1);
        clear(0xFF214365u);
    } else if (passthrough == 2) {
        method(0x0214, ZETA_AT);
        depth_clear(3, 0x456789A7u);
    } else {
        clear(0xFF214365u);
    }
    if (passthrough == 2) {
        selected_va = COLOR_AT + AA_PITCH * HEIGHT;
        put32(selected_va, 0xA1745230u);
        method(0x0210, selected_va);
    } else {
        method(0x0208, 0x0008);
        method(0x0210, COLOR_AT + AA_PITCH * HEIGHT);
        method(0x020C, 0);
    }
    if (immediate)
        method(0x0100, 0x300);
    else
        flip();
    expect_no_error();
    CHECK(s_framebuffer_info_count == 1 && s_present_count == 1);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_DESCRIPTOR &&
          s_events[1] == EVENT_PRESENT);
    CHECK(s_framebuffer_info.guest_va == selected_va && s_framebuffer_info.pitch == AA_PITCH);
    CHECK(s_framebuffer_info.width == AA_WIDTH && s_framebuffer_info.height == HEIGHT);
    CHECK(s_framebuffer_info.pixel_format == BW_METAL_PIXEL_BGRX8_ZERO);
    CHECK(s_framebuffer_info.aa_sx == 2 && s_framebuffer_info.aa_sy == 1);
    CHECK(s_present_va == s_framebuffer_info.guest_va && s_present_pitch == s_framebuffer_info.pitch);
    CHECK(s_framebuffer_info_pixel == (passthrough ? 0xA1745230u : 0x00214365u));
    CHECK(s_present_pixel == s_framebuffer_info_pixel);
}

enum { SURFACE_XRGB_SINGLE, SURFACE_XRGB_AA, SURFACE_ARGB_SINGLE };

static void setup_color_mode(unsigned mode, uint32_t address)
{
    if (mode == SURFACE_XRGB_AA) {
        setup_xrgb_aa_surface();
        method(0x0210, address);
    } else if (mode == SURFACE_XRGB_SINGLE) {
        setup_xrgb_single_surface(address);
    } else {
        method(0x0208, 0x0108);
        method(0x020C, PITCH);
        method(0x0210, address);
    }
}

static void flip_mode(int immediate)
{
    if (immediate)
        method(0x0100, 0x300);
    else
        flip();
}

static float reference_xrgb_channel(const uint8_t *source, int x, int y, unsigned shift)
{
    uint32_t color;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)AA_WIDTH) x = AA_WIDTH - 1;
    if (y >= (int)HEIGHT) y = HEIGHT - 1;
    memcpy(&color, source + (size_t)y * AA_PITCH + (size_t)x * 4u, sizeof color);
    return (float)((color >> shift) & 255u) / 255.0f;
}

static uint32_t reference_legacy_gaussian(const uint8_t *source, float u, float v)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        float sum = 0.0f;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                float x = u + (float)dx - 0.5f, y = v + (float)dy - 0.5f;
                int ix = (int)floorf(x), iy = (int)floorf(y);
                float fx = x - (float)ix, fy = y - (float)iy;
                float a = reference_xrgb_channel(source, ix, iy, shift);
                float b = reference_xrgb_channel(source, ix + 1, iy, shift);
                float c = reference_xrgb_channel(source, ix, iy + 1, shift);
                float d = reference_xrgb_channel(source, ix + 1, iy + 1, shift);
                float sample = (a * (1.0f - fx) + b * fx) * (1.0f - fy) +
                               (c * (1.0f - fx) + d * fx) * fy;
                sum += sample * (float)((2 - abs(dx)) * (2 - abs(dy))) / 16.0f;
            }
        }
        result |= (uint32_t)lroundf(sum * 255.0f) << shift;
    }
    return result;
}

static void framebuffer_xrgb_convolution_approx_test(int immediate)
{
    const uint32_t destination = COLOR_AT + 0x2000u;
    const uint32_t other_at = COLOR_AT + 0x4000u;
    uint8_t source[AA_PITCH * HEIGHT], other[PITCH * HEIGHT];
    Nv2aCombiner rc = synthetic_combiner();
    static const struct { float u, v; uint32_t expected; } cases[] = {
        {6.5f, 3.5f, 0x00705888u}, {8.5f, 4.5f, 0x0060645Cu},
        {0.5f, 0.5f, 0x00303018u}, {-3.0f, 5.5f, 0x00404040u},
        {AA_WIDTH - 0.5f, HEIGHT - 0.5f, 0x00505018u},
        {AA_WIDTH + 3.0f, HEIGHT + 3.0f, 0x00404000u},
        {7.0f, 4.0f, 0x00576984u}, {15.0f, 6.0f, 0x00606060u},
    };

    reset_capture();
    memset(s_mem + COLOR_AT, 0x79, AA_PITCH * HEIGHT);
    memset(s_mem + destination, 0x6B, PITCH * HEIGHT);
    memset(s_mem + other_at, 0x37, PITCH * HEIGHT);
    memcpy(other, s_mem + other_at, sizeof other);
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < AA_WIDTH; x++)
            put32(COLOR_AT + y * AA_PITCH + x * 4u,
                  ((0x41u + x + y) << 24) | (((x + 2u * y) & 3u) * 64u << 16) |
                  (((2u * x + y) & 3u) * 64u << 8) | (((x ^ y) & 3u) * 64u));
    memcpy(source, s_mem + COLOR_AT, sizeof source);
    setup_xrgb_aa_surface();
    set_clear_rect(3, 3, 3, 3);
    clear(0xFF4080C0u);
    CHECK(memcmp(source, s_mem + COLOR_AT, sizeof source) == 0);
    setup_xrgb_single_surface(destination);
    flip_mode(immediate);
    expect_no_error();
    CHECK(s_present_count == 1 && s_framebuffer_info_count == 1 && s_ab_frame == 1);
    CHECK(s_framebuffer_info.guest_va == COLOR_AT && s_framebuffer_info.aa_sx == 2);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_DESCRIPTOR && s_events[1] == EVENT_PRESENT);
    CHECK(aa_pixel(6, 3) == 0x004080C0u && aa_pixel(7, 3) == 0x004080C0u);
    CHECK(memcmp(other, s_mem + other_at, sizeof other) == 0);
    memcpy(source, s_mem + COLOR_AT, sizeof source);
    s_probe_x = 4;
    s_probe_y = 3;
    s_capture_probe = 1;
    set_clear_rect(0, 0, WIDTH - 1u, HEIGHT - 1u);
    setup_xrgb_convolution_texture(COLOR_AT, (4u << 24) | (7u << 16) | (1u << 13));
    setup_combiner(&rc);

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (i == 6u) {
            put32(COLOR_AT + 4u * AA_PITCH + 7u * 4u, 0x5540C080u);
            memcpy(source, s_mem + COLOR_AT, sizeof source);
        }
        uint32_t expected = reference_legacy_gaussian(source, cases[i].u, cases[i].v);
        CHECK(expected == cases[i].expected);
        uint32_t before = surface_pixel(destination, PITCH, 4, 3);
        setup_xrgb_single_surface(destination);
        triangle(0xFFFFFFFFu);
        if (i == 7u) {
            method(0x1784, 2 | (4 << 4) | (16 << 8));
            for (unsigned j = 0; j < 3; j++) {
                put_float(UV_AT + j * 16u, 2.0f * cases[i].u);
                put_float(UV_AT + j * 16u + 4u, 2.0f * cases[i].v);
                put_float(UV_AT + j * 16u + 8u, 0.0f);
                put_float(UV_AT + j * 16u + 12u, 2.0f);
            }
        } else {
            set_triangle_uv(cases[i].u, cases[i].v);
        }
        clear(0xFF010203u);
        draw(3);
        CHECK(surface_pixel(destination, PITCH, 4, 3) == before);
        CHECK(memcmp(source, s_mem + COLOR_AT, sizeof source) == 0);
        setup_xrgb_single_surface(other_at);
        flip_mode(immediate);
        expect_no_error();
        CHECK(surface_pixel(destination, PITCH, 4, 3) == expected);
        CHECK(surface_pixel(destination, PITCH, 0, 0) == 0x00010203u);
        CHECK(memcmp(source, s_mem + COLOR_AT, sizeof source) == 0);
        CHECK(memcmp(other, s_mem + other_at, sizeof other) == 0);
        CHECK(s_framebuffer_info_count == i + 2u && s_present_count == (int)i + 2);
        CHECK(s_framebuffer_info.guest_va == destination && s_present_va == destination);
        CHECK(s_framebuffer_info.pitch == PITCH && s_present_pitch == PITCH);
        CHECK(s_framebuffer_info.pixel_format == BW_METAL_PIXEL_BGRX8_ZERO);
        CHECK(s_framebuffer_info.aa_sx == 1 && s_framebuffer_info.aa_sy == 1);
        CHECK(s_framebuffer_info_pixel == 0x00010203u && s_present_pixel == 0x00010203u);
        CHECK(s_framebuffer_probe_pixel == expected && s_present_probe_pixel == expected);
        CHECK(s_event_count == (int)(2u * (i + 2u)) &&
              s_events[2u * (i + 1u)] == EVENT_DESCRIPTOR &&
              s_events[2u * (i + 1u) + 1u] == EVENT_PRESENT);
        int padding_ok = 1;
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = WIDTH * 4u; x < PITCH; x++)
                padding_ok &= s_mem[destination + y * PITCH + x] == 0x6B;
        CHECK(padding_ok);
    }

    rc.color_icw[0] = RC_WORD(4, 0x18, 0, 0);
    setup_combiner(&rc);
    setup_xrgb_single_surface(destination);
    method(0x1784, 2 | (2 << 4) | (8 << 8));
    triangle(0xFFFFFFFFu);
    set_triangle_uv(7.0f, 4.0f);
    clear(0xFF010203u);
    draw(3);
    flip_mode(immediate);
    expect_no_error();
    CHECK(surface_pixel(destination, PITCH, 4, 3) == 0x00FFFFFFu);
    CHECK(memcmp(source, s_mem + COLOR_AT, sizeof source) == 0);
}

static void framebuffer_xrgb_transition_test(unsigned from, unsigned to, int immediate)
{
    const uint32_t next_at = COLOR_AT + 0x2000u;
    const uint32_t source = 0x8064C832u;
    uint32_t from_pitch = from == SURFACE_XRGB_AA ? AA_PITCH : PITCH;
    uint32_t to_pitch = to == SURFACE_XRGB_AA ? AA_PITCH : PITCH;
    uint32_t from_aa = from == SURFACE_XRGB_AA ? 2u : 1u;
    uint32_t to_aa = to == SURFACE_XRGB_AA ? 2u : 1u;
    uint32_t from_format = from == SURFACE_ARGB_SINGLE
                         ? BW_METAL_PIXEL_BGRA8 : BW_METAL_PIXEL_BGRX8_ZERO;
    uint32_t to_format = to == SURFACE_ARGB_SINGLE
                       ? BW_METAL_PIXEL_BGRA8 : BW_METAL_PIXEL_BGRX8_ZERO;
    uint32_t from_clear = from == SURFACE_ARGB_SINGLE ? 0xFF214365u : 0x00214365u;
    uint32_t from_draw = from == SURFACE_ARGB_SINGLE ? source : source & 0x00FFFFFFu;
    uint32_t to_draw = to == SURFACE_ARGB_SINGLE ? source : source & 0x00FFFFFFu;

    reset_capture();
    memset(s_mem + COLOR_AT, 0xA5, from_pitch * HEIGHT);
    memset(s_mem + next_at, 0x6B, to_pitch * HEIGHT);
    setup_color_mode(from, COLOR_AT);
    clear(0xFF214365u);
    triangle(source);
    draw(3);
    setup_color_mode(to, next_at);
    CHECK(surface_pixel(COLOR_AT, from_pitch, 0, 0) == 0xA5A5A5A5u);
    CHECK(surface_pixel(COLOR_AT, from_pitch, 4u * from_aa, 3) == 0xA5A5A5A5u);
    CHECK(surface_pixel(next_at, to_pitch, 0, 0) == 0x6B6B6B6Bu);

    flip_mode(immediate);
    expect_no_error();
    CHECK(s_framebuffer_info_count == 1 && s_present_count == 1);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_DESCRIPTOR && s_events[1] == EVENT_PRESENT);
    CHECK(s_framebuffer_info.guest_va == COLOR_AT && s_framebuffer_info.pitch == from_pitch);
    CHECK(s_framebuffer_info.width == WIDTH * from_aa && s_framebuffer_info.height == HEIGHT);
    CHECK(s_framebuffer_info.pixel_format == from_format);
    CHECK(s_framebuffer_info.aa_sx == from_aa && s_framebuffer_info.aa_sy == 1);
    CHECK(s_framebuffer_info_pixel == from_clear && s_present_pixel == from_clear);
    CHECK(s_present_va == COLOR_AT && s_present_pitch == from_pitch);
    CHECK(surface_pixel(COLOR_AT, from_pitch, 4u * from_aa, 3) == from_draw);
    CHECK(surface_pixel(next_at, to_pitch, 0, 0) == 0x6B6B6B6Bu);

    triangle(source);
    draw(3);
    flip_mode(immediate);
    expect_no_error();
    CHECK(s_framebuffer_info_count == 2 && s_present_count == 2 && s_ab_frame == 2);
    CHECK(s_event_count == 4 && s_events[2] == EVENT_DESCRIPTOR && s_events[3] == EVENT_PRESENT);
    CHECK(s_framebuffer_info.guest_va == next_at && s_framebuffer_info.pitch == to_pitch);
    CHECK(s_framebuffer_info.width == WIDTH * to_aa && s_framebuffer_info.height == HEIGHT);
    CHECK(s_framebuffer_info.pixel_format == to_format);
    CHECK(s_framebuffer_info.aa_sx == to_aa && s_framebuffer_info.aa_sy == 1);
    CHECK(s_framebuffer_info_pixel == 0x6B6B6B6Bu && s_present_pixel == 0x6B6B6B6Bu);
    CHECK(s_present_va == next_at && s_present_pitch == to_pitch);
    CHECK(surface_pixel(next_at, to_pitch, 4u * to_aa, 3) == to_draw);
    CHECK(surface_pixel(COLOR_AT, from_pitch, 0, 0) == from_clear);
    CHECK(surface_pixel(COLOR_AT, from_pitch, 4u * from_aa, 3) == from_draw);
}

static void framebuffer_xrgb_single_passthrough_test(int scenario)
{
    reset_capture();
    setup_xrgb_single_surface(COLOR_AT);
    put32(COLOR_AT, 0xA1745230u);
    if (scenario == 1) {
        set_clear_rect(WIDTH, HEIGHT, WIDTH + 1, HEIGHT + 1);
        clear(0xFF214365u);
        method(0x0208, 0x0108);
        method(0x0210, COLOR_AT + PITCH * HEIGHT);
        method(0x020C, 0);
    } else if (scenario == 2) {
        method(0x0208, 0x0124u);
        method(0x020C, (PITCH << 16) | PITCH);
        method(0x0214, ZETA_AT);
        depth_clear(3, 0x456789A7u);
    }
    flip();
    expect_no_error();
    CHECK(s_framebuffer_info_count == 1 && s_present_count == 1 && s_ab_frame == 1);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_DESCRIPTOR && s_events[1] == EVENT_PRESENT);
    CHECK(s_framebuffer_info.guest_va == COLOR_AT && s_framebuffer_info.pitch == PITCH);
    CHECK(s_framebuffer_info.width == WIDTH && s_framebuffer_info.height == HEIGHT);
    CHECK(s_framebuffer_info.pixel_format == BW_METAL_PIXEL_BGRX8_ZERO);
    CHECK(s_framebuffer_info.aa_sx == 1 && s_framebuffer_info.aa_sy == 1);
    CHECK(s_framebuffer_info_pixel == 0xA1745230u && s_present_pixel == 0xA1745230u);
}

static void framebuffer_xrgb_convolution_reject_test(unsigned min, unsigned mag, int immediate)
{
    const uint32_t next_at = COLOR_AT + 0x2000u;
    uint8_t source[AA_PITCH * HEIGHT], destination[PITCH * HEIGHT];
    BwMetalFramebuffer descriptor;
    Nv2aCombiner rc = synthetic_combiner();
    reset_capture();
    s_probe_x = 8;
    s_probe_y = 3;
    s_capture_probe = 1;
    memset(s_mem + next_at, 0x6B, PITCH * HEIGHT);
    setup_xrgb_aa_surface();
    clear(0xFF214365u);
    triangle(0x8064C832u);
    draw(3);
    setup_xrgb_single_surface(next_at);
    flip_mode(immediate);
    expect_no_error();
    CHECK(s_present_count == 1 && s_framebuffer_info_count == 1 && s_ab_frame == 1);
    CHECK(s_framebuffer_info.guest_va == COLOR_AT && s_framebuffer_info.aa_sx == 2);
    CHECK(aa_pixel(8, 3) == 0x0064C832u);
    CHECK(s_framebuffer_probe_pixel == 0x0064C832u && s_present_probe_pixel == 0x0064C832u);

    memcpy(source, s_mem + COLOR_AT, sizeof source);
    memcpy(destination, s_mem + next_at, sizeof destination);
    descriptor = s_framebuffer_info;
    setup_xrgb_convolution_texture(COLOR_AT, (mag << 24) | (min << 16) | (1u << 13));
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    draw(3);
    CHECK(bw_metal_backend_error() &&
          strstr(bw_metal_backend_error(), "unsupported extended texture convolution filter (quincunx)"));
    flip_mode(immediate);
    CHECK(s_present_count == 1 && s_framebuffer_info_count == 1 && s_ab_frame == 1);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_DESCRIPTOR && s_events[1] == EVENT_PRESENT);
    CHECK(memcmp(&descriptor, &s_framebuffer_info, sizeof descriptor) == 0);
    CHECK(s_framebuffer_probe_pixel == 0x0064C832u && s_present_probe_pixel == 0x0064C832u);
    CHECK(memcmp(source, s_mem + COLOR_AT, sizeof source) == 0);
    CHECK(memcmp(destination, s_mem + next_at, sizeof destination) == 0);
    CHECK(surface_pixel(next_at, PITCH, 4, 3) == 0x6B6B6B6Bu);
    CHECK(aa_pixel(8, 3) == 0x0064C832u);
}

static void quincunx_approx_rejection_test(const char *which)
{
    const uint32_t destination = COLOR_AT + 0x2000u;
    const uint32_t format = (1u << 16) | (0x1Eu << 8) | (2u << 4);
    uint8_t source[AA_PITCH * HEIGHT], target[PITCH * HEIGHT];
    Nv2aCombiner rc = synthetic_combiner();
    const char *expected = "unsupported quincunx approximation";
    reset_capture();
    memset(s_mem + destination, 0x6B, sizeof target);
    setup_xrgb_single_surface(destination);
    setup_xrgb_convolution_texture(COLOR_AT, (4u << 24) | (7u << 16) | (1u << 13));
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);

    if (!strcmp(which, "quincunx-approx-reject-format"))
        method(0x1B04, (format & ~(0xFFu << 8)) | (0x12u << 8));
    else if (!strcmp(which, "quincunx-approx-reject-cube"))
        method(0x1B04, format | 4u);
    else if (!strcmp(which, "quincunx-approx-reject-mips"))
        method(0x1B04, (format & ~(15u << 16)) | (2u << 16));
    else if (!strcmp(which, "quincunx-approx-reject-wrap"))
        method(0x1B08, 0x00030301u);
    else if (!strcmp(which, "quincunx-approx-reject-signed"))
        method(0x1B14, (1u << 31) | (4u << 24) | (7u << 16) | (1u << 13));
    else if (!strcmp(which, "quincunx-approx-reject-disabled"))
        method(0x1B0C, 0);
    else if (!strcmp(which, "quincunx-approx-reject-target-argb"))
        setup_color_mode(SURFACE_ARGB_SINGLE, destination);
    else if (!strcmp(which, "quincunx-approx-reject-target-aa"))
        setup_color_mode(SURFACE_XRGB_AA, destination);
    else if (!strcmp(which, "quincunx-approx-reject-blend")) {
        expected = "blending with quincunx approximation is unsupported";
        blend_state(1, 0, 0x8006u, 0);
    }
    else if (!strcmp(which, "quincunx-approx-reject-width"))
        method(0x1B1C, ((AA_WIDTH - 1u) << 16) | HEIGHT);
    else if (!strcmp(which, "quincunx-approx-reject-height"))
        method(0x1B1C, (AA_WIDTH << 16) | (HEIGHT - 1u));
    else if (!strcmp(which, "quincunx-approx-reject-pitch")) {
        expected = "invalid quincunx approximation texture guest-memory span";
        method(0x1B10, (AA_WIDTH * 4u - 4u) << 16);
    } else if (!strcmp(which, "quincunx-approx-reject-unaligned")) {
        expected = "invalid quincunx approximation texture guest-memory span";
        method(0x1B00, COLOR_AT + 1u);
    } else if (!strcmp(which, "quincunx-approx-reject-span")) {
        expected = "invalid quincunx approximation texture guest-memory span";
        method(0x1B00, MEM_BYTES - 4u);
    } else if (!strcmp(which, "quincunx-approx-reject-active-alias")) {
        expected = "active colour surface";
        method(0x1B00, destination);
    } else if (!strcmp(which, "quincunx-approx-reject-depth-alias")) {
        expected = "depth/stencil surface";
        setup_depth(ZETA_AT);
        depth_clear(3, 0xFFFFFF00u);
        setup_xrgb_single_surface(destination);
        method(0x1B00, ZETA_AT);
    } else {
        CHECK(0 && "unknown quincunx approximation rejection test");
        return;
    }

    memcpy(source, s_mem + COLOR_AT, sizeof source);
    memcpy(target, s_mem + destination, sizeof target);
    draw(3);
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), expected));
    flip();
    CHECK(s_present_count == 0 && s_framebuffer_info_count == 0 && s_ab_frame == 0);
    CHECK(s_event_count == 0);
    CHECK(memcmp(source, s_mem + COLOR_AT, sizeof source) == 0);
    CHECK(memcmp(target, s_mem + destination, sizeof target) == 0);
}

static void quincunx_approx_lifecycle_test(void)
{
    const uint32_t destination = COLOR_AT + 0x2000u;
    Nv2aCombiner rc = synthetic_combiner();

    CHECK(setenv("BW_METAL_QUINCUNX_APPROX", "0", 1) == 0);
    setup_xrgb_single_surface(destination);
    setup_xrgb_convolution_texture(COLOR_AT, (4u << 24) | (7u << 16) | (1u << 13));
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    draw(3);
    flip();
    expect_no_error();

    bw_metal_backend_stop();
    CHECK(bw_metal_backend_start_for_test(s_mem, sizeof s_mem));
    setup_surface(1);
    setup_xrgb_single_surface(destination);
    setup_xrgb_convolution_texture(COLOR_AT, (4u << 24) | (7u << 16) | (1u << 13));
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    CHECK(setenv("BW_METAL_QUINCUNX_APPROX", "1", 1) == 0);
    draw(3);
    CHECK(bw_metal_backend_error() &&
          strstr(bw_metal_backend_error(), "unsupported extended texture convolution filter (quincunx)"));

    bw_metal_backend_stop();
    CHECK(bw_metal_backend_start_for_test(s_mem, sizeof s_mem));
    setup_surface(1);
    setup_xrgb_single_surface(destination);
    setup_xrgb_convolution_texture(COLOR_AT, (4u << 24) | (7u << 16) | (1u << 13));
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    draw(3);
    flip();
    expect_no_error();
}

static void quincunx_approx_reference_sweep_test(void)
{
    const uint32_t destination = COLOR_AT + 0x2000u;
    uint8_t source[AA_PITCH * HEIGHT];
    Nv2aCombiner rc = synthetic_combiner();
    unsigned mismatches = 0;
    memset(source, 0x79, sizeof source);
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < AA_WIDTH; x++) {
            uint32_t value = ((x * 19u + y * 31u) & 255u) << 24 |
                             ((x * 37u + y * 17u) & 255u) << 16 |
                             ((x * 13u + y * 43u) & 255u) << 8 |
                             ((x * 53u + y * 7u) & 255u);
            memcpy(source + y * AA_PITCH + x * 4u, &value, sizeof value);
        }
    memcpy(s_mem + COLOR_AT, source, sizeof source);
    setup_xrgb_single_surface(destination);
    setup_xrgb_convolution_texture(COLOR_AT, (4u << 24) | (7u << 16) | (1u << 13));
    setup_combiner(&rc);
    for (unsigned i = 0; i < 4096; i++) {
        float u = (float)((i * 193u + 37u) % 3700u) / 100.0f - 2.0f;
        float v = (float)((i * 97u + 11u) % 1700u) / 100.0f - 2.0f;
        uint32_t expected = reference_legacy_gaussian(source, u, v);
        triangle(0xFFFFFFFFu);
        set_triangle_uv(u, v);
        draw(3);
        flip();
        expect_no_error();
        uint32_t actual = surface_pixel(destination, PITCH, 4, 3);
        if (actual != expected) {
            if (mismatches < 4)
                fprintf(stderr, "Synthetic sample %u: expected %08X, got %08X\n",
                        i, expected, actual);
            mismatches++;
        }
    }
    if (mismatches)
        fprintf(stderr, "Legacy Gaussian sweep mismatched %u synthetic samples\n", mismatches);
    CHECK(mismatches == 0);
    CHECK(memcmp(source, s_mem + COLOR_AT, sizeof source) == 0);
}

static void framebuffer_info_stale_test(void)
{
    reset_capture();
    setup_xrgb_aa_surface();
    clear(0xFF214365u);
    flip();
    expect_no_error();
    put32(COLOR_AT, 0xA1765432u);
    method(0x0208, 0x0128);
    method(0x020C, (PITCH << 16) | PITCH);
    method(0x0210, COLOR_AT + AA_PITCH * HEIGHT);
    method(0x0214, ZETA_AT);
    depth_clear(3, 0x456789A7u);
    flip();
    expect_no_error();
    CHECK(s_framebuffer_info_count == 2 && s_present_count == 2);
    CHECK(s_framebuffer_info.guest_va == COLOR_AT && s_framebuffer_info.pitch == AA_PITCH);
    CHECK(s_framebuffer_info.width == AA_WIDTH && s_framebuffer_info.height == HEIGHT);
    CHECK(s_framebuffer_info.pixel_format == BW_METAL_PIXEL_BGRX8_ZERO &&
          s_framebuffer_info.aa_sx == 2 && s_framebuffer_info.aa_sy == 1);
    CHECK(s_framebuffer_info_pixel == 0xA1765432u && s_present_pixel == 0xA1765432u);
}

static void framebuffer_info_reject_test(void)
{
    reset_capture();
    setup_xrgb_aa_surface();
    s_framebuffer_info_accept = 0;
    clear(0xFF214365u);
    flip();
    CHECK(bw_metal_backend_error() &&
          strstr(bw_metal_backend_error(), "presenter rejected framebuffer descriptor"));
    CHECK(s_framebuffer_info_count == 1 && s_present_count == 0 && s_ab_frame == 0);
    CHECK(s_event_count == 1 && s_events[0] == EVENT_DESCRIPTOR);
    CHECK(s_framebuffer_info_pixel == 0x00214365u);
}

static void framebuffer_info_lifecycle_test(void)
{
    reset_capture();
    clear(0xFF214365u);
    flip();
    expect_no_error();
    CHECK(s_framebuffer_info_count == 1);
    CHECK(s_framebuffer_info.pixel_format == BW_METAL_PIXEL_BGRA8 &&
          s_framebuffer_info.width == WIDTH && s_framebuffer_info.aa_sx == 1);
    bw_metal_backend_stop();
    CHECK(bw_metal_backend_start_for_test(s_mem, sizeof s_mem));
    clear(0xFF123456u);
    flip();
    expect_no_error();
    CHECK(s_framebuffer_info_count == 1);
    bw_metal_backend_stop();
    bw_metal_backend_set_framebuffer_callback(capture_framebuffer_info);
    CHECK(bw_metal_backend_start_for_test(s_mem, sizeof s_mem));
    clear(0xFF789ABCu);
    flip();
    expect_no_error();
    CHECK(s_framebuffer_info_count == 2);
}

static atomic_int s_error_poll_started, s_error_poll_done, s_error_poll_seen;

static void *poll_error(void *unused)
{
    (void)unused;
    for (;;) {
        int done = atomic_load(&s_error_poll_done);
        const char *error = bw_metal_backend_error();
        if (error && strstr(error, "unsupported depth"))
            atomic_store(&s_error_poll_seen, 1);
        atomic_store(&s_error_poll_started, 1);
        if (done)
            break;
    }
    return NULL;
}

static void error_thread_test(void)
{
    pthread_t reader;
    if (pthread_create(&reader, NULL, poll_error, NULL)) {
        CHECK(0 && "could not start error reader");
        return;
    }
    while (!atomic_load(&s_error_poll_started))
        sched_yield();
    clear(0xFF123456u);
    triangle(0xFFFFFFFFu);
    method(0x030C, 1);
    draw(3);
    reset_capture();
    flip();
    atomic_store(&s_error_poll_done, 1);
    pthread_join(reader, NULL);
    CHECK(atomic_load(&s_error_poll_seen));
    CHECK(s_present_count == 0 && pixel(0, 0) == 0xA5A5A5A5u);
}

static int s_mock_sync_fails;
static int s_mock_remap;
static uint32_t s_mock_sync_va, s_mock_sync_pitch;
static const uint32_t s_sync_pixel = 0xFF123456u;
static const uint32_t s_flip_pixel = 0xFFC0783Au;

static void mock_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                       uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    (void)s; (void)rs; (void)flags; (void)argb; (void)zstencil;
}

static void mock_flip(void)
{
    event(EVENT_FLIP);
    put32(COLOR_AT, s_flip_pixel);
}

static int mock_sync(uint32_t color_va, uint32_t pitch)
{
    event(EVENT_SYNC);
    s_mock_sync_va = color_va;
    s_mock_sync_pitch = pitch;
    if (s_mock_sync_fails)
        return 0;
    if (color_va != COLOR_AT || pitch != PITCH)
        return 0;
    put32(color_va, s_sync_pixel);
    if (s_mock_remap)
        s_contig_high = COLOR_AT + 1;
    return 1;
}

static const Nv2aBackend s_mock_backend = {mock_clear, NULL, mock_flip};
static const Nv2aBackend s_other_backend = {mock_clear, NULL, mock_flip};

static uint32_t dumped_first_pixel(const char *path)
{
    unsigned char bgr[3] = {0};
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
        return 0;
    CHECK(fseek(f, 54 + (HEIGHT - 1) * ((WIDTH * 3 + 3) & ~3u), SEEK_SET) == 0);
    CHECK(fread(bgr, 1, sizeof bgr, f) == sizeof bgr);
    fclose(f);
    return ((uint32_t)bgr[2] << 16) | ((uint32_t)bgr[1] << 8) | bgr[0];
}

static void mock_order_test(const char *which)
{
    char prefix[128], path[144];
    int checked = strstr(which, "checked") != NULL;
    int immediate = strstr(which, "immediate") != NULL;
    int legacy_pre = strstr(which, "legacy-pre") != NULL;
    int failure = strstr(which, "failure") != NULL;
    const uint32_t old_pixel = 0xFFA0B0C0u;

    snprintf(prefix, sizeof prefix, "metal-%s-", which);
    snprintf(path, sizeof path, "%s00000.bmp", prefix);
    remove(path);
    setenv("RECOMP_FB_DUMP", prefix, 1);
    setenv("RECOMP_FB_DUMP_FLIPS", "1", 1);
    setenv("BW_DUMP_AFTER_FLIP", legacy_pre ? "0" : "1", 1);
    put32(COLOR_AT, old_pixel);
    s_mock_sync_fails = failure;
    s_mock_remap = strstr(which, "remap") != NULL;
    if (checked)
        bw_backend_register_with_present_sync(&s_mock_backend, mock_sync);
    else {
        bw_backend_register_with_present_sync(&s_mock_backend, mock_sync);
        nv2a_backend_register(&s_mock_backend);
    }
    clear(0);
    if (strstr(which, "rebind-aa")) {
        if (!checked) {
            method(0x0200, (WIDTH / 2) << 16);
            method(0x0204, (HEIGHT / 2) << 16);
        }
        method(0x0208, 0x2008);
    }
    if (checked) {
        if (strstr(which, "preflip"))
            s_contig_high = COLOR_AT + 1;
        method(0x0210, COLOR_AT + PITCH * HEIGHT);
        method(0x020C, 0);
    }
    reset_capture();
    if (immediate)
        method(0x0100, 0x300);
    else
        flip();

    if (checked) {
        CHECK(s_mock_sync_va == COLOR_AT && s_mock_sync_pitch == PITCH);
        CHECK(s_event_count == (failure ? 2 : 3));
        CHECK(s_events[0] == EVENT_SYNC);
        CHECK(s_events[failure ? 1 : 2] == EVENT_FLIP);
        if (failure) {
            FILE *f = fopen(path, "rb");
            CHECK(s_present_count == 0 && f == NULL);
            if (f)
                fclose(f);
        } else {
            CHECK(s_events[1] == EVENT_PRESENT);
            CHECK(s_present_count == 1 && s_present_va == COLOR_AT);
            CHECK(s_present_aa == 1);
            CHECK(s_present_pixel == s_sync_pixel);
            CHECK(dumped_first_pixel(path) == (s_sync_pixel & 0xFFFFFFu));
        }
    } else {
        CHECK(s_event_count == 2 && s_events[0] == EVENT_PRESENT && s_events[1] == EVENT_FLIP);
        CHECK(s_present_count == 1 && s_present_pixel == old_pixel);
        CHECK(s_present_aa == (strstr(which, "rebind-aa") ? 2 : 1));
        CHECK(dumped_first_pixel(path) ==
              ((immediate || legacy_pre ? old_pixel : s_flip_pixel) & 0xFFFFFFu));
    }
    remove(path);
}

static void registration_test(void)
{
    bw_backend_register_with_present_sync(&s_mock_backend, mock_sync);
    clear(0);
    bw_metal_backend_stop();
    reset_capture();
    flip();
    CHECK(s_event_count == 3 && s_events[0] == EVENT_SYNC &&
          s_events[1] == EVENT_PRESENT && s_events[2] == EVENT_FLIP);

    bw_backend_unregister_if_current(&s_other_backend);
    reset_capture();
    flip();
    CHECK(s_event_count == 3 && s_events[0] == EVENT_SYNC && s_events[2] == EVENT_FLIP);

    nv2a_backend_register(&s_other_backend);
    bw_backend_unregister_if_current(&s_mock_backend);
    reset_capture();
    flip();
    CHECK(s_event_count == 2 && s_events[0] == EVENT_PRESENT && s_events[1] == EVENT_FLIP);

    bw_backend_unregister_if_current(&s_other_backend);
    reset_capture();
    flip();
    CHECK(s_event_count == 1 && s_events[0] == EVENT_PRESENT);

    bw_backend_register_with_present_sync(NULL, mock_sync);
    reset_capture();
    flip();
    CHECK(s_event_count == 1 && s_events[0] == EVENT_PRESENT);

    bw_backend_register_with_present_sync(&s_mock_backend, NULL);
    reset_capture();
    flip();
    CHECK(s_event_count == 2 && s_events[0] == EVENT_PRESENT && s_events[1] == EVENT_FLIP);

    bw_backend_register_with_present_sync(&s_mock_backend, mock_sync);
    nv2a_backend_register(NULL);
    reset_capture();
    flip();
    CHECK(s_event_count == 1 && s_events[0] == EVENT_PRESENT);
}

static uint32_t read_guest32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, (void *)((uintptr_t)s_guest_base + address), sizeof value);
    return value;
}

static void production_memory_test(void)
{
    const uint32_t contig_va = 0x80000000u;
    const size_t mapping_size = (size_t)contig_va + MEM_BYTES;
    uint8_t *mapping;

    CHECK(!bw_metal_backend_start());
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), "guest memory"));
    s_runtime_base = s_mem;
    s_runtime_low_size = sizeof s_mem;
    CHECK(!bw_metal_backend_start());
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), "contiguous window"));

    mapping = mmap(NULL, mapping_size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(mapping != MAP_FAILED);
    if (mapping == MAP_FAILED)
        return;
    CHECK(mprotect(mapping, MEM_BYTES, PROT_READ | PROT_WRITE) == 0);
    CHECK(mprotect(mapping + contig_va, MEM_BYTES, PROT_READ | PROT_WRITE) == 0);
    if (s_failed) {
        munmap(mapping, mapping_size);
        return;
    }
    s_guest_base = mapping;
    s_runtime_base = mapping;
    s_runtime_low_size = MEM_BYTES;
    s_runtime_contig_va = contig_va;
    s_runtime_contig_size = MEM_BYTES;
    s_runtime_contig = mapping + contig_va + 1;
    CHECK(!bw_metal_backend_start());
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), "contiguous window"));
    s_runtime_contig = mapping + contig_va;
    s_runtime_contig_size = SIZE_MAX;
    CHECK(!bw_metal_backend_start());
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), "guest memory regions"));
    s_runtime_contig_size = MEM_BYTES;
    CHECK(bw_metal_backend_start());
    expect_no_error();

    setup_surface(1);
    reset_capture();
    clear(0xFF123456u);
    flip();
    expect_no_error();
    CHECK(s_present_count == 1 && s_present_va == COLOR_AT &&
          s_present_pixel == 0xFF123456u && read_guest32(COLOR_AT) == 0xFF123456u);

    method(0x0210, contig_va + COLOR_AT);
    clear(0xFFABCDEFu);
    flip();
    expect_no_error();
    CHECK(s_present_count == 2 && s_present_va == contig_va + COLOR_AT &&
          s_present_pixel == 0xFFABCDEFu && read_guest32(contig_va + COLOR_AT) == 0xFFABCDEFu);

    bw_metal_backend_stop();
    CHECK(bw_metal_backend_start());
    method(0x0210, 0x40000000u);
    clear(0xFF010203u);
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), "surface guest-memory span"));
    reset_capture();
    flip();
    CHECK(s_present_count == 0);

    bw_metal_backend_stop();
    CHECK(bw_metal_backend_start());
    method(0x0210, contig_va + MEM_BYTES - 4u);
    clear(0xFF010203u);
    CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), "surface guest-memory span"));
    reset_capture();
    flip();
    CHECK(s_present_count == 0);
    bw_metal_backend_stop();
    CHECK(munmap(mapping, mapping_size) == 0);
    s_guest_base = s_mem;
}

static void failure_test(const char *which)
{
    uint8_t before[PITCH * HEIGHT];
    memcpy(before, s_mem + COLOR_AT, sizeof before);
    triangle(0xFFFFFFFFu);

    if (!strcmp(which, "invalid-surface")) {
        method(0x020C, WIDTH * 4u - 4u);
        clear(0xFF000000u);
        expect_error("surface guest-memory span", before);
    } else if (!strcmp(which, "surface-extent")) {
        method(0x0200, 1u << 16);
        method(0x0204, 16385u << 16);
        method(0x020C, 4u);
        clear(0xFF000000u);
        expect_error("surface dimensions exceed", before);
    } else if (!strcmp(which, "unsupported-surface")) {
        method(0x0208, 0x0007);
        clear(0xFF000000u);
        expect_error("unsupported colour surface", before);
    } else if (!strcmp(which, "argb-with-aa") ||
               !strcmp(which, "xrgb-other-aa") || !strcmp(which, "aa-other-format") ||
               !strcmp(which, "xrgb-reserved-layout")) {
        uint32_t format = !strcmp(which, "argb-with-aa") ? 0x1108u
                        : !strcmp(which, "xrgb-other-aa") ? 0x2104u
                        : !strcmp(which, "xrgb-reserved-layout") ? 0x1024u : 0x1105u;
        method(0x0208, format);
        method(0x020C, AA_PITCH);
        clear(0xFF000000u);
        expect_error("unsupported colour surface", before);
    } else if (!strcmp(which, "unsupported-origin")) {
        method(0x0200, (WIDTH << 16) | 1u);
        clear(0xFF000000u);
        expect_error("surface clip origin", before);
    } else if (!strcmp(which, "invalid-address")) {
        method(0x0210, MEM_BYTES - 4u);
        clear(0xFF000000u);
        expect_error("surface guest-memory span", before);
    } else if (!strcmp(which, "unsupported-clear")) {
        method(0x1D94, 0x04);
        expect_error("clear flags", before);
    } else if (!strcmp(which, "incomplete-rect")) {
        method(0x1D98, 0);
        clear(0xFF000000u);
        expect_error("incomplete clear", before);
    } else {
        clear(0xFF102030u);
        if (!strcmp(which, "unsupported-state")) {
            setup_depth(ZETA_AT);
            method(0x030C, 1);
            method(0x0354, 0xFFFFu);
        } else if (!strcmp(which, "invalid-blend-src") || !strcmp(which, "invalid-blend-dst") ||
                 !strcmp(which, "invalid-blend-equation")) {
            blend_state(!strcmp(which, "invalid-blend-src") ? 0xFFFFu : 1,
                        !strcmp(which, "invalid-blend-dst") ? 0x308u : 0,
                        !strcmp(which, "invalid-blend-equation") ? 0xFFFFu : 0x8006u, 0);
        } else if (!strcmp(which, "invalid-alpha-func")) {
            method(0x0300, 1);
            method(0x033C, 0xFFFFu);
        } else if (!strcmp(which, "invalid-cull-face") || !strcmp(which, "invalid-cull-winding")) {
            method(0x0308, 1);
            method(0x039C, !strcmp(which, "invalid-cull-face") ? 0xFFFFu : 0x405u);
            method(0x03A0, !strcmp(which, "invalid-cull-winding") ? 0xFFFFu : 0x900u);
        } else if (!strcmp(which, "unsupported-fog"))
            method(0x02A4, 1);
        else if (!strcmp(which, "invalid-polygon-offset-factor")) {
            method(0x0338, 1);
            method_float(0x0384, NAN);
            method_float(0x0388, -1.0f);
        } else if (!strcmp(which, "invalid-polygon-offset-units")) {
            method(0x0338, 1);
            method_float(0x0384, -0.25f);
            method_float(0x0388, INFINITY);
        }
        else if (!strcmp(which, "unsupported-stage"))
            method(0x1B40, TEX_AT);
        else if (!strcmp(which, "unsupported-combiner"))
            method(0x1E60, 9);
        else if (!strcmp(which, "screen-combiner"))
            method(0x1E60, 0);
        else if (!strcmp(which, "unsupported-texture") || !strcmp(which, "cube-texture") ||
                 !strcmp(which, "texture-extent") ||
                 !strcmp(which, "invalid-texture") ||
                 !strcmp(which, "unaligned-texture") || !strcmp(which, "feedback")) {
            uint32_t address = !strcmp(which, "invalid-texture") ? MEM_BYTES - 4u
                             : !strcmp(which, "unaligned-texture") ? TEX_AT + 1u
                             : !strcmp(which, "feedback") ? COLOR_AT : TEX_AT;
            setup_texture(address, !strcmp(which, "unsupported-texture") ? 0x02020000u : 0x01010000u);
            if (!strcmp(which, "cube-texture"))
                method(0x1B04, 0x00101204u);
            if (!strcmp(which, "texture-extent")) {
                method(0x1B10, 4u << 16);
                method(0x1B1C, (1u << 16) | 16385u);
            }
            quad();
        }
        draw((!strcmp(which, "unsupported-texture") || !strcmp(which, "cube-texture") ||
              !strcmp(which, "texture-extent") ||
              !strcmp(which, "invalid-texture") || !strcmp(which, "unaligned-texture") ||
              !strcmp(which, "feedback")) ? 6 : 3);
        const char *expected = !strcmp(which, "unsupported-state") ? "unsupported depth comparison"
                             : !strcmp(which, "invalid-blend-src") ? "unsupported blend factor"
                             : !strcmp(which, "invalid-blend-dst") ? "unsupported destination blend factor"
                             : !strcmp(which, "invalid-blend-equation") ? "unsupported blend equation"
                             : !strcmp(which, "invalid-alpha-func") ? "unsupported alpha comparison"
                             : (!strcmp(which, "invalid-cull-face") ||
                                !strcmp(which, "invalid-cull-winding")) ? "unsupported cull"
                             : !strcmp(which, "unsupported-fog") ? "unsupported fog"
                             : (!strcmp(which, "invalid-polygon-offset-factor") ||
                                !strcmp(which, "invalid-polygon-offset-units")) ? "invalid polygon offset"
                             : !strcmp(which, "unsupported-stage") ? "texture stages"
                             : !strcmp(which, "unsupported-combiner") ? "register-combiner"
                             : !strcmp(which, "screen-combiner") ? "combiner state"
                             : (!strcmp(which, "unsupported-texture") ||
                                !strcmp(which, "cube-texture")) ? "unsupported stage-0"
                             : !strcmp(which, "texture-extent") ? "texture dimensions exceed"
                             : (!strcmp(which, "invalid-texture") ||
                                !strcmp(which, "unaligned-texture")) ? "texture guest-memory span"
                             : "pending render target";
        expect_error(expected, before);
    }
}

static void surface_switch_test(void)
{
    const uint32_t next_at = COLOR_AT + 0x2000u;
    const uint32_t first = 0xFF214365u;
    const uint32_t second = 0xFF102030u;
    const uint32_t cpu_seed = 0xFFABCDEFu;
    const uint32_t draw_color = 0xFF804020u;

    memset(s_mem + next_at, 0x6B, AA_PITCH * HEIGHT);
    clear(first);
    setup_color_mode(SURFACE_XRGB_AA, next_at);
    clear(second);
    expect_no_error();
    CHECK(pixel(0, 0) == first);
    CHECK(surface_pixel(next_at, AA_PITCH, 0, 0) == 0x6B6B6B6Bu);
    CHECK(bw_metal_test_color_surface_count() == 2u);

    put32(COLOR_AT, cpu_seed);
    setup_surface(1);
    triangle(draw_color);
    draw(3);
    expect_no_error();
    CHECK(surface_pixel(next_at, AA_PITCH, 0, 0) == (second & 0x00FFFFFFu));
    CHECK(bw_metal_test_color_surface_count() == 2u);

    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == cpu_seed);
    CHECK(pixel(4, 3) == draw_color);
}

static void extended_render_target_texture_test(void)
{
    Nv2aCombiner rc = synthetic_combiner();
    const uint32_t destination = ZETA3_AT;

    memset(s_mem + destination, 0x6B, PITCH * HEIGHT);
    setup_dxt1_texture(0xF800u, 0x001Fu, 0);
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    clear(0xFF102030u);
    method(0x1B00, COLOR_AT);
    method(0x0210, destination);
    draw(3);
    flip();
    expect_no_error();
    CHECK(pixel(0, 0) == 0xFF102030u);
    CHECK(surface_pixel(destination, PITCH, 0, 0) == 0x6B6B6B6Bu);
    CHECK(surface_pixel(destination, PITCH, 4, 3) != 0x6B6B6B6Bu);
}

static void quincunx_approx_render_target_resolve_test(void)
{
    const uint32_t destination = COLOR_AT + 0x2000u;
    Nv2aCombiner rc = synthetic_combiner();

    memset(s_mem + destination, 0x6B, PITCH * HEIGHT);
    setup_xrgb_aa_surface();
    clear(0xFF214365u);
    setup_xrgb_single_surface(destination);
    for (unsigned y = 0; y < HEIGHT; y++)
        for (unsigned x = 0; x < WIDTH; x++)
            put32(ZETA_AT + y * PITCH + x * 4u, 0xFFFFFF00u);
    method(0x0208, 0x0124u);
    method(0x020C, (PITCH << 16) | PITCH);
    method(0x0214, ZETA_AT);
    method(0x030C, 1);
    method(0x0354, 0x201u);
    method(0x035C, 1);
    setup_xrgb_convolution_texture(COLOR_AT, (4u << 24) | (7u << 16) | (1u << 13));
    setup_combiner(&rc);
    triangle(0xFFFFFFFFu);
    set_triangle_uv(6.5f, 3.5f);
    draw(3);
    flip();
    expect_no_error();
    CHECK(aa_pixel(0, 0) == 0x00214365u);
    CHECK(surface_pixel(destination, PITCH, 0, 0) == 0x6B6B6B6Bu);
    CHECK(surface_pixel(destination, PITCH, 4, 3) == 0x00214365u);
    float z;
    uint8_t stencil;
    CHECK(bw_metal_test_read_depth(ZETA_AT, 4, 3, &z, &stencil));
    CHECK(fabsf(z - 0.5f) < 0.000001f && stencil == 0);
}

static void basic_feedback_test(const char *which)
{
    uint8_t before[PITCH * HEIGHT];
    uint32_t end = COLOR_AT + (HEIGHT - 1u) * PITCH + WIDTH * 4u;
    uint32_t address = !strcmp(which, "basic-feedback-leading") ? COLOR_AT - 4u
                     : !strcmp(which, "basic-feedback-trailing") ? end - 4u
                     : !strcmp(which, "basic-feedback-adjacent") ? end
                     : COLOR_AT;

    if (!strcmp(which, "basic-feedback-adjacent"))
        for (unsigned i = 0; i < 4; i++)
            put32(address + i * 4u, 0xFF00FF00u);
    memcpy(before, s_mem + COLOR_AT, sizeof before);
    setup_texture(address, 0x01010000u);
    quad();
    draw(6);
    if (!strcmp(which, "basic-feedback-adjacent")) {
        flip();
        expect_no_error();
        CHECK(pixel(4, 4) == 0xFF00FF00u);
    } else {
        expect_error("sampling the active colour surface", before);
    }
}

static void cpu_write_counterexample_test(void)
{
    const uint32_t cpu_color = 0xFFABCDEFu;
    const uint32_t gpu_color = 0xFF102030u;
    const uint32_t address = COLOR_AT + 4u * PITCH + 4u * 4u;

    clear(gpu_color);
    expect_no_error();
    put32(address, cpu_color);
    CHECK(pixel(4, 4) == cpu_color);
    flip();
    expect_no_error();
    CHECK(pixel(4, 4) == gpu_color);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Expected one test case name\n");
        return 2;
    }
    unsetenv("RECOMP_NO_COMBINERS");
    unsetenv("RECOMP_FB_DUMP");
    unsetenv("RECOMP_FB_DUMP_FLIPS");
    setenv("BW_IMMEDIATE_PRESENT", "1", 1);
    memset(s_mem, 0, sizeof s_mem);
    memset(s_mem + COLOR_AT, 0xA5, PITCH * HEIGHT);
    if (!strcmp(argv[1], "semaphore-release")) {
        semaphore_release_test();
        return s_failed ? 1 : 0;
    }
    if (!strcmp(argv[1], "semaphore-concurrent")) {
        semaphore_concurrent_test();
        return s_failed ? 1 : 0;
    }
    if (!strcmp(argv[1], "production-memory")) {
        production_memory_test();
        return s_failed ? 1 : 0;
    }
    if (!strcmp(argv[1], "invalid-base")) {
        CHECK(!bw_metal_backend_start_for_test(s_mem + 1, sizeof s_mem - 1));
        CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), "guest memory"));
        bw_metal_backend_stop();
    }
    if (!strncmp(argv[1], "framebuffer-info-", 17) ||
        !strncmp(argv[1], "extended-reject-", 16) ||
        !strncmp(argv[1], "extended-bc1-inactive-kernel-",
                 sizeof("extended-bc1-inactive-kernel-") - 1u) ||
        !strncmp(argv[1], "quincunx-approx-", sizeof("quincunx-approx-") - 1u) ||
        !strncmp(argv[1], "xrgb-single-reject-", 19))
        bw_metal_backend_set_framebuffer_callback(capture_framebuffer_info);
    if (!bw_metal_backend_start_for_test(s_mem, sizeof s_mem)) {
        fprintf(stderr, "Metal setup failed: %s\n", bw_metal_backend_error());
        bw_metal_backend_stop();
        return 1;
    }
    setup_surface(strcmp(argv[1], "screen-combiner") != 0);
    if (!strcmp(argv[1], "visual"))
        visual_test();
    else if (!strcmp(argv[1], "seed-order"))
        seed_order_test();
    else if (!strcmp(argv[1], "command-batching"))
        command_batching_test();
    else if (!strcmp(argv[1], "perspective"))
        perspective_test();
    else if (!strcmp(argv[1], "blend"))
        blend_test();
    else if (!strcmp(argv[1], "color-mask"))
        color_mask_test();
    else if (!strcmp(argv[1], "alpha-test"))
        alpha_test();
    else if (!strcmp(argv[1], "pipeline-cache"))
        pipeline_cache_test();
    else if (!strcmp(argv[1], "cull"))
        cull_test();
    else if (!strcmp(argv[1], "full-rect"))
        full_rect_test();
    else if (!strcmp(argv[1], "depth-clear"))
        depth_clear_test();
    else if (!strcmp(argv[1], "depth-draw"))
        depth_draw_test();
    else if (!strcmp(argv[1], "polygon-offset"))
        polygon_offset_test(1);
    else if (!strcmp(argv[1], "polygon-offset-disabled"))
        polygon_offset_test(0);
    else if (!strcmp(argv[1], "polygon-offset-zero"))
        polygon_offset_test(2);
    else if (!strcmp(argv[1], "stencil-draw"))
        stencil_draw_test();
    else if (!strcmp(argv[1], "partial-color"))
        partial_color_test();
    else if (!strcmp(argv[1], "partial-color-masks"))
        partial_color_masks_test();
    else if (!strcmp(argv[1], "partial-color-default-rect"))
        partial_color_default_rect_test();
    else if (!strcmp(argv[1], "partial-clip"))
        partial_clip_test();
    else if (!strcmp(argv[1], "partial-empty"))
        partial_empty_test();
    else if (!strcmp(argv[1], "partial-depth"))
        partial_depth_test();
    else if (!strcmp(argv[1], "partial-depth-seed"))
        partial_depth_seed_test();
    else if (!strcmp(argv[1], "partial-depth-empty"))
        partial_depth_empty_test();
    else if (!strcmp(argv[1], "partial-depth-unbound"))
        partial_depth_unbound_test();
    else if (!strcmp(argv[1], "partial-depth-flip-only"))
        partial_depth_flip_only_test();
    else if (!strcmp(argv[1], "partial-depth-unbound-flip-only"))
        partial_depth_unbound_flip_only_test();
    else if (!strcmp(argv[1], "partial-depth-rebind"))
        partial_depth_rebind_test();
    else if (!strcmp(argv[1], "partial-mask-full-depth"))
        partial_mask_full_depth_test();
    else if (!strcmp(argv[1], "partial-restart"))
        partial_restart_test();
    else if (!strcmp(argv[1], "depth-unbound"))
        depth_unbound_test();
    else if (!strncmp(argv[1], "depth-", 6))
        depth_failure_test(argv[1]);
    else if (!strcmp(argv[1], "extent-boundary"))
        extent_boundary_test();
    else if (!strcmp(argv[1], "texture-boundary"))
        texture_boundary_test();
    else if (!strcmp(argv[1], "lifecycle") || !strcmp(argv[1], "invalid-base"))
        lifecycle_test();
    else if (!strcmp(argv[1], "snapshot-normal"))
        snapshot_test(0, 0, 0);
    else if (!strcmp(argv[1], "snapshot-immediate"))
        snapshot_test(1, 0, 0);
    else if (!strcmp(argv[1], "snapshot-preflip-normal"))
        snapshot_test(0, 1, 0);
    else if (!strcmp(argv[1], "snapshot-preflip-immediate"))
        snapshot_test(1, 1, 0);
    else if (!strcmp(argv[1], "snapshot-rebind-aa-normal"))
        snapshot_test(0, 0, 1);
    else if (!strcmp(argv[1], "snapshot-rebind-aa-immediate"))
        snapshot_test(1, 0, 1);
    else if (!strcmp(argv[1], "xrgb-aa-clear"))
        xrgb_aa_clear_test();
    else if (!strcmp(argv[1], "xrgb-aa-depth"))
        xrgb_aa_depth_test();
    else if (!strcmp(argv[1], "xrgb-aa-draw"))
        xrgb_aa_draw_test();
    else if (!strcmp(argv[1], "xrgb-single-clear"))
        xrgb_single_clear_test();
    else if (!strcmp(argv[1], "xrgb-single-depth-clear"))
        xrgb_single_depth_clear_test();
    else if (!strcmp(argv[1], "xrgb-single-draw"))
        xrgb_single_draw_test();
    else if (!strcmp(argv[1], "xrgb-single-extended"))
        xrgb_single_extended_test();
    else if (!strncmp(argv[1], "xrgb-single-reject-", 19))
        xrgb_single_rejection_test(argv[1]);
    else if (!strcmp(argv[1], "extended-basic"))
        extended_basic_test();
    else if (!strcmp(argv[1], "extended-texture-cache"))
        extended_texture_cache_test();
    else if (!strcmp(argv[1], "extended-combiner"))
        extended_combiner_test();
    else if (!strcmp(argv[1], "extended-multistage"))
        extended_multistage_combiner_test();
    else if (!strcmp(argv[1], "extended-combiner-mappings"))
        extended_combiner_mappings_test();
    else if (!strcmp(argv[1], "extended-combiner-operations"))
        extended_combiner_operations_test();
    else if (!strcmp(argv[1], "extended-combiner-constants"))
        extended_combiner_constants_test();
    else if (!strcmp(argv[1], "extended-combiner-secondary"))
        extended_combiner_secondary_test();
    else if (!strcmp(argv[1], "extended-fog"))
        extended_fog_test();
    else if (!strcmp(argv[1], "extended-passthrough-stage"))
        extended_passthrough_stage_test();
    else if (!strcmp(argv[1], "extended-texture-stages"))
        extended_texture_stages_test();
    else if (!strcmp(argv[1], "extended-dot-reflection"))
        extended_dot_reflection_test();
    else if (!strcmp(argv[1], "extended-dxt-alpha"))
        extended_dxt_alpha_test();
    else if (!strcmp(argv[1], "extended-swizzled"))
        extended_swizzled_test();
    else if (!strcmp(argv[1], "extended-swizzled-mips"))
        extended_swizzled_mips_test();
    else if (!strcmp(argv[1], "extended-linear"))
        extended_linear_texture_test();
    else if (!strcmp(argv[1], "extended-texture-address"))
        extended_texture_address_test();
    else if (!strcmp(argv[1], "extended-dxt-mips"))
        extended_dxt_mips_test();
    else if (!strcmp(argv[1], "extended-dxt-nearest-mip"))
        extended_dxt_nearest_mip_test();
    else if (!strcmp(argv[1], "extended-dxt-mip-limit"))
        extended_dxt_mip_limit_test();
    else if (!strcmp(argv[1], "extended-bc1"))
        extended_bc1_test();
    else if (!strncmp(argv[1], "extended-bc1-inactive-kernel-",
                      sizeof("extended-bc1-inactive-kernel-") - 1u))
        extended_bc1_inactive_kernel_test(strstr(argv[1], "-immediate") != NULL);
    else if (!strcmp(argv[1], "extended-projective"))
        extended_projective_test();
    else if (!strcmp(argv[1], "extended-projective-zero"))
        extended_projective_divisor_test(0);
    else if (!strcmp(argv[1], "extended-projective-negative"))
        extended_projective_divisor_test(1);
    else if (!strcmp(argv[1], "extended-projective-subnormal"))
        extended_projective_divisor_test(2);
    else if (!strcmp(argv[1], "extended-nonfinite-texture-coordinate"))
        extended_nonfinite_texture_coordinate_test();
    else if (!strcmp(argv[1], "extended-blend"))
        extended_blend_test();
    else if (!strcmp(argv[1], "extended-alpha-pipeline"))
        extended_alpha_pipeline_test();
    else if (!strcmp(argv[1], "extended-render-target-texture"))
        extended_render_target_texture_test();
    else if (!strncmp(argv[1], "extended-reject-", 16))
        extended_rejection_test(argv[1]);
    else if (!strcmp(argv[1], "framebuffer-info-normal"))
        framebuffer_info_test(0, 0);
    else if (!strcmp(argv[1], "framebuffer-info-immediate"))
        framebuffer_info_test(1, 0);
    else if (!strcmp(argv[1], "framebuffer-info-passthrough"))
        framebuffer_info_test(0, 1);
    else if (!strcmp(argv[1], "framebuffer-info-depth-only"))
        framebuffer_info_test(0, 2);
    else if (!strcmp(argv[1], "framebuffer-info-stale"))
        framebuffer_info_stale_test();
    else if (!strcmp(argv[1], "framebuffer-info-reject"))
        framebuffer_info_reject_test();
    else if (!strcmp(argv[1], "framebuffer-info-lifecycle"))
        framebuffer_info_lifecycle_test();
    else if (!strcmp(argv[1], "framebuffer-info-xrgb-single-passthrough"))
        framebuffer_xrgb_single_passthrough_test(0);
    else if (!strcmp(argv[1], "framebuffer-info-xrgb-single-empty-rect"))
        framebuffer_xrgb_single_passthrough_test(1);
    else if (!strcmp(argv[1], "framebuffer-info-xrgb-single-depth-only"))
        framebuffer_xrgb_single_passthrough_test(2);
    else if (!strncmp(argv[1], "framebuffer-info-xrgb-convolution-reject",
                      sizeof("framebuffer-info-xrgb-convolution-reject") - 1u))
        framebuffer_xrgb_convolution_reject_test(
            strstr(argv[1], "-mag-") ? 6u : 7u,
            strstr(argv[1], "-min-") ? 2u : 4u,
            strstr(argv[1], "-immediate") != NULL);
    else if (!strncmp(argv[1], "framebuffer-info-xrgb-convolution-approx-",
                      sizeof("framebuffer-info-xrgb-convolution-approx-") - 1u))
        framebuffer_xrgb_convolution_approx_test(strstr(argv[1], "-immediate") != NULL);
    else if (!strncmp(argv[1], "quincunx-approx-reject-",
                      sizeof("quincunx-approx-reject-") - 1u))
        quincunx_approx_rejection_test(argv[1]);
    else if (!strcmp(argv[1], "quincunx-approx-lifecycle"))
        quincunx_approx_lifecycle_test();
    else if (!strcmp(argv[1], "quincunx-approx-reference-sweep"))
        quincunx_approx_reference_sweep_test();
    else if (!strcmp(argv[1], "quincunx-approx-render-target-resolve"))
        quincunx_approx_render_target_resolve_test();
    else if (!strncmp(argv[1], "framebuffer-info-xrgb-from-", 27)) {
        unsigned from = strstr(argv[1], "-from-aa-") ? SURFACE_XRGB_AA
                      : strstr(argv[1], "-from-argb-") ? SURFACE_ARGB_SINGLE
                      : SURFACE_XRGB_SINGLE;
        unsigned to = strstr(argv[1], "-to-aa-") ? SURFACE_XRGB_AA
                    : strstr(argv[1], "-to-argb-") ? SURFACE_ARGB_SINGLE
                    : SURFACE_XRGB_SINGLE;
        framebuffer_xrgb_transition_test(from, to, strstr(argv[1], "-immediate") != NULL);
    }
    else if (!strcmp(argv[1], "error-thread"))
        error_thread_test();
    else if (!strcmp(argv[1], "registration"))
        registration_test();
    else if (!strncmp(argv[1], "mock-", 5))
        mock_order_test(argv[1]);
    else if (!strncmp(argv[1], "basic-feedback-", 15))
        basic_feedback_test(argv[1]);
    else if (!strcmp(argv[1], "cpu-write-counterexample"))
        cpu_write_counterexample_test();
    else if (!strcmp(argv[1], "surface-switch"))
        surface_switch_test();
    else
        failure_test(argv[1]);
    bw_metal_backend_stop();
    if (!s_failed)
        printf("Metal backend %s: passed\n", argv[1]);
    return s_failed ? 1 : 0;
}
