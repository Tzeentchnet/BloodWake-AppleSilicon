#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_metal_backend.h"
#include "bw_backend_sync.h"
#include "../toolkit_patches/bw_runtime_lifecycle.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xbox_GetMemoryBase(void);
extern size_t xbox_GetMappedSize(void);
extern void bw_ab_frame(uint32_t frame);

#define METAL_MAX_DIMENSION 16384u
#define METAL_MAX_PIPELINES 128u
#define METAL_MAX_DEPTH_SURFACES 32u
#define METAL_COLOR_XRGB_ZERO 0x4u
#define METAL_COLOR_ARGB8 0x8u

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
    MetalUniforms draw;
    uint32_t combiner[4];
    uint32_t final[4];
    uint32_t texture[4];
} MetalExtendedUniforms;

typedef struct {
    uint32_t textured, color_mask, fixed_alpha_zero;
    uint32_t blend_enable, blend_src, blend_dst, blend_eq;
} MetalPipelineKey;

typedef struct {
    uint64_t start, end;
} MetalGuestRegion;

@interface BWMetalDepthSurface : NSObject {
@public
    id<MTLTexture> texture;
    uint32_t va, width, height, pitch;
    uint64_t end;
    unsigned initialized;
}
@end
@implementation BWMetalDepthSurface
@end

static uintptr_t s_guest;
static MetalGuestRegion s_guest_regions[2];
static char s_error[512];
static atomic_int s_error_ready;
static int s_registered;
static int s_dirty;
static int s_quincunx_approx;
static int s_quincunx_reported;
static uint32_t s_frame;
static uint32_t s_current_format;
static Nv2aSurface s_current;
static BwMetalFramebufferCallback s_framebuffer_callback;
static id<MTLDevice> s_device;
static id<MTLCommandQueue> s_queue;
static id<MTLFunction> s_vertex_function;
static id<MTLFunction> s_color_function;
static id<MTLFunction> s_texture_function;
static id<MTLFunction> s_extended_vertex_function;
static id<MTLFunction> s_extended_fragment_function;
static id<MTLFunction> s_clear_vertex_function;
static id<MTLFunction> s_clear_color_function;
static id<MTLFunction> s_clear_depth_function;
static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *s_pipelines;
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *s_clear_pipelines;
static NSMutableDictionary<NSNumber *, BWMetalDepthSurface *> *s_depth_surfaces;
static id<MTLDepthStencilState> s_clear_depth_states[4];
static id<MTLSamplerState> s_sampler;
static id<MTLSamplerState> s_linear_sampler;
static id<MTLTexture> s_surface;

