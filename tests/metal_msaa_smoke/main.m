#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "metal_msaa_smoke.h"

static NSString *const kShaderSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VertexOut { float4 position [[position]]; };\n"
     "vertex VertexOut vertex_main(uint vertex_id [[vertex_id]],\n"
     "                             constant float &depth [[buffer(0)]]) {\n"
     "  const float2 positions[3] = {\n"
     "    float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)\n"
     "  };\n"
     "  VertexOut out;\n"
     "  out.position = float4(positions[vertex_id], depth, 1.0);\n"
     "  return out;\n"
     "}\n"
     "fragment float4 fragment_main(constant float4 &color [[buffer(0)]]) {\n"
     "  return color;\n"
     "}\n";

static const char *utf8_or_fallback(NSString *value, const char *fallback)
{
    const char *utf8 = value.UTF8String;
    return utf8 ? utf8 : fallback;
}

static void copy_device_name(BwMetalMsaaSmokeReport *report, NSString *name)
{
    const char *utf8 = name.UTF8String;
    if (!utf8)
        return;
    snprintf(report->device_name, sizeof report->device_name, "%s", utf8);
}

static void set_nserror(BwMetalMsaaSmokeSampleResult *result,
                        BwMetalMsaaSmokeErrorStage stage,
                        NSError *error, const char *fallback)
{
    if (error) {
        bw_metal_msaa_smoke_set_sample_error(
            result, stage, 1, (int64_t)error.code,
            utf8_or_fallback(error.localizedDescription, fallback));
    } else {
        bw_metal_msaa_smoke_set_sample_error(
            result, stage, 0, 0, fallback);
    }
}

static void set_exception(BwMetalMsaaSmokeSampleResult *result,
                          NSException *exception)
{
    NSString *description =
        [NSString stringWithFormat:@"%@: %@", exception.name,
         exception.reason ?: @"no reason provided"];
    bw_metal_msaa_smoke_set_sample_error(
        result, BW_METAL_MSAA_SMOKE_ERROR_EXCEPTION, 0, 0,
        utf8_or_fallback(description, "Metal API raised an exception"));
}

static void encode_draw(id<MTLRenderCommandEncoder> encoder,
                        NSUInteger x, NSUInteger y,
                        NSUInteger width, NSUInteger height,
                        float depth,
                        const float color[4],
                        uint32_t stencil_reference)
{
    MTLScissorRect scissor = { x, y, width, height };
    [encoder setScissorRect:scissor];
    [encoder setVertexBytes:&depth length:sizeof depth atIndex:0];
    [encoder setFragmentBytes:color
                       length:sizeof(float) * 4
                      atIndex:0];
    [encoder setStencilReferenceValue:stencil_reference];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
}

static MTLTextureDescriptor *multisample_texture_descriptor(
    MTLPixelFormat format, NSUInteger sample_count)
{
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType2DMultisample;
    descriptor.pixelFormat = format;
    descriptor.width = BW_METAL_MSAA_SMOKE_WIDTH;
    descriptor.height = BW_METAL_MSAA_SMOKE_HEIGHT;
    descriptor.depth = 1;
    descriptor.mipmapLevelCount = 1;
    descriptor.sampleCount = sample_count;
    descriptor.arrayLength = 1;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget;
    return descriptor;
}

