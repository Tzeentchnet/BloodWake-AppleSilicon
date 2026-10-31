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
#include "d3d/d3d8_swizzle.h"
#include "../toolkit_patches/bw_runtime_lifecycle.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xbox_GetMemoryBase(void);
extern size_t xbox_GetMappedSize(void);
extern void bw_ab_frame(uint32_t frame);
extern uint64_t xbox_HostNowNs(void);

#define METAL_MAX_DIMENSION 16384u
#define METAL_MAX_PIPELINES 128u
#define METAL_MAX_DEPTH_STATES 128u
#define METAL_MAX_COLOR_SURFACES 32u
#define METAL_MAX_DEPTH_SURFACES 32u
#define METAL_MAX_FRAME_SAMPLES 16384u
#define METAL_INITIAL_VERTEX_UPLOAD (16u * 1024u * 1024u)
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
} MetalPipelineKey;

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
    uint32_t format, bytes_per_pixel, aa_sx, aa_sy;
} MetalColorSurfaceKey;

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
static id<MTLCommandBuffer> s_pending_command;
static id<MTLRenderCommandEncoder> s_draw_encoder;
static id<MTLTexture> s_draw_color;
static id<MTLTexture> s_draw_depth;
static uint64_t s_command_submissions;
static uint64_t s_draw_encoder_creations;
static id<MTLBuffer> s_vertex_upload;
static size_t s_vertex_upload_capacity;
static size_t s_vertex_upload_offset;
static uint64_t s_vertex_upload_allocations;
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
static NSMutableDictionary<NSData *, id<MTLTexture>> *s_color_surfaces;
static NSMutableDictionary<NSData *, BWMetalTextureEntry *> *s_textures;
static NSMutableDictionary<NSNumber *, BWMetalDepthSurface *> *s_depth_surfaces;
static NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *s_depth_states;
static id<MTLDepthStencilState> s_clear_depth_states[4];
static id<MTLSamplerState> s_sampler;
static id<MTLSamplerState> s_samplers[3][3][2][2][3];
static id<MTLTexture> s_surface;
static id<MTLTexture> s_inactive_texture;
static id<MTLTexture> s_inactive_cube_texture;
static uint64_t s_texture_uploads;
static uint64_t s_frame_times[METAL_MAX_FRAME_SAMPLES];
static size_t s_frame_time_count;
static uint64_t s_last_flip_ns;
static int s_frame_timing_enabled;

