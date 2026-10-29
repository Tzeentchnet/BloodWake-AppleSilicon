#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdint.h>
#include <stdio.h>

static int fail(const char *step, NSError *error)
{
    fprintf(stderr, "Metal smoke test: %s: %s\n", step,
            error ? error.localizedDescription.UTF8String : "unavailable");
    return 1;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
            return fail("no Metal device", nil);

        NSError *error = nil;
        NSString *source =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "vertex float4 vertex_main(uint i [[vertex_id]]) {\n"
             "  const float2 v[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};\n"
             "  return float4(v[i], 0, 1);\n"
             "}\n"
             "fragment float4 fragment_main() { return float4(1, 0, 0, 1); }\n";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library)
            return fail("shader compilation", error);

        MTLRenderPipelineDescriptor *pipelineDesc = [MTLRenderPipelineDescriptor new];
        pipelineDesc.vertexFunction = [library newFunctionWithName:@"vertex_main"];
        pipelineDesc.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
        pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        if (!pipelineDesc.vertexFunction || !pipelineDesc.fragmentFunction)
            return fail("shader entry points", nil);
        id<MTLRenderPipelineState> pipeline =
            [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
        if (!pipeline)
            return fail("render pipeline", error);

        MTLTextureDescriptor *textureDesc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:4 height:4 mipmapped:NO];
        textureDesc.storageMode = MTLStorageModeShared;
        textureDesc.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:textureDesc];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!texture || !queue)
            return fail("texture or command queue", nil);

        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!command || !encoder)
            return fail("command buffer or render encoder", nil);
        [encoder setRenderPipelineState:pipeline];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted)
            return fail("GPU execution", command.error);

        uint8_t pixels[4 * 4 * 4];
        [texture getBytes:pixels bytesPerRow:4 * 4
               fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
        for (size_t i = 0; i < sizeof pixels; i += 4) {
            if (pixels[i] != 255 || pixels[i + 1] != 0 ||
                pixels[i + 2] != 0 || pixels[i + 3] != 255) {
                fprintf(stderr, "Metal smoke test: unexpected pixel at %zu: %u,%u,%u,%u\n",
                        i / 4, pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]);
                return 1;
            }
        }
        printf("Metal shader compilation, draw and readback passed on %s\n", device.name.UTF8String);
        return 0;
    }
}
