#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdio.h>

#include "metal_msaa_capability.h"

static void describe_exception(NSException *exception,
                               char *error, size_t error_capacity)
{
    NSString *description =
        [NSString stringWithFormat:@"%@: %@", exception.name,
         exception.reason ?: @"no reason provided"];
    const char *utf8 = description.UTF8String;
    snprintf(error, error_capacity, "%s",
             utf8 ? utf8 : "Metal API raised an exception");
}

static BwMetalMsaaQueryState query_sample_count(
    void *context, unsigned sample_count, char *error, size_t error_capacity)
{
    id<MTLDevice> device = (__bridge id<MTLDevice>)context;

    @try {
        return [device supportsTextureSampleCount:sample_count]
            ? BW_METAL_MSAA_SUPPORTED
            : BW_METAL_MSAA_UNSUPPORTED;
    } @catch (NSException *exception) {
        describe_exception(exception, error, error_capacity);
        return BW_METAL_MSAA_QUERY_ERROR;
    }
}

int main(void)
{
    BwMetalMsaaProbeResult result;
    char output[8192];
    int probe_status;

    @autoreleasepool {
        BwMetalMsaaProbeSource source = { 0 };
        char device_error[BW_METAL_MSAA_ERROR_CAPACITY] = { 0 };

        @try {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) {
                source.device_error =
                    "MTLCreateSystemDefaultDevice returned no Metal device";
                probe_status = bw_metal_msaa_probe(&source, &result);
            } else {
                const char *device_name = device.name.UTF8String;
                if (!device_name) {
                    source.device_error =
                        "MTLDevice name could not be converted to UTF-8";
                } else {
                    source.device_name = device_name;
                    source.query = query_sample_count;
                    source.query_context = (__bridge void *)device;
                }
                probe_status = bw_metal_msaa_probe(&source, &result);
            }
        } @catch (NSException *exception) {
            describe_exception(exception, device_error, sizeof device_error);
            source.device_error = device_error;
            probe_status = bw_metal_msaa_probe(&source, &result);
        }

        if (bw_metal_msaa_format_json(&result, output, sizeof output)) {
            fprintf(stderr, "Metal MSAA capability output formatting failed\n");
            return 2;
        }
        puts(output);
    }
    return probe_status ? 1 : 0;
}