static int flush_commands(void);

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
    "struct XUniforms { float4 surface_texture, depth, alpha, fog_color; uint4 combiner[8], factors[8], final, texture, stages, dot_state; };\n"
    "struct XVarying { float4 position [[position]]; float4 d0; float4 d1; float fog; float fogp; float4 tc0, tc1, tc2, tc3; };\n"
    "vertex XVarying extended_vertex(uint i [[vertex_id]],\n"
    "    const device XVertex *vertices [[buffer(0)]], constant XUniforms &u [[buffer(1)]]) {\n"
    "  XVertex v = vertices[i]; XVarying out; float w = 1.0 / v.position.w;\n"
    "  float x = 2.0 * v.position.x / u.surface_texture.x - 1.0;\n"
    "  float y = 1.0 - 2.0 * v.position.y / u.surface_texture.y;\n"
    "  float z = (v.position.z - u.depth.x) / (u.depth.y - u.depth.x);\n"
    "  out.position = float4(x * w, y * w, z * w, w);\n"
    "  out.d0 = v.d0; out.d1 = v.d1; out.fog = v.fog.x; out.fogp = v.fog.y;\n"
    "  out.tc0 = v.tc0; out.tc1 = v.tc1;\n"
    "  out.tc2 = v.tc2; out.tc3 = v.tc3; return out;\n"
    "}\n"
    "float xmap(float x, uint m) {\n"
    "  float p = max(x, 0.0);\n"
    "  if (m == 0u) return p; if (m == 1u) return 1.0 - saturate(x);\n"
    "  if (m == 2u) return 2.0 * p - 1.0; if (m == 3u) return 1.0 - 2.0 * p;\n"
    "  if (m == 4u) return p - 0.5; if (m == 5u) return 0.5 - p;\n"
    "  return m == 6u ? x : -x;\n"
    "}\n"
    "float3 xrgb(thread const float4 *r, uint b) {\n"
    "  float4 v = r[b & 15u]; float3 c = (b & 16u) ? v.aaa : v.rgb;\n"
    "  uint m = (b >> 5) & 7u;\n"
    "  return float3(xmap(c.r, m), xmap(c.g, m), xmap(c.b, m));\n"
    "}\n"
    "float xa(thread const float4 *r, uint b) {\n"
    "  float4 v = r[b & 15u]; return xmap((b & 16u) ? v.a : v.b, (b >> 5) & 7u);\n"
    "}\n"
    "float3 xout(float3 x, uint m) {\n"
    "  if (m == 0x08u) return x - 0.5; if (m == 0x10u) return x * 2.0;\n"
    "  if (m == 0x18u) return (x - 0.5) * 2.0; if (m == 0x20u) return x * 4.0;\n"
    "  if (m == 0x30u) return x * 0.5; return x;\n"
    "}\n"
    "float4 xunpack(uint c) {\n"
    "  return float4((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, c >> 24) / 255.0;\n"
    "}\n"
    "float2 finite_uv(float2 uv) { return all(isfinite(uv)) ? uv : float2(0.0); }\n"
    "float4 finite_tc(float4 tc) { return select(float4(0.0), tc, isfinite(tc)); }\n"
    "float3 dot_map(float4 c, uint m) {\n"
    "  float3 x = c.rgb * 255.0;\n"
    "  if (m == 1u) return (x - 128.0) / 127.0;\n"
    "  if (m == 2u) return select((x + 0.5) / 127.5, (x - 255.5) / 127.5, x >= 128.0);\n"
    "  if (m == 3u) return select(x / 127.0, (x - 256.0) / 127.0, x >= 128.0);\n"
    "  return c.rgb;\n"
    "}\n"
    "float4 cube_sample(texture2d_array<float> tex, sampler samp, float3 d) {\n"
    "  float3 a = abs(d); float ma, sc, tc; uint face;\n"
    "  if (a.x >= a.y && a.x >= a.z) {\n"
    "    face = d.x > 0.0 ? 0u : 1u; ma = a.x; sc = d.x > 0.0 ? -d.z : d.z; tc = -d.y;\n"
    "  } else if (a.y >= a.z) {\n"
    "    face = d.y > 0.0 ? 2u : 3u; ma = a.y; sc = d.x; tc = d.y > 0.0 ? d.z : -d.z;\n"
    "  } else {\n"
    "    face = d.z > 0.0 ? 4u : 5u; ma = a.z; sc = d.z > 0.0 ? d.x : -d.x; tc = -d.y;\n"
    "  }\n"
    "  if (ma == 0.0) ma = 1.0;\n"
    "  return tex.sample(samp, float2(sc / ma + 1.0, tc / ma + 1.0) * 0.5, face);\n"
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
    "    texture2d<float> tex [[texture(0)]], texture2d<float> tex1 [[texture(1)]],\n"
    "    texture2d<float> tex2 [[texture(2)]], texture2d<float> tex3 [[texture(3)]],\n"
    "    texture2d_array<float> cube0 [[texture(4)]], texture2d_array<float> cube1 [[texture(5)]],\n"
    "    texture2d_array<float> cube2 [[texture(6)]], texture2d_array<float> cube3 [[texture(7)]],\n"
    "    sampler samp [[sampler(0)]], sampler samp1 [[sampler(1)]],\n"
    "    sampler samp2 [[sampler(2)]], sampler samp3 [[sampler(3)]]) {\n"
    "  float4 r[16]; for (uint i = 0; i < 16; ++i) r[i] = 0.0;\n"
    "  r[3] = float4(u.fog_color.rgb, saturate(in.fogp));\n"
    "  r[4] = in.d0; r[5] = in.d1;\n"
    "  float4 tc[4] = { finite_tc(in.tc0), finite_tc(in.tc1), finite_tc(in.tc2), finite_tc(in.tc3) };\n"
    "  float q0 = tc[0].w != 0.0 ? tc[0].w : 1.0;\n"
    "  float q1 = tc[1].w != 0.0 ? tc[1].w : 1.0;\n"
    "  float q2 = tc[2].w != 0.0 ? tc[2].w : 1.0;\n"
    "  float q3 = tc[3].w != 0.0 ? tc[3].w : 1.0;\n"
    "  float2 uv0 = tc[0].xy / q0, uv1 = tc[1].xy / q1;\n"
    "  float2 uv2 = tc[2].xy / q2, uv3 = tc[3].xy / q3;\n"
    "  uv0 = finite_uv(uv0); uv1 = finite_uv(uv1);\n"
    "  uv2 = finite_uv(uv2); uv3 = finite_uv(uv3);\n"
    "  if (u.stages.y & 1u) uv0 /= float2(tex.get_width(), tex.get_height());\n"
    "  if (u.stages.y & 2u) uv1 /= float2(tex1.get_width(), tex1.get_height());\n"
    "  if (u.stages.y & 4u) uv2 /= float2(tex2.get_width(), tex2.get_height());\n"
    "  if (u.stages.y & 8u) uv3 /= float2(tex3.get_width(), tex3.get_height());\n"
    "  uint m0 = u.dot_state.x & 31u, m1 = (u.dot_state.x >> 5) & 31u;\n"
    "  uint m2 = (u.dot_state.x >> 10) & 31u, m3 = (u.dot_state.x >> 15) & 31u;\n"
    "  float dp1 = 0.0, dp2 = 0.0, dp3 = 0.0;\n"
    "  if (m0 == 4u) r[8] = saturate(tc[0]);\n"
    "  else if (!(u.stages.x & 1u)) r[8] = m0 == 1u ? float4(1.0) : float4(0, 0, 0, 1);\n"
    "  else if (u.texture.x) {\n"
    "    float2 uv = finite_uv(tc[0].xy / q0);\n"
    "    r[8] = 0.0;\n"
    "    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n"
    "      r[8] += legacy_linear(tex, uv + float2(x, y))\n"
    "            * (float((2 - abs(x)) * (2 - abs(y))) / 16.0);\n"
    "  } else r[8] = tex.sample(samp, uv0);\n"
    "  if (m1 == 17u) {\n"
    "    r[9] = float4(0.0); dp1 = dot(tc[1].xyz, dot_map(r[8], u.dot_state.y & 15u));\n"
    "  } else if (m1 == 4u) r[9] = saturate(tc[1]);\n"
    "  else if (m1 == 1u) r[9] = (u.stages.x & 2u) ? tex1.sample(samp1, uv1) : float4(1.0);\n"
    "  else r[9] = float4(0, 0, 0, 1);\n"
    "  if (m2 == 11u) {\n"
    "    r[10] = float4(0.0);\n"
    "    uint input2 = (u.dot_state.z >> 16) & 15u; if (input2 >= 2u) input2 = 0u;\n"
    "    dp2 = dot(tc[2].xyz, dot_map(r[8u + input2], (u.dot_state.y >> 4) & 15u));\n"
    "    uint input3 = (u.dot_state.z >> 20) & 15u; if (input3 >= 3u) input3 = 0u;\n"
    "    dp3 = dot(tc[3].xyz, dot_map(r[8u + input3], (u.dot_state.y >> 8) & 15u));\n"
    "    if (u.stages.w & 4u) r[10] = cube_sample(cube2, samp2, float3(dp1, dp2, dp3));\n"
    "  } else if (m2 == 4u) r[10] = saturate(tc[2]);\n"
    "  else if (m2 == 1u) r[10] = (u.stages.x & 4u) ? tex2.sample(samp2, uv2) : float4(1.0);\n"
    "  else r[10] = float4(0, 0, 0, 1);\n"
    "  if (m3 == 12u) {\n"
    "    r[11] = float4(0.0);\n"
    "    uint input3 = (u.dot_state.z >> 20) & 15u; if (input3 >= 3u) input3 = 0u;\n"
    "    dp3 = dot(tc[3].xyz, dot_map(r[8u + input3], (u.dot_state.y >> 8) & 15u));\n"
    "    float3 n = float3(dp1, dp2, dp3), e = float3(tc[1].w, tc[2].w, tc[3].w);\n"
    "    float nn = dot(n, n); float3 reflected = nn != 0.0 ? 2.0 * (dot(n, e) / nn) * n - e : e;\n"
    "    if (u.stages.w & 8u) r[11] = cube_sample(cube3, samp3, reflected);\n"
    "  } else if (m3 == 4u) r[11] = saturate(tc[3]);\n"
    "  else if (m3 == 1u) r[11] = (u.stages.x & 8u) ? tex3.sample(samp3, uv3) : float4(1.0);\n"
    "  else r[11] = float4(0, 0, 0, 1);\n"
    "  r[12].a = r[8].a;\n"
    "  for (uint s = 0; s < u.texture.y; ++s) {\n"
    "    r[1] = xunpack(u.factors[(u.texture.w & 0x10u) ? s : 0].x);\n"
    "    r[2] = xunpack(u.factors[(u.texture.w & 0x100u) ? s : 0].y);\n"
    "    uint ci = u.combiner[s].x, ai = u.combiner[s].y;\n"
    "    uint co = u.combiner[s].z, ao = u.combiner[s].w;\n"
    "    float3 A = xrgb(r, ci >> 24), B = xrgb(r, ci >> 16);\n"
    "    float3 C = xrgb(r, ci >> 8), D = xrgb(r, ci);\n"
    "    float aA = xa(r, ai >> 24), aB = xa(r, ai >> 16);\n"
    "    float aC = xa(r, ai >> 8), aD = xa(r, ai);\n"
    "    uint fl = co >> 12, afl = ao >> 12;\n"
    "    bool mux = u.texture.z ? r[12].a >= 0.5 : (int(r[12].a * 255.0) & 1) != 0;\n"
    "    float3 pab = (fl & 2u) ? float3(dot(A, B)) : A * B;\n"
    "    float3 pcd = (fl & 1u) ? float3(dot(C, D)) : C * D;\n"
    "    float3 psum = (fl & 4u) ? (mux ? pcd : pab) : pab + pcd;\n"
    "    float3 ab = clamp(xout(pab, fl & 0x38u), -1.0, 1.0);\n"
    "    float3 cd = clamp(xout(pcd, fl & 0x38u), -1.0, 1.0);\n"
    "    float3 sum = clamp(xout(psum, fl & 0x38u), -1.0, 1.0);\n"
    "    float apab = aA * aB, apcd = aC * aD;\n"
    "    float apsum = (afl & 4u) ? (mux ? apcd : apab) : apab + apcd;\n"
    "    float aab = clamp(xout(float3(apab), afl & 0x38u).x, -1.0, 1.0);\n"
    "    float acd = clamp(xout(float3(apcd), afl & 0x38u).x, -1.0, 1.0);\n"
    "    float asum = clamp(xout(float3(apsum), afl & 0x38u).x, -1.0, 1.0);\n"
    "    uint d = (co >> 4) & 15u; if (d) { r[d].rgb = ab; if (fl & 0x80u) r[d].a = ab.b; }\n"
    "    d = co & 15u; if (d) { r[d].rgb = cd; if (fl & 0x40u) r[d].a = cd.b; }\n"
    "    d = (co >> 8) & 15u; if (d) r[d].rgb = sum;\n"
    "    d = (ao >> 4) & 15u; if (d) r[d].a = aab;\n"
    "    d = ao & 15u; if (d) r[d].a = acd;\n"
    "    d = (ao >> 8) & 15u; if (d) r[d].a = asum;\n"
    "  }\n"
    "  uint f0 = u.final.x, f1 = u.final.y;\n"
    "  float4 o;\n"
    "  if (f0 || f1) {\n"
    "    r[1] = xunpack(u.final.z); r[2] = xunpack(u.final.w);\n"
    "    float3 v1 = (f1 & 0x40u) ? 1.0 - r[5].rgb : r[5].rgb;\n"
    "    float3 r0 = (f1 & 0x20u) ? 1.0 - r[12].rgb : r[12].rgb;\n"
    "    float3 sum = v1 + r0; r[14] = float4((f1 & 0x80u) ? saturate(sum) : sum, 0.0);\n"
    "    r[15] = float4(xrgb(r, f1 >> 24) * xrgb(r, f1 >> 16), 0.0);\n"
    "    float3 A = xrgb(r, f0 >> 24), B = xrgb(r, f0 >> 16);\n"
    "    float3 C = xrgb(r, f0 >> 8), D = xrgb(r, f0);\n"
    "    o = saturate(float4(D + C * (1.0 - A) + B * A, xa(r, f1 >> 8)));\n"
    "  } else o = saturate(r[12]);\n"
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