static const char s_shader[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Vertex { float4 position; float4 color; float4 texcoord; };\n"
    "struct Uniforms { float4 surface_texture; float4 depth; float4 alpha; };\n"
    "struct Varying { float4 position [[position]]; float4 color; float2 uv; };\n"
    "vertex Varying vertex_main(uint i [[vertex_id]],\n"
    "    const device Vertex *vertices [[buffer(0)]],\n"
    "    constant Uniforms &u [[buffer(1)]]) {\n"
    "  Vertex v = vertices[i]; Varying out;\n"
    "  float w = 1.0 / v.position.w;\n"
    "  float x = 2.0 * v.position.x / u.surface_texture.x - 1.0;\n"
    "  float y = 1.0 - 2.0 * v.position.y / u.surface_texture.y;\n"
    "  float z = (v.position.z - u.depth.x) / (u.depth.y - u.depth.x);\n"
    "  out.position = float4(x * w, y * w, z * w, w);\n"
    "  out.color = v.color;\n"
    "  out.uv = v.texcoord.xy / u.surface_texture.zw;\n"
    "  return out;\n"
    "}\n"
    /* Match the D3D11 basic path, including its equality tolerance. */
    "float4 alpha_test(float4 color, constant Uniforms &u) {\n"
    "  if (u.alpha.z > 0.5) {\n"
    "    float a = color.a, r = u.alpha.y; int f = int(u.alpha.x);\n"
    "    bool ok = (f == 7) || (f == 1 && a < r) || (f == 2 && abs(a-r) < 0.002)\n"
    "           || (f == 3 && a <= r) || (f == 4 && a > r)\n"
    "           || (f == 5 && abs(a-r) >= 0.002) || (f == 6 && a >= r);\n"
    "    if (!ok) discard_fragment();\n"
    "  }\n"
    "  return color;\n"
    "}\n"
    "fragment float4 color_main(Varying in [[stage_in]],\n"
    "    constant Uniforms &u [[buffer(1)]]) { return alpha_test(in.color, u); }\n"
    "fragment float4 texture_main(Varying in [[stage_in]],\n"
    "    constant Uniforms &u [[buffer(1)]],\n"
    "    texture2d<float> tex [[texture(0)]], sampler samp [[sampler(0)]]) {\n"
    "  return alpha_test(in.color * tex.sample(samp, in.uv), u);\n"
    "}\n"
    "struct XVertex { float4 position, d0, d1, fog, tc0, tc1, tc2, tc3; };\n"
    "struct XUniforms { float4 surface_texture, depth, alpha; uint4 combiner, final, texture; };\n"
    "struct XVarying { float4 position [[position]]; float4 d0; float4 d1; float4 tc0; };\n"
    "vertex XVarying extended_vertex(uint i [[vertex_id]],\n"
    "    const device XVertex *vertices [[buffer(0)]], constant XUniforms &u [[buffer(1)]]) {\n"
    "  XVertex v = vertices[i]; XVarying out; float w = 1.0 / v.position.w;\n"
    "  float x = 2.0 * v.position.x / u.surface_texture.x - 1.0;\n"
    "  float y = 1.0 - 2.0 * v.position.y / u.surface_texture.y;\n"
    "  float z = (v.position.z - u.depth.x) / (u.depth.y - u.depth.x);\n"
    "  out.position = float4(x * w, y * w, z * w, w);\n"
    "  out.d0 = v.d0; out.d1 = v.d1; out.tc0 = v.tc0; return out;\n"
    "}\n"
    "float3 xrgb(thread const float4 *r, uint b) {\n"
    "  float4 v = r[b & 15u]; float3 c = (b & 16u) ? v.aaa : v.rgb;\n"
    "  return ((b >> 5) & 7u) == 0u ? max(c, 0.0) : 1.0 - saturate(c);\n"
    "}\n"
    "float xa(thread const float4 *r, uint b) {\n"
    "  float4 v = r[b & 15u]; float c = (b & 16u) ? v.a : v.b;\n"
    "  return ((b >> 5) & 7u) == 0u ? max(c, 0.0) : 1.0 - saturate(c);\n"
    "}\n"
    /* Explicit bilinear arithmetic and half-up output rounding match the
     * legacy CPU reference where sampler/UNorm hardware can differ by 1. */
    "float4 legacy_linear(texture2d<float> tex, float2 uv) {\n"
    "  float2 size = float2(tex.get_width(), tex.get_height());\n"
    "  float2 p = clamp(uv, float2(-1.0), size + 1.0) - 0.5;\n"
    "  float2 base = floor(p), f = p - base;\n"
    "  int2 lo = clamp(int2(base), int2(0), int2(size) - 1);\n"
    "  int2 hi = clamp(int2(base) + 1, int2(0), int2(size) - 1);\n"
    "  float4 a = tex.read(uint2(lo.x, lo.y)), b = tex.read(uint2(hi.x, lo.y));\n"
    "  float4 c = tex.read(uint2(lo.x, hi.y)), d = tex.read(uint2(hi.x, hi.y));\n"
    "  return (a * (1.0 - f.x) + b * f.x) * (1.0 - f.y)\n"
    "       + (c * (1.0 - f.x) + d * f.x) * f.y;\n"
    "}\n"
    "fragment float4 extended_fragment(XVarying in [[stage_in]],\n"
    "    constant XUniforms &u [[buffer(1)]],\n"
    "    texture2d<float> tex [[texture(0)]], sampler samp [[sampler(0)]]) {\n"
    "  float4 r[16]; for (uint i = 0; i < 16; ++i) r[i] = 0.0;\n"
    "  r[4] = in.d0; r[5] = in.d1; float2 uv = in.tc0.xy / in.tc0.w;\n"
    "  if (u.texture.x) {\n"
    "    r[8] = 0.0;\n"
    "    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n"
    "      r[8] += legacy_linear(tex, uv + float2(x, y))\n"
    "            * (float((2 - abs(x)) * (2 - abs(y))) / 16.0);\n"
    "  } else r[8] = tex.sample(samp, uv);\n"
    "  r[12].a = r[8].a;\n"
    "  uint ci = u.combiner.x, ai = u.combiner.y, co = u.combiner.z, ao = u.combiner.w;\n"
    "  float3 A = xrgb(r, ci >> 24), B = xrgb(r, ci >> 16);\n"
    "  float3 C = xrgb(r, ci >> 8), D = xrgb(r, ci);\n"
    "  float aA = xa(r, ai >> 24), aB = xa(r, ai >> 16);\n"
    "  float aC = xa(r, ai >> 8), aD = xa(r, ai);\n"
    "  float3 ab = clamp(A * B, -1.0, 1.0), cd = clamp(C * D, -1.0, 1.0);\n"
    "  float3 sum = clamp(A * B + C * D, -1.0, 1.0);\n"
    "  float aab = clamp(aA * aB, -1.0, 1.0), acd = clamp(aC * aD, -1.0, 1.0);\n"
    "  float asum = clamp(aA * aB + aC * aD, -1.0, 1.0);\n"
    "  uint d = (co >> 4) & 15u; if (d) r[d].rgb = ab;\n"
    "  d = co & 15u; if (d) r[d].rgb = cd;\n"
    "  d = (co >> 8) & 15u; if (d) r[d].rgb = sum;\n"
    "  d = (ao >> 4) & 15u; if (d) r[d].a = aab;\n"
    "  d = ao & 15u; if (d) r[d].a = acd;\n"
    "  d = (ao >> 8) & 15u; if (d) r[d].a = asum;\n"
    "  uint f0 = u.final.x, f1 = u.final.y;\n"
    "  A = xrgb(r, f0 >> 24); B = xrgb(r, f0 >> 16);\n"
    "  C = xrgb(r, f0 >> 8); D = xrgb(r, f0);\n"
    "  float4 o = saturate(float4(D + C * (1.0 - A) + B * A, xa(r, f1 >> 8)));\n"
    "  if (u.alpha.z > 0.5) {\n"
    "    int a = int(o.a * 255.0 + 0.5), ref = int(u.alpha.y * 255.0 + 0.5);\n"
    "    int f = int(u.alpha.x);\n"
    "    bool ok = f == 7 || (f == 1 && a < ref) || (f == 2 && a == ref)\n"
    "           || (f == 3 && a <= ref) || (f == 4 && a > ref)\n"
    "           || (f == 5 && a != ref) || (f == 6 && a >= ref);\n"
    "    if (!ok) discard_fragment();\n"
    "  }\n"
    "  if (u.texture.x) o.rgb = floor(o.rgb * 255.0 + 0.5) / 255.0;\n"
    "  return o;\n"
    "}\n"
    "vertex float4 clear_vertex(uint i [[vertex_id]], constant float &z [[buffer(0)]]) {\n"
    "  const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};\n"
    "  return float4(p[i], z, 1);\n"
    "}\n"
    "fragment float4 clear_color(constant float4 &c [[buffer(0)]]) { return c; }\n"
    "fragment void clear_depth() {}\n";

static int metal_fail(const char *format, ...)
{
    if (!atomic_load_explicit(&s_error_ready, memory_order_acquire)) {
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
    return atomic_load_explicit(&s_error_ready, memory_order_acquire) ? s_error : NULL;
}

static uint8_t *guest_pointer(uint32_t address)
{
    return (uint8_t *)(s_guest + (uintptr_t)address);
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

static int writeback(void)
{
    size_t row, bytes;
    uint8_t *pixels;

    if (!s_surface || !s_dirty)
        return 1;
    row = (size_t)s_current.width * 4u;
    if ((size_t)s_current.height > SIZE_MAX / row)
        return metal_fail("colour surface readback size overflows");
    bytes = row * s_current.height;
    pixels = malloc(bytes);
    if (!pixels)
        return metal_fail("could not allocate colour surface readback");
    [s_surface getBytes:pixels bytesPerRow:row
             fromRegion:MTLRegionMake2D(0, 0, s_current.width, s_current.height)
            mipmapLevel:0];
    for (uint32_t y = 0; y < s_current.height; y++)
        memcpy(guest_pointer(s_current.color_va) + (size_t)y * s_current.pitch,
               pixels + (size_t)y * row, row);
    free(pixels);
    s_dirty = 0;
    return 1;
}

static int prepare_surface(const Nv2aSurface *s)
{
    MTLTextureDescriptor *desc;
    uint32_t format = bw_backend_reg(0x0208u) & 0xFu;

    if (!surface_valid(s))
        return 0;
    if (depth_overlaps(s->color_va, span_end(s->color_va, s->width, s->height, s->pitch)))
        return metal_fail("colour surface aliases a cleared depth/stencil surface");
    if (s_surface && same_surface(s, &s_current) && format == s_current_format)
        return 1;
    if (s_surface && s_dirty)
        return metal_fail("changing colour surfaces within a frame is unsupported");
    s_surface = nil;
    desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:s->width
                                                            height:s->height
                                                         mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s_surface = [s_device newTextureWithDescriptor:desc];
    if (!s_surface)
        return metal_fail("could not create colour surface %ux%u", s->width, s->height);
    [s_surface replaceRegion:MTLRegionMake2D(0, 0, s->width, s->height)
                 mipmapLevel:0 withBytes:guest_pointer(s->color_va) bytesPerRow:s->pitch];
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
    uint32_t format, pitch;
    uint64_t end;
    BWMetalDepthSurface *depth;
    MTLTextureDescriptor *desc;

    if (!rs || !rs->zeta_va)
        return metal_fail("depth/stencil clear requires a bound depth surface"), nil;
    if (!surface_valid(s))
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
        if (depth->width != s->width || depth->height != s->height || depth->pitch != pitch)
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
                                                             width:s->width
                                                            height:s->height
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
    depth->width = s->width;
    depth->height = s->height;
    depth->pitch = pitch;
    depth->end = end;
    s_depth_surfaces[@(depth->va)] = depth;
    return depth;
}

static int finish_command(id<MTLCommandBuffer> command)
{
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
        return metal_fail("GPU command failed: %s", metal_error_text(command.error));
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
        const uint8_t *guest = guest_pointer(depth->va) + (size_t)y * depth->pitch;
        float *z = (float *)((uint8_t *)buffer.contents + (size_t)y * layout.depth_row);
        uint8_t *stencil = (uint8_t *)buffer.contents + layout.stencil_offset
                         + (size_t)y * layout.stencil_row;
        for (uint32_t x = 0; x < depth->width; x++) {
            uint32_t packed;
            memcpy(&packed, guest + (size_t)x * 4u, sizeof packed);
            if (missing & 1u)
                z[x] = (float)(packed >> 8) / 16777215.0f;
            if (missing & 2u)
                stencil[x] = (uint8_t)packed;
        }
    }
    command = [s_queue commandBuffer];
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
    if (!finish_command(command))
        return 0;
    depth->initialized |= missing;
    return 1;
}

static id<MTLRenderPipelineState> partial_clear_pipeline(uint32_t color_flags, int has_depth)
{
    NSNumber *key = @((color_flags >> 4) | (has_depth ? 16u : 0u));
    id<MTLRenderPipelineState> state = s_clear_pipelines[key];
    if (state)
        return state;
    if (!s_clear_pipelines)
        s_clear_pipelines = [NSMutableDictionary new];
    if (!s_clear_pipelines)
        return metal_fail("could not create clear pipeline cache"), nil;
    MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
    if (!pipeline)
        return metal_fail("could not create clear pipeline descriptor"), nil;
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
    state = [s_device newRenderPipelineStateWithDescriptor:pipeline error:&error];
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

static int validate_state(const Nv2aRenderState *rs, int extended)
{
    if (!rs)
        return metal_fail("missing render state");
    if (rs->depth_test_enable || rs->depth_write || rs->stencil_enable)
        return metal_fail("unsupported depth or stencil state");
    if (rs->cull_enable &&
        ((rs->cull_face != 0x404u && rs->cull_face != 0x405u && rs->cull_face != 0x408u) ||
         (rs->front_face != 0x900u && rs->front_face != 0x901u)))
        return metal_fail("unsupported cull face or winding");
    if (rs->alpha_test_enable && (rs->alpha_func < 0x200u || rs->alpha_func > 0x207u))
        return metal_fail("unsupported alpha comparison 0x%08x", rs->alpha_func);
    if (bw_backend_reg(0x02A4u) || bw_backend_reg(0x0338u) ||
        bw_backend_reg(0x17F8u) ||
        (!extended && (bw_backend_combiner_seen() || bw_backend_reg(0x1E70u))))
        return metal_fail("unsupported fog, polygon offset, clip plane or combiner state");
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

static id<MTLRenderPipelineState> draw_pipeline(int shader_kind, const Nv2aRenderState *rs)
{
    MetalPipelineKey key = {
        .textured = (uint32_t)shader_kind,
        .color_mask = rs->color_mask & 0x01010101u,
        .fixed_alpha_zero = (bw_backend_reg(0x0208u) & 0xFu) == METAL_COLOR_XRGB_ZERO,
        .blend_enable = rs->blend_enable != 0,
        .blend_src = rs->blend_enable ? rs->blend_src : 0,
        .blend_dst = rs->blend_enable ? rs->blend_dst : 0,
        .blend_eq = rs->blend_enable ? rs->blend_eq : 0,
    };
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
    NSData *cache_key = [NSData dataWithBytes:&key length:sizeof key];
    id<MTLRenderPipelineState> state = s_pipelines[cache_key];
    if (state)
        return state;
    MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = shader_kind == 2 ? s_extended_vertex_function : s_vertex_function;
    pipeline.fragmentFunction = shader_kind == 2 ? s_extended_fragment_function
                               : shader_kind == 1 ? s_texture_function : s_color_function;
    MTLRenderPipelineColorAttachmentDescriptor *attachment = pipeline.colorAttachments[0];
    attachment.pixelFormat = MTLPixelFormatBGRA8Unorm;
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
    state = [s_device newRenderPipelineStateWithDescriptor:pipeline error:&error];
    if (!state)
        return metal_fail("draw pipeline creation failed: %s", metal_error_text(error)), nil;
    if (s_pipelines.count >= METAL_MAX_PIPELINES)
        [s_pipelines removeAllObjects];
    s_pipelines[cache_key] = state;
    return state;
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

static int valid_combiner_input(uint32_t byte)
{
    uint32_t reg = byte & 15u;
    return ((byte >> 5) & 7u) <= 1u &&
           (reg == 0u || reg == 4u || reg == 5u || reg == 8u ||
            reg == 12u || reg == 13u);
}

static int valid_combiner_word(uint32_t word, unsigned inputs)
{
    for (unsigned i = 0; i < inputs; i++)
        if (!valid_combiner_input(word >> (24u - i * 8u)))
            return 0;
    return 1;
}

static int valid_combiner_destinations(uint32_t word)
{
    uint32_t dest[3] = {word & 15u, (word >> 4) & 15u, (word >> 8) & 15u};
    if (word & ~0xFFFu)
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
    if ((rc->control & 0xFFu) != 1u || (rc->control & ~0x111FFu))
        return metal_fail("unsupported register-combiner stage count or control");
    if (rc->stage_program != 1u)
        return metal_fail("unsupported register-combiner texture-stage program");
    if (!valid_combiner_word(rc->color_icw[0], 4) ||
        !valid_combiner_word(rc->alpha_icw[0], 4) ||
        !valid_combiner_destinations(rc->color_ocw[0]) ||
        !valid_combiner_destinations(rc->alpha_ocw[0]))
        return metal_fail("unsupported register-combiner general-stage inputs or outputs");
    if ((!rc->final0 && !rc->final1) || (rc->final1 & 0x1Fu) ||
        !valid_combiner_word(rc->final0, 4) || !valid_combiner_word(rc->final1, 3))
        return metal_fail("unsupported register-combiner final-stage inputs or flags");
    return 1;
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
    return texture;
}

static id<MTLTexture> extended_texture(const Nv2aSurface *surface, const BwXBatch *batch,
                                       int *quincunx_approx)
{
    const Nv2aTexture *tex = &batch->tex[0];
    uint32_t format = bw_backend_reg(0x1B04u), address = bw_backend_reg(0x1B08u);
    uint32_t control = bw_backend_reg(0x1B0Cu);
    uint32_t filter = tex->filter, min = (filter >> 16) & 0xFFu, mag = (filter >> 24) & 15u;
    *quincunx_approx = 0;
    if (batch->tex_valid[0] && (min == 7u || mag == 4u)) {
        if (((filter >> 13) & 7u) == 1u) {
            if (s_quincunx_approx && min == 7u && mag == 4u) {
                id<MTLTexture> texture = quincunx_approx_texture(surface, batch);
                if (texture)
                    *quincunx_approx = 1;
                return texture;
            }
            return metal_fail("unsupported extended texture convolution filter (quincunx)"), nil;
        }
        return metal_fail("unsupported extended texture convolution filter"), nil;
    }
    if (!batch->tex_valid[0] || !batch->tex_normalized[0] || batch->tex_cube[0] ||
        tex->color != 0x0Cu || tex->levels != 1u || tex->addr_u != 3u || tex->addr_v != 3u ||
        ((format >> 4) & 15u) != 2u || (format & 4u) ||
        ((format >> 8) & 0xFFu) != 0x0Cu || ((format >> 16) & 15u) != 1u ||
        (address & ~0x000F0F0Fu) || (filter & 0xF0000000u) ||
        (min != 2u && min != 6u) || mag != 2u ||
        !(control & (1u << 30)) || (control & 0x8000003Fu))
        return metal_fail("unsupported extended texture format, sampler or layout"), nil;
    if (tex->width < 4u || tex->height < 4u ||
        tex->width > METAL_MAX_DIMENSION || tex->height > METAL_MAX_DIMENSION ||
        (tex->width & (tex->width - 1u)) || (tex->height & (tex->height - 1u)) ||
        tex->width != (1u << ((format >> 20) & 15u)) ||
        tex->height != (1u << ((format >> 24) & 15u)))
        return metal_fail("unsupported extended texture dimensions"), nil;
    uint64_t bytes = (uint64_t)(tex->width / 4u) * (tex->height / 4u) * 8u;
    uint64_t end = (uint64_t)tex->offset + bytes;
    if ((tex->offset & 7u) || !byte_span_valid(tex->offset, bytes))
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
    if (!s_device.supportsBCTextureCompression)
        return metal_fail("Metal device does not support BC1 texture compression"), nil;
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC1_RGBA
                                                           width:tex->width height:tex->height
                                                       mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [s_device newTextureWithDescriptor:desc];
    if (!texture)
        return metal_fail("could not create extended BC1 texture"), nil;
    [texture replaceRegion:MTLRegionMake2D(0, 0, tex->width, tex->height)
               mipmapLevel:0 withBytes:guest_pointer(tex->offset)
               bytesPerRow:(size_t)tex->width / 4u * 8u];
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
        if (depth_flags) {
            depth = prepare_depth_surface(s, rs);
            if (!depth)
                return;
        }
        if (color_flags && !prepare_surface(s))
            return;
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
        id<MTLCommandBuffer> command = [s_queue commandBuffer];
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
        [encoder endEncoding];
        if (finish_command(command)) {
            if (color_flags)
                s_dirty = 1;
            if (depth)
                depth->initialized |= depth_flags;
        }
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
}

static void metal_draw(const Nv2aSurface *s, const Nv2aBatch *batch)
{
    @autoreleasepool {
        if (bw_metal_backend_error())
            return;
        if (!batch || !batch->vertices || !batch->count || batch->count % 3u ||
            (size_t)batch->count > SIZE_MAX / sizeof(MetalVertex)) {
            metal_fail("invalid triangle batch");
            return;
        }
        if (!validate_state(batch->state, 0))
            return;
        id<MTLRenderPipelineState> pipeline = draw_pipeline(batch->texture != NULL, batch->state);
        if (!pipeline || !prepare_surface(s))
            return;
        if (!batch->texture && bw_backend_reg(0x1B00u)) {
            metal_fail("bound stage-0 texture was not supplied to the draw");
            return;
        }
        id<MTLTexture> texture = nil;
        if (batch->texture && !(texture = decode_texture(batch->texture)))
            return;
        id<MTLBuffer> vertices =
            [s_device newBufferWithLength:(size_t)batch->count * sizeof(MetalVertex)
                                  options:MTLResourceStorageModeShared];
        if (!vertices) {
            metal_fail("could not allocate vertex buffer");
            return;
        }
        MetalVertex *out = vertices.contents;
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
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = s_surface;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> command = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!command || !encoder) {
            metal_fail("could not create draw command");
            return;
        }
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vertices offset:0 atIndex:0];
        [encoder setVertexBytes:&uniforms length:sizeof uniforms atIndex:1];
        [encoder setFragmentBytes:&uniforms length:sizeof uniforms atIndex:1];
        set_draw_state(encoder, batch->state);
        if (texture) {
            [encoder setFragmentTexture:texture atIndex:0];
            [encoder setFragmentSamplerState:s_sampler atIndex:0];
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:batch->count];
        [encoder endEncoding];
        if (finish_command(command))
            s_dirty = 1;
    }
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
        for (uint32_t i = 0; i < batch->count; i++) {
            const BwXVertex *v = &batch->vertices[i];
            if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z) ||
                !isfinite(v->rhw) || v->rhw <= 0.0f ||
                !isfinite(v->tc[0][0]) || !isfinite(v->tc[0][1]) ||
                !isfinite(v->tc[0][3]) || v->tc[0][3] < FLT_MIN ||
                !isfinite(v->tc[0][0] / v->tc[0][3]) ||
                !isfinite(v->tc[0][1] / v->tc[0][3])) {
                metal_fail("invalid extended vertex coordinates or projective divisor");
                return;
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
                metal_fail("extended vertex projection overflows");
                return;
            }
        }
        if (batch->state->cull_enable && batch->state->cull_face == 0x408u)
            return;
        int quincunx_approx;
        id<MTLTexture> texture = extended_texture(surface, batch, &quincunx_approx);
        id<MTLRenderPipelineState> pipeline = texture ? draw_pipeline(2, batch->state) : nil;
        if (!pipeline || !prepare_surface(surface))
            return;
        id<MTLBuffer> vertices =
            [s_device newBufferWithBytes:batch->vertices
                                 length:(size_t)batch->count * sizeof(BwXVertex)
                                options:MTLResourceStorageModeShared];
        if (!vertices) {
            metal_fail("could not allocate extended vertex buffer");
            return;
        }
        MetalExtendedUniforms uniforms = {
            .draw = {
                .surface_texture = {(float)surface->width, (float)surface->height, 1.0f, 1.0f},
                .depth = {batch->state->depth_min, batch->state->depth_max, 0.0f, 0.0f},
                .alpha = {batch->state->alpha_test_enable
                              ? (float)(batch->state->alpha_func - 0x200u) : 7.0f,
                          (float)(batch->state->alpha_ref & 0xFFu) / 255.0f,
                          batch->state->alpha_test_enable ? 1.0f : 0.0f, 0.0f},
            },
            .combiner = {batch->rc->color_icw[0], batch->rc->alpha_icw[0],
                         batch->rc->color_ocw[0], batch->rc->alpha_ocw[0]},
            .final = {batch->rc->final0, batch->rc->final1, 0, 0},
            .texture = {(uint32_t)quincunx_approx, 0, 0, 0},
        };
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = s_surface;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> command = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!command || !encoder) {
            metal_fail("could not create extended draw command");
            return;
        }
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vertices offset:0 atIndex:0];
        [encoder setVertexBytes:&uniforms length:sizeof uniforms atIndex:1];
        [encoder setFragmentBytes:&uniforms length:sizeof uniforms atIndex:1];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder setFragmentSamplerState:s_linear_sampler atIndex:0];
        set_draw_state(encoder, batch->state);
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:batch->count];
        [encoder endEncoding];
        if (finish_command(command)) {
            s_dirty = 1;
            if (quincunx_approx && !s_quincunx_reported) {
                fprintf(stderr, "[GPU-METAL] quincunx approximation used: legacy Gaussian; "
                                "not verified NV2A filtering\n");
                s_quincunx_reported = 1;
            }
        }
    }
}

