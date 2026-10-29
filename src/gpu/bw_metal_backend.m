#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_metal_backend.h"
#include "nv2a_backend.h"
#include "../toolkit_patches/kernel/bw_backend_ext.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define METAL_TEST_MAX_DIMENSION 16384u
#define METAL_TEST_MAX_PIPELINES 128u

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
    uint32_t textured, color_mask;
    uint32_t blend_enable, blend_src, blend_dst, blend_eq;
} MetalPipelineKey;

static uint8_t *s_guest;
static size_t s_guest_size;
static char s_error[512];
static int s_registered;
static int s_dirty;
static Nv2aSurface s_current;
static id<MTLDevice> s_device;
static id<MTLCommandQueue> s_queue;
static id<MTLFunction> s_vertex_function;
static id<MTLFunction> s_color_function;
static id<MTLFunction> s_texture_function;
static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *s_pipelines;
static id<MTLSamplerState> s_sampler;
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
    "}\n";

static int metal_fail(const char *format, ...)
{
    if (!s_error[0]) {
        va_list args;
        va_start(args, format);
        vsnprintf(s_error, sizeof s_error, format, args);
        va_end(args);
        fprintf(stderr, "[GPU-METAL] %s\n", s_error);
    }
    return 0;
}

static const char *metal_error_text(NSError *error)
{
    return error ? error.localizedDescription.UTF8String : "unknown Metal error";
}

const char *bw_metal_backend_error(void)
{
    return s_error[0] ? s_error : NULL;
}

static int span_valid(uint32_t address, uint32_t width, uint32_t height,
                      uint32_t pitch)
{
    size_t row;
    size_t available;

    if (!width || !height || (address & 3u) || (pitch & 3u) ||
        (size_t)width > SIZE_MAX / 4u)
        return 0;
    row = (size_t)width * 4u;
    if (pitch < row || address > s_guest_size)
        return 0;
    available = s_guest_size - address;
    return row <= available && (size_t)(height - 1u) <= (available - row) / pitch;
}

static int surface_valid(const Nv2aSurface *s)
{
    uint32_t format = bw_backend_reg(0x0208u);
    if (!s || s->bytes_per_pixel != 4 || s->aa_sx != 1 || s->aa_sy != 1 ||
        (format & 0xFu) != 8u || ((format >> 8) & 0xFu) > 1u)
        return metal_fail("unsupported colour surface format or anti-aliasing");
    if (s->width > METAL_TEST_MAX_DIMENSION || s->height > METAL_TEST_MAX_DIMENSION)
        return metal_fail("colour surface dimensions exceed %u", METAL_TEST_MAX_DIMENSION);
    if ((bw_backend_reg(0x0200u) & 0xFFFFu) || (bw_backend_reg(0x0204u) & 0xFFFFu))
        return metal_fail("nonzero surface clip origin is unsupported");
    if (!span_valid(s->color_va, s->width, s->height, s->pitch))
        return metal_fail("invalid colour surface guest-memory span");
    return 1;
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
        memcpy(s_guest + s_current.color_va + (size_t)y * s_current.pitch,
               pixels + (size_t)y * row, row);
    free(pixels);
    s_dirty = 0;
    return 1;
}

static int prepare_surface(const Nv2aSurface *s)
{
    MTLTextureDescriptor *desc;

    if (!surface_valid(s))
        return 0;
    if (s_surface && same_surface(s, &s_current))
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
                 mipmapLevel:0 withBytes:s_guest + s->color_va bytesPerRow:s->pitch];
    s_current = *s;
    s_dirty = 0;
    return 1;
}

static int finish_command(id<MTLCommandBuffer> command)
{
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
        return metal_fail("GPU command failed: %s", metal_error_text(command.error));
    return 1;
}

static int validate_state(const Nv2aRenderState *rs)
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
        bw_backend_reg(0x17F8u) || bw_backend_combiner_seen() ||
        bw_backend_reg(0x1E70u))
        return metal_fail("unsupported fog, polygon offset, clip plane or combiner state");
    if (!isfinite(rs->depth_min) || !isfinite(rs->depth_max) ||
        rs->depth_max <= rs->depth_min || !isfinite(rs->depth_max - rs->depth_min))
        return metal_fail("invalid depth range");
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

