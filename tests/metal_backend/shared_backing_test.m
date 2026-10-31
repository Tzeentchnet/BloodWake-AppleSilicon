#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int clear_texture(id<MTLCommandQueue> queue, id<MTLTexture> texture,
                         double red, double blue)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(red, 0.0, blue, 1.0);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!command || !encoder) {
        fprintf(stderr, "Could not create Metal clear command\n");
        return 0;
    }
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "Metal clear failed: %s\n", command.error.localizedDescription.UTF8String);
        return 0;
    }
    return 1;
}

static int exercise_shared_backing(void *memory, size_t total, size_t second_offset)
{
    @autoreleasepool {
        const NSUInteger height = 480, first_pitch = 2560, second_pitch = 5120;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            fprintf(stderr, "Metal device unavailable\n");
            return 0;
        }
        NSUInteger alignment =
            [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm];
        if (!alignment || first_pitch % alignment || second_pitch % alignment ||
            second_offset % alignment) {
            fprintf(stderr, "Test surface layout does not meet Metal alignment requirements\n");
            return 0;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:memory length:total
                                                       options:MTLResourceStorageModeShared
                                                   deallocator:nil];
        MTLTextureDescriptor *first_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              width:640 height:height mipmapped:NO];
        first_desc.storageMode = MTLStorageModeShared;
        first_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        MTLTextureDescriptor *second_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              width:1280 height:height mipmapped:NO];
        second_desc.storageMode = MTLStorageModeShared;
        second_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> first = [buffer newTextureWithDescriptor:first_desc offset:0
                                                   bytesPerRow:first_pitch];
        id<MTLTexture> alias = [buffer newTextureWithDescriptor:first_desc offset:0
                                                   bytesPerRow:first_pitch];
        id<MTLTexture> second = [buffer newTextureWithDescriptor:second_desc offset:second_offset
                                                    bytesPerRow:second_pitch];
        if (!queue || !buffer || !first || !alias || !second) {
            fprintf(stderr, "Could not create buffer-backed Metal render targets\n");
            return 0;
        }
        if (!clear_texture(queue, first, 1.0, 0.0))
            return 0;
        uint32_t *first_pixel = memory;
        uint32_t *first_last = (uint32_t *)((uint8_t *)memory + 479u * first_pitch + 639u * 4u);
        uint32_t *second_pixel = (uint32_t *)((uint8_t *)memory + second_offset);
        uint32_t *second_last =
            (uint32_t *)((uint8_t *)memory + second_offset + 479u * second_pitch + 1279u * 4u);
        if (*first_pixel != 0xFFFF0000u || *first_last != 0xFFFF0000u) {
            fprintf(stderr, "GPU clear not visible in host backing\n");
            return 0;
        }
        *first_pixel = 0xFF123456u;
        if (!clear_texture(queue, second, 0.0, 1.0))
            return 0;
        id<MTLBuffer> probe = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!probe || !command || !blit) {
            fprintf(stderr, "Could not create Metal readback command\n");
            return 0;
        }
        [blit copyFromTexture:alias sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1)
                     toBuffer:probe destinationOffset:0 destinationBytesPerRow:256
     destinationBytesPerImage:256];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "Metal readback failed: %s\n", command.error.localizedDescription.UTF8String);
            return 0;
        }
        if (*first_pixel != 0xFF123456u || *(uint32_t *)probe.contents != 0xFF123456u ||
            *first_last != 0xFFFF0000u ||
            *second_pixel != 0xFF0000FFu || *second_last != 0xFF0000FFu) {
            fprintf(stderr, "Shared backing or ordered CPU write visibility mismatch\n");
            return 0;
        }
        return 1;
    }
}