static void run_sample(id<MTLDevice> device,
                       id<MTLCommandQueue> queue,
                       id<MTLFunction> vertex_function,
                       id<MTLFunction> fragment_function,
                       BwMetalMsaaSmokeSampleResult *result)
{
    @try {
        NSError *error = nil;
        MTLRenderPipelineDescriptor *pipeline_descriptor =
            [MTLRenderPipelineDescriptor new];
        pipeline_descriptor.label =
            [NSString stringWithFormat:@"MSAA %ux diagnostic pipeline",
                                       result->sample_count];
        pipeline_descriptor.vertexFunction = vertex_function;
        pipeline_descriptor.fragmentFunction = fragment_function;
        pipeline_descriptor.rasterSampleCount = result->sample_count;
        pipeline_descriptor.colorAttachments[0].pixelFormat =
            MTLPixelFormatRGBA8Unorm;
        pipeline_descriptor.depthAttachmentPixelFormat =
            MTLPixelFormatDepth32Float_Stencil8;
        pipeline_descriptor.stencilAttachmentPixelFormat =
            MTLPixelFormatDepth32Float_Stencil8;

        id<MTLRenderPipelineState> pipeline =
            [device newRenderPipelineStateWithDescriptor:pipeline_descriptor
                                                   error:&error];
        if (!pipeline) {
            set_nserror(result, BW_METAL_MSAA_SMOKE_ERROR_PIPELINE,
                        error, "sample-count-matched pipeline creation failed");
            return;
        }

        MTLDepthStencilDescriptor *depth_descriptor =
            [MTLDepthStencilDescriptor new];
        depth_descriptor.label = @"MSAA diagnostic depth/stencil state";
        depth_descriptor.depthCompareFunction = MTLCompareFunctionLess;
        depth_descriptor.depthWriteEnabled = YES;
        MTLStencilDescriptor *stencil_descriptor =
            [MTLStencilDescriptor new];
        stencil_descriptor.stencilCompareFunction = MTLCompareFunctionEqual;
        stencil_descriptor.stencilFailureOperation = MTLStencilOperationKeep;
        stencil_descriptor.depthFailureOperation = MTLStencilOperationKeep;
        stencil_descriptor.depthStencilPassOperation = MTLStencilOperationKeep;
        stencil_descriptor.readMask = 0xff;
        stencil_descriptor.writeMask = 0;
        depth_descriptor.frontFaceStencil = stencil_descriptor;
        depth_descriptor.backFaceStencil = stencil_descriptor;
        id<MTLDepthStencilState> depth_state =
            [device newDepthStencilStateWithDescriptor:depth_descriptor];
        if (!depth_state) {
            bw_metal_msaa_smoke_set_sample_error(
                result, BW_METAL_MSAA_SMOKE_ERROR_DEPTH_STENCIL_STATE,
                0, 0, "depth/stencil state creation failed");
            return;
        }

        MTLTextureDescriptor *color_descriptor =
            multisample_texture_descriptor(
                MTLPixelFormatRGBA8Unorm, result->sample_count);
        MTLTextureDescriptor *depth_texture_descriptor =
            multisample_texture_descriptor(
                MTLPixelFormatDepth32Float_Stencil8, result->sample_count);
        MTLTextureDescriptor *resolve_descriptor =
            [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                              width:BW_METAL_MSAA_SMOKE_WIDTH
                                             height:BW_METAL_MSAA_SMOKE_HEIGHT
                                          mipmapped:NO];
        resolve_descriptor.storageMode = MTLStorageModePrivate;
        resolve_descriptor.usage =
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

        id<MTLTexture> color_texture =
            [device newTextureWithDescriptor:color_descriptor];
        id<MTLTexture> depth_texture =
            [device newTextureWithDescriptor:depth_texture_descriptor];
        id<MTLTexture> resolve_texture =
            [device newTextureWithDescriptor:resolve_descriptor];
        NSUInteger readback_length =
            BW_METAL_MSAA_SMOKE_READBACK_BYTES_PER_ROW *
            BW_METAL_MSAA_SMOKE_HEIGHT;
        id<MTLBuffer> readback =
            [device newBufferWithLength:readback_length
                               options:MTLResourceStorageModeShared];
        if (!color_texture || !depth_texture ||
            !resolve_texture || !readback) {
            bw_metal_msaa_smoke_set_sample_error(
                result, BW_METAL_MSAA_SMOKE_ERROR_RESOURCE_ALLOCATION,
                0, 0,
                "multisample color, depth/stencil, resolve, or readback "
                "resource allocation failed");
            return;
        }
        color_texture.label = @"MSAA diagnostic color";
        depth_texture.label = @"MSAA diagnostic depth/stencil";
        resolve_texture.label = @"MSAA diagnostic resolve";
        readback.label = @"MSAA diagnostic synchronized readback";
        result->resources_allocated = 1;
        result->allocated_bytes_lower_bound =
            bw_metal_msaa_smoke_allocated_bytes_lower_bound(
                result->sample_count);

        MTLRenderPassDescriptor *pass =
            [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color_texture;
        pass.colorAttachments[0].resolveTexture = resolve_texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction =
            MTLStoreActionMultisampleResolve;
        pass.colorAttachments[0].clearColor =
            MTLClearColorMake(0.0, 0.0, 1.0, 1.0);
        pass.depthAttachment.texture = depth_texture;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 0.5;
        pass.stencilAttachment.texture = depth_texture;
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
        pass.stencilAttachment.clearStencil = 3;

        MTLCommandBufferDescriptor *command_descriptor =
            [MTLCommandBufferDescriptor new];
        command_descriptor.errorOptions =
            MTLCommandBufferErrorOptionEncoderExecutionStatus;
        id<MTLCommandBuffer> command =
            [queue commandBufferWithDescriptor:command_descriptor];
        if (!command) {
            bw_metal_msaa_smoke_set_sample_error(
                result, BW_METAL_MSAA_SMOKE_ERROR_COMMAND_BUFFER,
                0, 0, "command buffer creation failed");
            return;
        }
        command.label =
            [NSString stringWithFormat:@"MSAA %ux diagnostic command",
                                       result->sample_count];

        id<MTLRenderCommandEncoder> encoder =
            [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) {
            bw_metal_msaa_smoke_set_sample_error(
                result, BW_METAL_MSAA_SMOKE_ERROR_ENCODER,
                0, 0, "render command encoder creation failed");
            return;
        }
        encoder.label = @"MSAA diagnostic render and resolve";
        [encoder setRenderPipelineState:pipeline];
        [encoder setDepthStencilState:depth_state];
        [encoder setViewport:(MTLViewport){
            0.0, 0.0,
            BW_METAL_MSAA_SMOKE_WIDTH,
            BW_METAL_MSAA_SMOKE_HEIGHT,
            0.0, 1.0
        }];

        const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        const float yellow[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
        const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };

        encode_draw(encoder, 0, 0, 4, 4, 0.75f, green, 3);
        encode_draw(encoder, 0, 4, 2, 4, 0.25f, red, 3);
        encode_draw(encoder, 2, 4, 2, 4, 0.25f, red, 3);
        encode_draw(encoder, 2, 4, 2, 4, 0.375f, yellow, 3);
        encode_draw(encoder, 4, 4, 4, 4, 0.25f, magenta, 4);
        [encoder endEncoding];

        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!blit) {
            bw_metal_msaa_smoke_set_sample_error(
                result, BW_METAL_MSAA_SMOKE_ERROR_ENCODER,
                0, 0, "readback blit command encoder creation failed");
            return;
        }
        blit.label = @"MSAA diagnostic resolve readback";
        [blit copyFromTexture:resolve_texture
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(
                      BW_METAL_MSAA_SMOKE_WIDTH,
                      BW_METAL_MSAA_SMOKE_HEIGHT, 1)
                    toBuffer:readback
           destinationOffset:0
      destinationBytesPerRow:BW_METAL_MSAA_SMOKE_READBACK_BYTES_PER_ROW
    destinationBytesPerImage:readback_length];
        [blit endEncoding];

        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            set_nserror(result, BW_METAL_MSAA_SMOKE_ERROR_COMMAND_BUFFER,
                        command.error,
                        "command buffer did not complete successfully");
            return;
        }

        CFTimeInterval gpu_start = command.GPUStartTime;
        CFTimeInterval gpu_end = command.GPUEndTime;
        if (isfinite(gpu_start) && isfinite(gpu_end) &&
            gpu_start > 0.0 && gpu_end >= gpu_start) {
            result->gpu_timing_available = 1;
            result->gpu_time_milliseconds =
                (gpu_end - gpu_start) * 1000.0;
        }

        const uint8_t *pixels = readback.contents;
        if (!pixels) {
            bw_metal_msaa_smoke_set_sample_error(
                result, BW_METAL_MSAA_SMOKE_ERROR_READBACK,
                0, 0, "completed shared readback buffer has no CPU address");
            return;
        }
        bw_metal_msaa_smoke_check_pixels(
            result, pixels, BW_METAL_MSAA_SMOKE_READBACK_BYTES_PER_ROW);
        if (!bw_metal_msaa_smoke_checks_pass(&result->correctness)) {
            bw_metal_msaa_smoke_set_sample_error(
                result, BW_METAL_MSAA_SMOKE_ERROR_CORRECTNESS,
                0, 0,
                "resolved pixels failed clear, draw, depth/stencil, "
                "or resolve validation");
            return;
        }
        result->status = BW_METAL_MSAA_SMOKE_PASSED;
    } @catch (NSException *exception) {
        set_exception(result, exception);
    }
}