static id<MTLRenderPipelineState> draw_pipeline(int textured, const Nv2aRenderState *rs)
{
    MetalPipelineKey key = {
        .textured = textured != 0,
        .color_mask = rs->color_mask & 0x01010101u,
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
    }
    NSData *cache_key = [NSData dataWithBytes:&key length:sizeof key];
    id<MTLRenderPipelineState> state = s_pipelines[cache_key];
    if (state)
        return state;
    MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = s_vertex_function;
    pipeline.fragmentFunction = textured ? s_texture_function : s_color_function;
    MTLRenderPipelineColorAttachmentDescriptor *attachment = pipeline.colorAttachments[0];
    attachment.pixelFormat = MTLPixelFormatBGRA8Unorm;
    attachment.writeMask = MTLColorWriteMaskNone;
    if (key.color_mask & 0x00010000u) attachment.writeMask |= MTLColorWriteMaskRed;
    if (key.color_mask & 0x00000100u) attachment.writeMask |= MTLColorWriteMaskGreen;
    if (key.color_mask & 0x00000001u) attachment.writeMask |= MTLColorWriteMaskBlue;
    if (key.color_mask & 0x01000000u) attachment.writeMask |= MTLColorWriteMaskAlpha;
    if (key.blend_enable) {
        attachment.blendingEnabled = YES;
        attachment.sourceRGBBlendFactor = src_rgb;
        attachment.destinationRGBBlendFactor = dst_rgb;
        attachment.sourceAlphaBlendFactor = src_alpha;
        attachment.destinationAlphaBlendFactor = dst_alpha;
        attachment.rgbBlendOperation = operation;
        attachment.alphaBlendOperation = operation;
    }
    NSError *error = nil;
    state = [s_device newRenderPipelineStateWithDescriptor:pipeline error:&error];
    if (!state)
        return metal_fail("draw pipeline creation failed: %s", metal_error_text(error)), nil;
    if (s_pipelines.count >= METAL_TEST_MAX_PIPELINES)
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
    if (tex->width > METAL_TEST_MAX_DIMENSION || tex->height > METAL_TEST_MAX_DIMENSION)
        return metal_fail("stage-0 texture dimensions exceed %u", METAL_TEST_MAX_DIMENSION), nil;
    if (!span_valid(tex->offset, tex->width, tex->height, tex->pitch) ||
        (size_t)tex->width > SIZE_MAX / tex->height ||
        (size_t)tex->width * tex->height > SIZE_MAX / sizeof(uint32_t))
        return metal_fail("invalid stage-0 texture guest-memory span"), nil;
    if (s_dirty) {
        size_t texture_end = (size_t)tex->offset + (size_t)(tex->height - 1u) * tex->pitch
                           + (size_t)tex->width * 4u;
        size_t surface_end = (size_t)s_current.color_va
                           + (size_t)(s_current.height - 1u) * s_current.pitch
                           + (size_t)s_current.width * 4u;
        if (tex->offset < surface_end && s_current.color_va < texture_end)
            return metal_fail("sampling a pending render target is unsupported"), nil;
    }
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

static void metal_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                        uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    (void)rs;
    (void)zstencil;
    @autoreleasepool {
        uint32_t rect[4];
        if (s_error[0])
            return;
        if (flags != 0xF0u) {
            metal_fail("unsupported clear flags 0x%08x", flags);
            return;
        }
        uint32_t rect_mask = bw_backend_clear_rect_mask();
        if (rect_mask && rect_mask != 3u) {
            metal_fail("incomplete clear rectangle is unsupported");
            return;
        }
        if (bw_backend_clear_rect(rect) &&
            (rect[0] || rect[1] || !s || rect[2] != s->width || rect[3] != s->height)) {
            metal_fail("partial clear rectangles are unsupported");
            return;
        }
        if (!prepare_surface(s))
            return;
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = s_surface;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor =
            MTLClearColorMake(((argb >> 16) & 255u) / 255.0,
                              ((argb >> 8) & 255u) / 255.0,
                              (argb & 255u) / 255.0, (argb >> 24) / 255.0);
        id<MTLCommandBuffer> command = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!command || !encoder) {
            metal_fail("could not create clear command");
            return;
        }
        [encoder endEncoding];
        if (finish_command(command))
            s_dirty = 1;
    }
}