void bw_metal_backend_set_framebuffer_callback(BwMetalFramebufferCallback callback)
{
    s_framebuffer_callback = callback;
}

static int report_framebuffer(const Nv2aSurface *surface, uint32_t format)
{
    if (!s_framebuffer_callback)
        return 1;
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
    if (!s_framebuffer_callback(&info))
        return metal_fail("presenter rejected framebuffer descriptor");
    return 1;
}

static int metal_present_sync(uint32_t color_va, uint32_t pitch)
{
    @autoreleasepool {
        BwBackendSnapshotSurface selected;
        if (bw_metal_backend_error())
            return 0;
        bw_backend_snapshot_surface(&selected);
        if (!color_va || color_va != selected.surface.color_va || pitch != selected.surface.pitch)
            return metal_fail("presented colour surface does not match the selected surface");
        if (s_dirty) {
            if (!s_surface || !same_surface(&selected.surface, &s_current) ||
                (selected.format & 0xFu) != s_current_format)
                return metal_fail("presented colour surface does not match the rendered surface");
            /* Each submitted command has completed in finish_command(). */
            if (!writeback())
                return 0;
        } else {
            if (!surface_valid_for(&selected.surface, selected.format,
                                   selected.clip_x, selected.clip_y))
                return 0;
            if (depth_overlaps(color_va, span_end(color_va, selected.surface.width,
                                                   selected.surface.height, pitch)))
                return metal_fail("colour surface aliases a cleared depth/stencil surface");
        }
        return report_framebuffer(&selected.surface, selected.format & 0xFu);
    }
}

