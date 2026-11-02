#ifndef BW_METAL_MSAA_SMOKE_H
#define BW_METAL_MSAA_SMOKE_H

#include <stddef.h>
#include <stdint.h>

#define BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL 3
#define BW_METAL_MSAA_SMOKE_WIDTH 8
#define BW_METAL_MSAA_SMOKE_HEIGHT 8
#define BW_METAL_MSAA_SMOKE_READBACK_BYTES_PER_ROW 256
#define BW_METAL_MSAA_SMOKE_DEVICE_NAME_CAPACITY 256
#define BW_METAL_MSAA_SMOKE_ERROR_CAPACITY 512

typedef enum BwMetalMsaaSmokeStatus {
    BW_METAL_MSAA_SMOKE_NOT_RUN,
    BW_METAL_MSAA_SMOKE_SKIPPED_UNSUPPORTED,
    BW_METAL_MSAA_SMOKE_PASSED,
    BW_METAL_MSAA_SMOKE_FAILED
} BwMetalMsaaSmokeStatus;

typedef enum BwMetalMsaaSmokeCheck {
    BW_METAL_MSAA_SMOKE_CHECK_UNAVAILABLE = -1,
    BW_METAL_MSAA_SMOKE_CHECK_FAILED = 0,
    BW_METAL_MSAA_SMOKE_CHECK_PASSED = 1
} BwMetalMsaaSmokeCheck;

typedef enum BwMetalMsaaSmokeErrorStage {
    BW_METAL_MSAA_SMOKE_ERROR_NONE,
    BW_METAL_MSAA_SMOKE_ERROR_DEVICE,
    BW_METAL_MSAA_SMOKE_ERROR_CAPABILITY_QUERY,
    BW_METAL_MSAA_SMOKE_ERROR_SHADER,
    BW_METAL_MSAA_SMOKE_ERROR_COMMAND_QUEUE,
    BW_METAL_MSAA_SMOKE_ERROR_PIPELINE,
    BW_METAL_MSAA_SMOKE_ERROR_DEPTH_STENCIL_STATE,
    BW_METAL_MSAA_SMOKE_ERROR_RESOURCE_ALLOCATION,
    BW_METAL_MSAA_SMOKE_ERROR_ENCODER,
    BW_METAL_MSAA_SMOKE_ERROR_COMMAND_BUFFER,
    BW_METAL_MSAA_SMOKE_ERROR_READBACK,
    BW_METAL_MSAA_SMOKE_ERROR_CORRECTNESS,
    BW_METAL_MSAA_SMOKE_ERROR_EXCEPTION
} BwMetalMsaaSmokeErrorStage;

typedef struct BwMetalMsaaSmokeCorrectness {
    BwMetalMsaaSmokeCheck clear_color;
    BwMetalMsaaSmokeCheck draw_color;
    BwMetalMsaaSmokeCheck depth_clear_reject;
    BwMetalMsaaSmokeCheck depth_write_reject;
    BwMetalMsaaSmokeCheck stencil_clear_and_reject;
    BwMetalMsaaSmokeCheck resolve_readback;
} BwMetalMsaaSmokeCorrectness;

typedef struct BwMetalMsaaSmokeError {
    BwMetalMsaaSmokeErrorStage stage;
    int code_available;
    int64_t code;
    char message[BW_METAL_MSAA_SMOKE_ERROR_CAPACITY];
} BwMetalMsaaSmokeError;

typedef struct BwMetalMsaaSmokeSampleResult {
    unsigned sample_count;
    int supported;
    BwMetalMsaaSmokeStatus status;
    BwMetalMsaaSmokeCorrectness correctness;
    int resources_allocated;
    uint64_t allocated_bytes_lower_bound;
    int gpu_timing_available;
    double gpu_time_milliseconds;
    BwMetalMsaaSmokeError error;
} BwMetalMsaaSmokeSampleResult;

typedef struct BwMetalMsaaSmokeReport {
    char device_name[BW_METAL_MSAA_SMOKE_DEVICE_NAME_CAPACITY];
    BwMetalMsaaSmokeSampleResult
        results[BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL];
    BwMetalMsaaSmokeError fatal_error;
} BwMetalMsaaSmokeReport;

void bw_metal_msaa_smoke_report_init(BwMetalMsaaSmokeReport *report);
uint64_t bw_metal_msaa_smoke_allocated_bytes_lower_bound(
    unsigned sample_count);
void bw_metal_msaa_smoke_check_pixels(
    BwMetalMsaaSmokeSampleResult *result,
    const uint8_t *pixels, size_t bytes_per_row);
int bw_metal_msaa_smoke_checks_pass(
    const BwMetalMsaaSmokeCorrectness *correctness);
void bw_metal_msaa_smoke_set_sample_error(
    BwMetalMsaaSmokeSampleResult *result,
    BwMetalMsaaSmokeErrorStage stage,
    int code_available, int64_t code, const char *message);
void bw_metal_msaa_smoke_set_fatal_error(
    BwMetalMsaaSmokeReport *report,
    BwMetalMsaaSmokeErrorStage stage,
    int code_available, int64_t code, const char *message);
int bw_metal_msaa_smoke_report_succeeded(
    const BwMetalMsaaSmokeReport *report);
int bw_metal_msaa_smoke_format_json(
    const BwMetalMsaaSmokeReport *report,
    char *output, size_t output_capacity);

#endif