static int exercise_queued_write_counterexample(void *memory, void *writer, size_t total)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLSharedEvent> event = [device newSharedEvent];
        id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:memory length:total
                                                       options:MTLResourceStorageModeShared
                                                   deallocator:nil];
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              width:640 height:480 mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [buffer newTextureWithDescriptor:desc offset:0 bytesPerRow:2560];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (!device || !queue || !event || !buffer || !texture || !command) {
            fprintf(stderr, "Could not create reuse counterexample resources\n");
            return 0;
        }
        [command encodeWaitForEvent:event value:1];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 1.0);
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) {
            fprintf(stderr, "Could not create reuse counterexample clear\n");
            return 0;
        }
        [encoder endEncoding];
        [command commit];
        *(uint32_t *)writer = 0xFF123456u;
        event.signaledValue = 1;
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "Reuse counterexample failed: %s\n", command.error.localizedDescription.UTF8String);
            return 0;
        }
        if (*(uint32_t *)memory != 0xFFFF0000u || *(uint32_t *)writer != 0xFFFF0000u) {
            fprintf(stderr, "Queued write did not overwrite the conflicting host write\n");
            return 0;
        }
        return 1;
    }
}

static void expected_protection_fault(int signal_number)
{
    _exit(signal_number == SIGSEGV || signal_number == SIGBUS ? 84 : 85);
}

static int protected_access_faults(void *memory)
{
    pid_t pid = fork();
    int status;
    if (pid < 0) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        struct sigaction action = {0};
        action.sa_handler = expected_protection_fault;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, NULL) || sigaction(SIGBUS, &action, NULL))
            _exit(85);
        volatile uint8_t value = *(volatile uint8_t *)memory;
        (void)value;
        _exit(0);
    }
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        return 0;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 84) {
        fprintf(stderr, "Protected CPU mapping did not fault\n");
        return 0;
    }
    return 1;
}

static int remap_shared(void *memory, size_t total, mach_vm_address_t *alias)
{
    vm_prot_t current, maximum;
    *alias = 0;
    kern_return_t result = mach_vm_remap(mach_task_self(), alias, total, 0,
                                        VM_FLAGS_ANYWHERE, mach_task_self(),
                                        (mach_vm_address_t)(uintptr_t)memory,
                                        FALSE, &current, &maximum, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "Could not remap CPU backing: %s\n", mach_error_string(result));
        *alias = 0;
        return 0;
    }
    return 1;
}

static int release_alias(mach_vm_address_t alias, size_t total)
{
    kern_return_t result = mach_vm_deallocate(mach_task_self(), alias, total);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "Could not release remapped backing: %s\n", mach_error_string(result));
        return 0;
    }
    return 1;
}

static int exercise_protected_alias(void *memory, size_t total, int with_tiled)
{
    mach_vm_address_t alias = 0, tiled = 0;
    int tiled_protected = 0;
    if (!remap_shared(memory, total, &alias))
        return 0;
    int ok = 0;
    if (with_tiled && !remap_shared(memory, total, &tiled))
        goto done;
    *(uint32_t *)memory = 0xFF123456u;
    if (*(uint32_t *)(uintptr_t)alias != 0xFF123456u) {
        fprintf(stderr, "CPU mappings do not share backing\n");
        goto done;
    }
    if (mprotect(memory, total, PROT_NONE) != 0) {
        perror("mprotect");
        goto done;
    }
    if (with_tiled) {
        if (mprotect((void *)(uintptr_t)tiled, total, PROT_NONE) != 0) {
            perror("mprotect tiled");
            goto restore;
        }
        tiled_protected = 1;
    }
    if (!protected_access_faults(memory) ||
        (with_tiled && !protected_access_faults((void *)(uintptr_t)tiled)))
        goto restore;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:(void *)(uintptr_t)alias
                                                        length:total
                                                       options:MTLResourceStorageModeShared
                                                   deallocator:nil];
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              width:640 height:480 mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [buffer newTextureWithDescriptor:desc offset:0 bytesPerRow:2560];
        if (!device || !queue || !buffer || !texture) {
            fprintf(stderr, "Could not create Metal resources for protected alias\n");
        } else if (clear_texture(queue, texture, 1.0, 0.0) &&
                   *(uint32_t *)(uintptr_t)alias == 0xFFFF0000u) {
            ok = 1;
        } else {
            fprintf(stderr, "Metal clear did not reach protected backing\n");
        }
    }