static void metal_flip(void)
{
    @autoreleasepool {
        if (bw_metal_backend_error())
            return;
        if (s_dirty) {
            metal_fail("flip occurred without a successful surface readback");
            return;
        }
        s_surface = nil;
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

static int initialize_metal(void)
{
    NSError *error = nil;
    s_device = MTLCreateSystemDefaultDevice();
    if (!s_device)
        return metal_fail("no Metal device");
    s_queue = [s_device newCommandQueue];
    if (!s_queue)
        return metal_fail("could not create Metal command queue");
    MTLCompileOptions *options = nil;
    if (s_quincunx_approx) {
        /* Keep the reference filter's float operation order. */
        options = [MTLCompileOptions new];
        if (!options)
            return metal_fail("could not create precise Metal shader options");
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
    id<MTLLibrary> library = [s_device newLibraryWithSource:@(s_shader) options:options error:&error];
    if (!library)
        return metal_fail("shader compilation failed: %s", metal_error_text(error));
    s_vertex_function = [library newFunctionWithName:@"vertex_main"];
    s_color_function = [library newFunctionWithName:@"color_main"];
    s_texture_function = [library newFunctionWithName:@"texture_main"];
    s_extended_vertex_function = [library newFunctionWithName:@"extended_vertex"];
    s_extended_fragment_function = [library newFunctionWithName:@"extended_fragment"];
    s_clear_vertex_function = [library newFunctionWithName:@"clear_vertex"];
    s_clear_color_function = [library newFunctionWithName:@"clear_color"];
    s_clear_depth_function = [library newFunctionWithName:@"clear_depth"];
    if (!s_vertex_function || !s_color_function || !s_texture_function ||
        !s_extended_vertex_function || !s_extended_fragment_function ||
        !s_clear_vertex_function || !s_clear_color_function || !s_clear_depth_function)
        return metal_fail("Metal shader entry point missing");
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
    sampler.lodMinClamp = 0.0f;
    sampler.lodMaxClamp = 0.0f;
    s_linear_sampler = [s_device newSamplerStateWithDescriptor:sampler];
    if (!s_linear_sampler)
        return metal_fail("linear sampler creation failed");
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
    const char *approx = getenv("BW_METAL_QUINCUNX_APPROX");
    s_quincunx_approx = approx && !strcmp(approx, "1");
    if (!initialize_metal()) {
        bw_metal_backend_stop();
        return 0;
    }
    bw_backend_register_with_present_sync(&s_backend, metal_present_sync);
    bw_backend_ext_register(&s_extended_backend);
    s_frame = 0;
    bw_ab_frame(0);
    s_registered = 1;
    if (s_quincunx_approx)
        fprintf(stderr, "[GPU-METAL] WARNING: BW_METAL_QUINCUNX_APPROX=1 uses a legacy "
                        "Gaussian approximation, not verified NV2A quincunx filtering\n");
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
    if (s_registered) {
        bw_backend_ext_unregister_if_current(&s_extended_backend);
        bw_backend_unregister_if_current(&s_backend);
    }
    s_registered = 0;
    s_dirty = 0;
    s_quincunx_approx = 0;
    s_quincunx_reported = 0;
    s_current_format = 0;
    s_framebuffer_callback = NULL;
    s_surface = nil;
    s_sampler = nil;
    s_linear_sampler = nil;
    s_pipelines = nil;
    s_clear_pipelines = nil;
    s_depth_surfaces = nil;
    for (unsigned i = 0; i < 4; i++)
        s_clear_depth_states[i] = nil;
    s_vertex_function = nil;
    s_color_function = nil;
    s_texture_function = nil;
    s_extended_vertex_function = nil;
    s_extended_fragment_function = nil;
    s_clear_vertex_function = nil;
    s_clear_color_function = nil;
    s_clear_depth_function = nil;
    s_queue = nil;
    s_device = nil;
    s_guest = 0;
    memset(s_guest_regions, 0, sizeof s_guest_regions);
    memset(&s_current, 0, sizeof s_current);
}