static int writeback(void)
{
    size_t row, bytes;
    uint8_t *pixels;

    if (!flush_commands())
        return 0;
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
    invalidate_texture_cache(s_current.color_va,
                             span_end(s_current.color_va, s_current.width,
                                      s_current.height, s_current.pitch));
    s_dirty = 0;
    return 1;
}

static id<MTLTexture> cached_surface(const Nv2aSurface *s, uint32_t format)
{
    MetalColorSurfaceKey value = {
        .color_va = s->color_va,
        .width = s->width,
        .height = s->height,
        .pitch = s->pitch,
        .format = format,
        .bytes_per_pixel = s->bytes_per_pixel,
        .aa_sx = s->aa_sx,
        .aa_sy = s->aa_sy,
    };
    NSData *key = [NSData dataWithBytes:&value length:sizeof value];
    id<MTLTexture> texture = s_color_surfaces[key];
    if (texture)
        return texture;
    if (!s_color_surfaces)
        s_color_surfaces = [NSMutableDictionary new];
    if (!s_color_surfaces)
        return metal_fail("could not create colour surface cache"), nil;
    if (s_color_surfaces.count >= METAL_MAX_COLOR_SURFACES)
        return metal_fail("colour surface cache is full"), nil;
    MTLTextureDescriptor *desc;
    desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:s->width
                                                            height:s->height
                                                         mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    texture = [s_device newTextureWithDescriptor:desc];
    if (!texture)
        return metal_fail("could not create colour surface %ux%u", s->width, s->height), nil;
    s_color_surfaces[key] = texture;
    return texture;
}

