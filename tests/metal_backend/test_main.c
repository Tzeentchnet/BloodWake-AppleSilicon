#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_metal_backend.h"
#include "bw_gpu_audit.h"

#define WIDTH 16u
#define HEIGHT 12u
#define PITCH 80u
#define COLOR_AT 0x10000u
#define POS_AT 0x20000u
#define DIFF_AT 0x21000u
#define UV_AT 0x22000u
#define TEX_AT 0x30000u
#define MEM_BYTES 0x40000u

static _Alignas(16) uint8_t s_mem[MEM_BYTES];
static int s_failed;

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }
int bw_ab_off(const char *fix) { (void)fix; return 0; }
int bw_audit_on(void) { return 0; }
void bw_audit_method(uint32_t subch, uint32_t method, uint32_t param)
{ (void)subch; (void)method; (void)param; }
void bw_audit_draw(const BwAuditDraw *draw) { (void)draw; }
void bw_audit_clear(uint32_t flags, uint32_t width, uint32_t height, const uint32_t *rect)
{ (void)flags; (void)width; (void)height; (void)rect; }
void bw_audit_flip(uint32_t offset, uint32_t format) { (void)offset; (void)format; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

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
    memcpy(s_mem + address, &value, sizeof value);
}

static void put_float(uint32_t address, float value)
{
    memcpy(s_mem + address, &value, sizeof value);
}

static uint32_t pixel(unsigned x, unsigned y)
{
    uint32_t value;
    memcpy(&value, s_mem + COLOR_AT + y * PITCH + x * 4u, sizeof value);
    return value;
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
    } else if (!strcmp(which, "unsupported-origin")) {
        method(0x0200, (WIDTH << 16) | 1u);
        clear(0xFF000000u);
        expect_error("surface clip origin", before);
    } else if (!strcmp(which, "invalid-address")) {
        method(0x0210, MEM_BYTES - 4u);
        clear(0xFF000000u);
        expect_error("surface guest-memory span", before);
    } else if (!strcmp(which, "unsupported-clear")) {
        method(0x1D94, 0x10);
        expect_error("clear flags", before);
    } else if (!strcmp(which, "unsupported-rect")) {
        method(0x1D98, 3u << 16);
        method(0x1D9C, 3u << 16);
        clear(0xFF000000u);
        expect_error("partial clear", before);
    } else if (!strcmp(which, "incomplete-rect")) {
        method(0x1D98, 0);
        clear(0xFF000000u);
        expect_error("incomplete clear", before);
    } else {
        clear(0xFF102030u);
        if (!strcmp(which, "unsupported-state"))
            method(0x030C, 1);
        else if (!strcmp(which, "invalid-blend-src") || !strcmp(which, "invalid-blend-dst") ||
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
        else if (!strcmp(which, "unsupported-stage"))
            method(0x1B40, TEX_AT);
        else if (!strcmp(which, "unsupported-combiner"))
            method(0x1E60, 1);
        else if (!strcmp(which, "screen-combiner"))
            method(0x1E60, 0);
        else if (!strcmp(which, "surface-switch"))
            method(0x0210, COLOR_AT + PITCH * HEIGHT);
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
        const char *expected = !strcmp(which, "unsupported-state") ? "unsupported depth"
                             : !strcmp(which, "invalid-blend-src") ? "unsupported blend factor"
                             : !strcmp(which, "invalid-blend-dst") ? "unsupported destination blend factor"
                             : !strcmp(which, "invalid-blend-equation") ? "unsupported blend equation"
                             : !strcmp(which, "invalid-alpha-func") ? "unsupported alpha comparison"
                             : (!strcmp(which, "invalid-cull-face") ||
                                !strcmp(which, "invalid-cull-winding")) ? "unsupported cull"
                             : !strcmp(which, "unsupported-fog") ? "unsupported fog"
                             : !strcmp(which, "unsupported-stage") ? "texture stages"
                             : !strcmp(which, "unsupported-combiner") ? "register-combiner"
                             : !strcmp(which, "screen-combiner") ? "combiner state"
                             : !strcmp(which, "surface-switch") ? "changing colour surfaces"
                             : (!strcmp(which, "unsupported-texture") ||
                                !strcmp(which, "cube-texture")) ? "unsupported stage-0"
                             : !strcmp(which, "texture-extent") ? "texture dimensions exceed"
                             : (!strcmp(which, "invalid-texture") ||
                                !strcmp(which, "unaligned-texture")) ? "texture guest-memory span"
                             : "pending render target";
        expect_error(expected, before);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Expected one test case name\n");
        return 2;
    }
    unsetenv("RECOMP_NO_COMBINERS");
    memset(s_mem, 0, sizeof s_mem);
    memset(s_mem + COLOR_AT, 0xA5, PITCH * HEIGHT);
    if (!strcmp(argv[1], "invalid-base")) {
        CHECK(!bw_metal_backend_start_for_test(s_mem + 1, sizeof s_mem - 1));
        CHECK(bw_metal_backend_error() && strstr(bw_metal_backend_error(), "guest memory"));
        bw_metal_backend_stop();
    }
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
    else if (!strcmp(argv[1], "extent-boundary"))
        extent_boundary_test();
    else if (!strcmp(argv[1], "texture-boundary"))
        texture_boundary_test();
    else if (!strcmp(argv[1], "lifecycle") || !strcmp(argv[1], "invalid-base"))
        lifecycle_test();
    else
        failure_test(argv[1]);
    bw_metal_backend_stop();
    if (!s_failed)
        printf("Metal backend %s: passed\n", argv[1]);
    return s_failed ? 1 : 0;
}