static void metal_draw(const Nv2aSurface *s, const Nv2aBatch *batch)
{
    @autoreleasepool {
        if (s_error[0])
            return;
        if (!batch || !batch->vertices || !batch->count || batch->count % 3u ||
            (size_t)batch->count > SIZE_MAX / sizeof(MetalVertex)) {
            metal_fail("invalid triangle batch");
            return;
        }
        if (!validate_state(batch->state))
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
        if (batch->state->cull_enable) {
            [encoder setFrontFacingWinding:batch->state->front_face == 0x900u
                                            ? MTLWindingClockwise : MTLWindingCounterClockwise];
            [encoder setCullMode:batch->state->cull_face == 0x404u ? MTLCullModeFront : MTLCullModeBack];
        }
        if (batch->state->blend_enable) {
            uint32_t color = batch->state->blend_color;
            [encoder setBlendColorRed:((color >> 16) & 255u) / 255.0f
                                green:((color >> 8) & 255u) / 255.0f
                                 blue:(color & 255u) / 255.0f
                                alpha:((color >> 24) & 255u) / 255.0f];
        }
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
    (void)surface;
    (void)batch;
    metal_fail("register-combiner batches are unsupported");
}

static void metal_flip(void)
{
    @autoreleasepool {
        if (s_error[0])
            return;
        if (!s_surface) {
            metal_fail("flip without a colour surface");
            return;
        }
        if (!writeback())
            return;
        s_surface = nil;
    }
}

static const Nv2aBackend s_backend = {metal_clear, metal_draw, metal_flip};
static const BwXBackend s_extended_backend = {metal_draw_extended, NULL, NULL, NULL};

static int initialize_metal(void)
{
    NSError *error = nil;
    s_device = MTLCreateSystemDefaultDevice();
    if (!s_device)
        return metal_fail("no Metal device");
    s_queue = [s_device newCommandQueue];
    if (!s_queue)
        return metal_fail("could not create Metal command queue");
    id<MTLLibrary> library = [s_device newLibraryWithSource:@(s_shader) options:nil error:&error];
    if (!library)
        return metal_fail("shader compilation failed: %s", metal_error_text(error));
    s_vertex_function = [library newFunctionWithName:@"vertex_main"];
    s_color_function = [library newFunctionWithName:@"color_main"];
    s_texture_function = [library newFunctionWithName:@"texture_main"];
    if (!s_vertex_function || !s_color_function || !s_texture_function)
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
    return 1;
}

int bw_metal_backend_start_for_test(void *guest_memory, size_t guest_size)
{
    @autoreleasepool {
        if (s_registered)
            return metal_fail("backend is already registered");
        s_error[0] = '\0';
        if (!guest_memory || !guest_size || ((uintptr_t)guest_memory & 3u) ||
            (uintptr_t)guest_memory != (uintptr_t)xbox_GetMemoryOffset())
            return metal_fail("synthetic guest memory does not match the executor");
        s_guest = guest_memory;
        s_guest_size = guest_size;
        if (!initialize_metal()) {
            bw_metal_backend_stop();
            return 0;
        }
        nv2a_backend_register(&s_backend);
        bw_backend_ext_register(&s_extended_backend);
        s_registered = 1;
        return 1;
    }
}

void bw_metal_backend_stop(void)
{
    if (s_registered) {
        bw_backend_ext_register(NULL);
        nv2a_backend_register(NULL);
    }
    s_registered = 0;
    s_dirty = 0;
    s_surface = nil;
    s_sampler = nil;
    s_pipelines = nil;
    s_vertex_function = nil;
    s_color_function = nil;
    s_texture_function = nil;
    s_queue = nil;
    s_device = nil;
    s_guest = NULL;
    s_guest_size = 0;
    memset(&s_current, 0, sizeof s_current);
}
