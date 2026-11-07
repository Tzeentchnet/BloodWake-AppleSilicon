#include "bw_metal_pipeline_trace.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static BwMetalPipelineTraceKey sample_key(uint32_t value)
{
    BwMetalPipelineTraceKey key;
    memset(&key, 0, sizeof key);
    key.textured = value % 5u;
    key.color_mask = 0x01010101u;
    key.depth_attachment = 1;
    key.specialized = key.textured == 2u;
    if (key.specialized) {
        key.texture[0] = value;
        key.stages[1] = value + 1u;
        key.dot[2] = value + 2u;
        key.combiner[3][2] = value + 3u;
        key.final[0] = value + 4u;
        key.alpha[1] = value + 5u;
    }
    return key;
}

static int deterministic_test(void)
{
    BwMetalPipelineTraceKey first[3] = {
        sample_key(12), sample_key(2), sample_key(7),
    };
    BwMetalPipelineTraceKey second[3] = {
        first[2], first[0], first[1],
    };
    uint8_t a[3 * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES];
    uint8_t b[sizeof a];
    CHECK(bw_metal_pipeline_trace_encode(
              first, 3, a, sizeof a) == BW_METAL_PIPELINE_TRACE_OK);
    CHECK(bw_metal_pipeline_trace_encode(
              second, 3, b, sizeof b) == BW_METAL_PIPELINE_TRACE_OK);
    CHECK(!memcmp(a, b, sizeof a));
    CHECK(!strcmp(BW_METAL_PIPELINE_TRACE_SCHEMA,
                  "bloodwake.metal-pipeline-trace/v1"));
    return 1;
}

static int roundtrip_test(void)
{
    BwMetalPipelineTraceKey input[3] = {
        sample_key(8), sample_key(2), sample_key(14),
    };
    BwMetalPipelineTraceKey output[3];
    uint8_t payload[3 * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES];
    uint8_t encoded[sizeof payload];
    CHECK(bw_metal_pipeline_trace_payload_size(3) == sizeof payload);
    CHECK(bw_metal_pipeline_trace_encode(
              input, 3, payload, sizeof payload) == BW_METAL_PIPELINE_TRACE_OK);
    CHECK(bw_metal_pipeline_trace_decode(
              payload, sizeof payload, 3, output, 3) ==
          BW_METAL_PIPELINE_TRACE_OK);
    CHECK(bw_metal_pipeline_trace_encode(
              output, 3, encoded, sizeof encoded) ==
          BW_METAL_PIPELINE_TRACE_OK);
    CHECK(!memcmp(payload, encoded, sizeof payload));
    return 1;
}

static int bounds_test(void)
{
    BwMetalPipelineTraceKey key = sample_key(1);
    uint8_t payload[BW_METAL_PIPELINE_TRACE_ENTRY_BYTES];
    CHECK(!bw_metal_pipeline_trace_payload_size(0));
    CHECK(!bw_metal_pipeline_trace_payload_size(
        BW_METAL_PIPELINE_TRACE_MAX_ENTRIES + 1u));
    CHECK(bw_metal_pipeline_trace_encode(
              NULL, 1, payload, sizeof payload) ==
          BW_METAL_PIPELINE_TRACE_ARGUMENT);
    CHECK(bw_metal_pipeline_trace_encode(
              &key, 0, payload, sizeof payload) ==
          BW_METAL_PIPELINE_TRACE_LIMIT);
    CHECK(bw_metal_pipeline_trace_encode(
              &key, 1, payload, sizeof payload - 1u) ==
          BW_METAL_PIPELINE_TRACE_SIZE);
    CHECK(bw_metal_pipeline_trace_decode(
              payload, sizeof payload, 1, &key, 0) ==
          BW_METAL_PIPELINE_TRACE_LIMIT);
    CHECK(!strcmp(bw_metal_pipeline_trace_status_string(
                      BW_METAL_PIPELINE_TRACE_LIMIT), "over_limit"));
    return 1;
}

static int format_test(void)
{
    BwMetalPipelineTraceKey keys[2] = {
        sample_key(5), sample_key(5),
    };
    BwMetalPipelineTraceKey decoded[2];
    uint8_t payload[2 * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES];
    CHECK(bw_metal_pipeline_trace_encode(
              keys, 2, payload, sizeof payload) ==
          BW_METAL_PIPELINE_TRACE_ORDER);
    keys[1] = sample_key(6);
    CHECK(bw_metal_pipeline_trace_encode(
              keys, 2, payload, sizeof payload) ==
          BW_METAL_PIPELINE_TRACE_OK);
    memcpy(payload + BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
           payload, BW_METAL_PIPELINE_TRACE_ENTRY_BYTES);
    CHECK(bw_metal_pipeline_trace_decode(
              payload, sizeof payload, 2, decoded, 2) ==
          BW_METAL_PIPELINE_TRACE_ORDER);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <deterministic|roundtrip|bounds|format>\n",
                argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "deterministic"))
        return deterministic_test() ? 0 : 1;
    if (!strcmp(argv[1], "roundtrip"))
        return roundtrip_test() ? 0 : 1;
    if (!strcmp(argv[1], "bounds"))
        return bounds_test() ? 0 : 1;
    if (!strcmp(argv[1], "format"))
        return format_test() ? 0 : 1;
    return 2;
}