restore:
    if (tiled_protected &&
        mprotect((void *)(uintptr_t)tiled, total, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect tiled restore");
        ok = 0;
    }
    if (mprotect(memory, total, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect restore");
        ok = 0;
    } else if (ok && *(uint32_t *)memory != 0xFFFF0000u) {
        fprintf(stderr, "GPU result was not visible through restored CPU mapping\n");
        ok = 0;
    } else if (ok && with_tiled) {
        if (*(uint32_t *)(uintptr_t)tiled != 0xFFFF0000u) {
            fprintf(stderr, "GPU result was not visible through restored tiled mapping\n");
            ok = 0;
        } else {
            *(uint32_t *)(uintptr_t)tiled = 0xFF123456u;
            if (*(uint32_t *)memory != 0xFF123456u ||
                *(uint32_t *)(uintptr_t)alias != 0xFF123456u) {
                fprintf(stderr, "Restored tiled CPU write was not visible through both aliases\n");
                ok = 0;
            }
        }
    }
done:
    if (tiled && !release_alias(tiled, total))
        ok = 0;
    if (!release_alias(alias, total))
        ok = 0;
    return ok;
}

static int exercise_unprotected_alias_counterexample(void *memory, size_t total)
{
    mach_vm_address_t gpu = 0, tiled = 0;
    int ok = 0;
    if (!remap_shared(memory, total, &gpu))
        return 0;
    if (!remap_shared(memory, total, &tiled))
        goto done;
    if (mprotect(memory, total, PROT_NONE) != 0) {
        perror("mprotect");
        goto done;
    }
    if (protected_access_faults(memory))
        ok = exercise_queued_write_counterexample((void *)(uintptr_t)gpu,
                                                   (void *)(uintptr_t)tiled, total);
    if (mprotect(memory, total, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect restore");
        ok = 0;
    } else if (ok && *(uint32_t *)memory != 0xFFFF0000u) {
        fprintf(stderr, "The original mapping did not share the overwritten byte\n");
        ok = 0;
    }
done:
    if (tiled && !release_alias(tiled, total))
        ok = 0;
    if (!release_alias(gpu, total))
        ok = 0;
    return ok;
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "reuse-counterexample") &&
                     strcmp(argv[1], "protected-alias") &&
                     strcmp(argv[1], "protected-all-aliases") &&
                     strcmp(argv[1], "unprotected-alias-counterexample"))) {
        fprintf(stderr, "Expected no arguments or a supported experiment name\n");
        return 1;
    }
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        fprintf(stderr, "Could not determine host page size\n");
        return 1;
    }
    size_t first_bytes = 2560u * 480u, second_bytes = 5120u * 480u;
    size_t second_offset = (first_bytes + (size_t)page - 1u) / (size_t)page * (size_t)page;
    size_t total = (second_offset + second_bytes + (size_t)page - 1u) / (size_t)page * (size_t)page;
    void *memory = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    int ok;
    if (argc == 1)
        ok = exercise_shared_backing(memory, total, second_offset);
    else if (!strcmp(argv[1], "protected-alias"))
        ok = exercise_protected_alias(memory, total, 0);
    else if (!strcmp(argv[1], "protected-all-aliases"))
        ok = exercise_protected_alias(memory, total, 1);
    else if (!strcmp(argv[1], "unprotected-alias-counterexample"))
        ok = exercise_unprotected_alias_counterexample(memory, total);
    else
        ok = exercise_queued_write_counterexample(memory, memory, total);
    if (munmap(memory, total)) {
        perror("munmap");
        return 1;
    }
    return ok ? 0 : 1;
}