static int prepare_surface(const Nv2aSurface *s)
{
    uint32_t format = bw_backend_reg(0x0208u) & 0xFu;
    id<MTLTexture> surface;

    if (!surface_valid(s))
        return 0;
    if (depth_overlaps(s->color_va, span_end(s->color_va, s->width, s->height, s->pitch)))
        return metal_fail("colour surface aliases a cleared depth/stencil surface");
    if (s_surface && same_surface(s, &s_current) && format == s_current_format)
        return 1;
    if (!writeback())
        return 0;
    surface = cached_surface(s, format);
    if (!surface)
        return 0;
    [surface replaceRegion:MTLRegionMake2D(0, 0, s->width, s->height)
               mipmapLevel:0 withBytes:guest_pointer(s->color_va) bytesPerRow:s->pitch];
    s_surface = surface;
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
    s_command_submissions++;
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
        return metal_fail("GPU command failed: %s", metal_error_text(command.error));
    return 1;
}

static id<MTLCommandBuffer> pending_command(void)
{
    if (!s_pending_command)
        s_pending_command = [s_queue commandBuffer];
    if (!s_pending_command)
        metal_fail("could not create pending GPU command");
    return s_pending_command;
}

static int flush_commands(void)
{
    end_draw_encoder();
    if (!s_pending_command)
        return 1;
    id<MTLCommandBuffer> command = s_pending_command;
    s_pending_command = nil;
    int result = finish_command(command);
    s_vertex_upload_offset = 0;
    return result;
}

