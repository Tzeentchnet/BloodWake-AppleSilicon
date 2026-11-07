#ifndef BW_METAL_PIPELINE_TRACE_H
#define BW_METAL_PIPELINE_TRACE_H

#include <stddef.h>
#include <stdint.h>

#define BW_METAL_PIPELINE_TRACE_SCHEMA "bloodwake.metal-pipeline-trace/v1"
#define BW_METAL_PIPELINE_TRACE_ENTRY_WORDS 57u
#define BW_METAL_PIPELINE_TRACE_ENTRY_BYTES \
    (BW_METAL_PIPELINE_TRACE_ENTRY_WORDS * sizeof(uint32_t))
#define BW_METAL_PIPELINE_TRACE_MAX_ENTRIES 128u
#define BW_METAL_PIPELINE_TRACE_MAX_PAYLOAD_BYTES \
    (BW_METAL_PIPELINE_TRACE_MAX_ENTRIES * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES)

typedef struct {
    uint32_t textured, color_mask, fixed_alpha_zero;
    uint32_t blend_enable, blend_src, blend_dst, blend_eq;
    uint32_t depth_attachment;
    uint32_t specialized;
    uint32_t texture[4], stages[4], dot[4];
    uint32_t combiner[8][4], final[2], alpha[2];
} BwMetalPipelineTraceKey;

typedef enum {
    BW_METAL_PIPELINE_TRACE_OK = 0,
    BW_METAL_PIPELINE_TRACE_ARGUMENT,
    BW_METAL_PIPELINE_TRACE_LIMIT,
    BW_METAL_PIPELINE_TRACE_SIZE,
    BW_METAL_PIPELINE_TRACE_ORDER
} BwMetalPipelineTraceStatus;

size_t bw_metal_pipeline_trace_payload_size(size_t entry_count);

BwMetalPipelineTraceStatus bw_metal_pipeline_trace_encode(
    const BwMetalPipelineTraceKey *keys,
    size_t entry_count,
    uint8_t *payload,
    size_t payload_size);

BwMetalPipelineTraceStatus bw_metal_pipeline_trace_decode(
    const uint8_t *payload,
    size_t payload_size,
    size_t entry_count,
    BwMetalPipelineTraceKey *keys,
    size_t key_capacity);

const char *bw_metal_pipeline_trace_status_string(
    BwMetalPipelineTraceStatus status);

#endif
