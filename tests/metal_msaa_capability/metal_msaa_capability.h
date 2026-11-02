#ifndef BW_METAL_MSAA_CAPABILITY_H
#define BW_METAL_MSAA_CAPABILITY_H

#include <stddef.h>

#define BW_METAL_MSAA_DEVICE_NAME_CAPACITY 256
#define BW_METAL_MSAA_ERROR_CAPACITY 512
#define BW_METAL_MSAA_SAMPLE_COUNT_TOTAL 3

typedef enum BwMetalMsaaQueryState {
    BW_METAL_MSAA_QUERY_ERROR = -1,
    BW_METAL_MSAA_UNSUPPORTED = 0,
    BW_METAL_MSAA_SUPPORTED = 1
} BwMetalMsaaQueryState;

typedef enum BwMetalMsaaSampleState {
    BW_METAL_MSAA_NOT_QUERIED,
    BW_METAL_MSAA_SAMPLE_UNSUPPORTED,
    BW_METAL_MSAA_SAMPLE_SUPPORTED,
    BW_METAL_MSAA_SAMPLE_ERROR
} BwMetalMsaaSampleState;

typedef enum BwMetalMsaaErrorStage {
    BW_METAL_MSAA_ERROR_NONE,
    BW_METAL_MSAA_ERROR_DEVICE,
    BW_METAL_MSAA_ERROR_QUERY
} BwMetalMsaaErrorStage;

typedef BwMetalMsaaQueryState (*BwMetalMsaaQueryFn)(
    void *context, unsigned sample_count, char *error, size_t error_capacity);

typedef struct BwMetalMsaaProbeSource {
    const char *device_name;
    const char *device_error;
    BwMetalMsaaQueryFn query;
    void *query_context;
} BwMetalMsaaProbeSource;

typedef struct BwMetalMsaaProbeResult {
    int succeeded;
    char device_name[BW_METAL_MSAA_DEVICE_NAME_CAPACITY];
    BwMetalMsaaSampleState sample_states[BW_METAL_MSAA_SAMPLE_COUNT_TOTAL];
    BwMetalMsaaErrorStage error_stage;
    unsigned error_sample_count;
    char error_message[BW_METAL_MSAA_ERROR_CAPACITY];
} BwMetalMsaaProbeResult;

int bw_metal_msaa_probe(const BwMetalMsaaProbeSource *source,
                        BwMetalMsaaProbeResult *result);
int bw_metal_msaa_format_json(const BwMetalMsaaProbeResult *result,
                              char *output, size_t output_capacity);

#endif