static int upload_vertices(size_t length, id<MTLBuffer> *buffer,
                           NSUInteger *offset, void **contents)
{
    size_t aligned = (s_vertex_upload_offset + 255u) & ~(size_t)255u;
    if (aligned < s_vertex_upload_offset)
        return metal_fail("vertex upload offset overflows");
    if (!s_vertex_upload || aligned > s_vertex_upload_capacity ||
        length > s_vertex_upload_capacity - aligned) {
        size_t capacity = s_vertex_upload_capacity;
        if (capacity < METAL_INITIAL_VERTEX_UPLOAD)
            capacity = METAL_INITIAL_VERTEX_UPLOAD;
        while (capacity < length) {
            if (capacity > SIZE_MAX / 2u)
                return metal_fail("vertex upload size overflows");
            capacity *= 2u;
        }
        if (capacity > s_device.maxBufferLength)
            return metal_fail("vertex upload exceeds Metal buffer limit");
        s_vertex_upload =
            [s_device newBufferWithLength:capacity options:MTLResourceStorageModeShared];
        if (!s_vertex_upload || !s_vertex_upload.contents)
            return metal_fail("could not allocate vertex upload buffer");
        s_vertex_upload_capacity = capacity;
        s_vertex_upload_offset = 0;
        aligned = 0;
        s_vertex_upload_allocations++;
    }
    *buffer = s_vertex_upload;
    *offset = aligned;
    *contents = (uint8_t *)s_vertex_upload.contents + aligned;
    s_vertex_upload_offset = aligned + length;
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
                                                int has_depth)
{
    MetalPipelineKey key = {
        .textured = (uint32_t)shader_kind,
        .color_mask = rs->color_mask & 0x01010101u,
        .fixed_alpha_zero = (bw_backend_reg(0x0208u) & 0xFu) == METAL_COLOR_XRGB_ZERO,
        .blend_enable = rs->blend_enable != 0,
        .blend_src = rs->blend_enable ? rs->blend_src : 0,
        .blend_dst = rs->blend_enable ? rs->blend_dst : 0,
        .blend_eq = rs->blend_enable ? rs->blend_eq : 0,
        .depth_attachment = has_depth != 0,
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
    state = [s_device newRenderPipelineStateWithDescriptor:pipeline error:&error];
    if (!state)
        return metal_fail("draw pipeline creation failed: %s", metal_error_text(error)), nil;
    if (s_pipelines.count >= METAL_MAX_PIPELINES)
        [s_pipelines removeAllObjects];
    s_pipelines[cache_key] = state;
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
            draw_pipeline(batch->texture != NULL, batch->state, depth != nil);
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
        id<MTLRenderPipelineState> pipeline =
            draw_pipeline(2, batch->state, depth != nil);
        id<MTLDepthStencilState> depth_state =
            draw_depth_state(batch->state, depth != nil);
        if (!pipeline || !depth_state)
            return;
        int quincunx_approx = 0;
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
        id<MTLRenderCommandEncoder> encoder = draw_encoder(depth);
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
            [encoder setFragmentSamplerState:
                s_samplers[u][v][min_linear][mag_linear][mip_mode] atIndex:stage];
        }
        [encoder setDepthStencilState:depth_state];
        [encoder setStencilReferenceValue:batch->state->stencil_ref & 0xFFu];
        set_draw_state(encoder, batch->state);
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:batch->count];
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
#endif

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
        /* Frame commands are completed by writeback below. A selected surface
         * other than the current target was synchronized when the target
         * changed, so guest memory is authoritative for both here. */
        if (!writeback())
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