static void fail_supported_not_run(BwMetalMsaaSmokeReport *report,
                                   BwMetalMsaaSmokeErrorStage stage,
                                   NSError *error, const char *message)
{
    size_t i;
    for (i = 0; i < BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL; i++) {
        BwMetalMsaaSmokeSampleResult *result = &report->results[i];
        if (result->supported &&
            result->status == BW_METAL_MSAA_SMOKE_NOT_RUN)
            set_nserror(result, stage, error, message);
    }
}

int main(void)
{
    BwMetalMsaaSmokeReport report;
    char output[16384];

    bw_metal_msaa_smoke_report_init(&report);
    @autoreleasepool {
        @try {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) {
                bw_metal_msaa_smoke_set_fatal_error(
                    &report, BW_METAL_MSAA_SMOKE_ERROR_DEVICE,
                    0, 0, "MTLCreateSystemDefaultDevice returned no device");
            } else {
                copy_device_name(&report, device.name);
                for (size_t i = 0;
                     i < BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL; i++) {
                    BwMetalMsaaSmokeSampleResult *result = &report.results[i];
                    if ([device supportsTextureSampleCount:
                            result->sample_count]) {
                        result->supported = 1;
                    } else {
                        result->status =
                            BW_METAL_MSAA_SMOKE_SKIPPED_UNSUPPORTED;
                    }
                }

                NSError *error = nil;
                id<MTLLibrary> library =
                    [device newLibraryWithSource:kShaderSource
                                         options:nil
                                           error:&error];
                if (!library) {
                    fail_supported_not_run(
                        &report, BW_METAL_MSAA_SMOKE_ERROR_SHADER,
                        error, "diagnostic shader compilation failed");
                } else {
                    id<MTLFunction> vertex =
                        [library newFunctionWithName:@"vertex_main"];
                    id<MTLFunction> fragment =
                        [library newFunctionWithName:@"fragment_main"];
                    if (!vertex || !fragment) {
                        fail_supported_not_run(
                            &report, BW_METAL_MSAA_SMOKE_ERROR_SHADER,
                            nil, "diagnostic shader entry point is unavailable");
                    } else {
                        id<MTLCommandQueue> queue = [device newCommandQueue];
                        if (!queue) {
                            fail_supported_not_run(
                                &report,
                                BW_METAL_MSAA_SMOKE_ERROR_COMMAND_QUEUE,
                                nil, "Metal command queue creation failed");
                        } else {
                            queue.label = @"MSAA diagnostic command queue";
                            for (size_t i = 0;
                                 i < BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL;
                                 i++) {
                                BwMetalMsaaSmokeSampleResult *result =
                                    &report.results[i];
                                if (result->supported)
                                    run_sample(device, queue, vertex, fragment,
                                               result);
                            }
                        }
                    }
                }
            }
        } @catch (NSException *exception) {
            NSString *description =
                [NSString stringWithFormat:@"%@: %@", exception.name,
                 exception.reason ?: @"no reason provided"];
            bw_metal_msaa_smoke_set_fatal_error(
                &report, BW_METAL_MSAA_SMOKE_ERROR_EXCEPTION,
                0, 0,
                utf8_or_fallback(
                    description, "Metal API raised an exception"));
        }
    }

    if (bw_metal_msaa_smoke_format_json(
            &report, output, sizeof output)) {
        fprintf(stderr, "Metal MSAA smoke JSON formatting failed\n");
        return 2;
    }
    puts(output);
    return bw_metal_msaa_smoke_report_succeeded(&report) ? 0 : 1;
}