static void metal_flip(void)
{
    @autoreleasepool {
        if (bw_metal_backend_error())
            return;
        if (s_dirty) {
            metal_fail("flip occurred without a successful surface readback");
            return;
        }
        uint64_t now = xbox_HostNowNs();
        if (now && s_last_flip_ns && now > s_last_flip_ns &&
            s_frame_time_count < METAL_MAX_FRAME_SAMPLES) {
            s_frame_times[s_frame_time_count++] = now - s_last_flip_ns;
            if (s_frame_timing_enabled && s_frame_time_count >= 300u &&
                !(s_frame_time_count % 100u))
                report_frame_times("300-frame window",
                                   s_frame_times + s_frame_time_count - 300u, 300u);
        }
        if (now)
            s_last_flip_ns = now;
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
    const MTLSamplerAddressMode modes[3] = {
        MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat,
        MTLSamplerAddressModeClampToEdge,
    };
    sampler.lodMinClamp = 0.0f;
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
                        s_samplers[u][v][min_linear][mag_linear][mip_mode] =
                            [s_device newSamplerStateWithDescriptor:sampler];
                        if (!s_samplers[u][v][min_linear][mag_linear][mip_mode])
                            return metal_fail("extended sampler creation failed");
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
    s_pending_command = nil;
    s_draw_encoder = nil;
    s_draw_color = nil;
    s_draw_depth = nil;
    s_command_submissions = 0;
    s_draw_encoder_creations = 0;
    s_vertex_upload = nil;
    s_vertex_upload_capacity = 0;
    s_vertex_upload_offset = 0;
    s_vertex_upload_allocations = 0;
    s_texture_uploads = 0;
    s_frame_time_count = 0;
    s_last_flip_ns = 0;
    const char *audit = getenv("BW_GPU_AUDIT");
    s_frame_timing_enabled = audit && !strcmp(audit, "1");
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
    if (s_pending_command)
        (void)flush_commands();
    if (s_frame_timing_enabled)
        report_frame_times("all", s_frame_times, s_frame_time_count);
    size_t trailing = s_frame_time_count < 300u ? s_frame_time_count : 300u;
    if (s_frame_timing_enabled && trailing && trailing != s_frame_time_count)
        report_frame_times("trailing 300",
                           s_frame_times + s_frame_time_count - trailing, trailing);
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
    s_pending_command = nil;
    s_draw_encoder = nil;
    s_draw_color = nil;
    s_draw_depth = nil;
    s_vertex_upload = nil;
    s_vertex_upload_capacity = 0;
    s_vertex_upload_offset = 0;
    s_inactive_texture = nil;
    s_inactive_cube_texture = nil;
    s_sampler = nil;
    for (unsigned u = 0; u < 3; u++)
        for (unsigned v = 0; v < 3; v++)
            for (unsigned min_linear = 0; min_linear < 2; min_linear++)
                for (unsigned mag_linear = 0; mag_linear < 2; mag_linear++)
                    for (unsigned mip_mode = 0; mip_mode < 3; mip_mode++)
                        s_samplers[u][v][min_linear][mag_linear][mip_mode] = nil;
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
    s_clear_vertex_function = nil;
    s_clear_color_function = nil;
    s_clear_depth_function = nil;
    s_queue = nil;
    s_device = nil;
    s_guest = 0;
    memset(s_guest_regions, 0, sizeof s_guest_regions);
    memset(&s_current, 0, sizeof s_current);
}
