#include "bw_metal_pipeline_trace.h"

#include <string.h>

_Static_assert(sizeof(BwMetalPipelineTraceKey) ==
                   BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
               "pipeline trace key layout changed");

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void encode_key(const BwMetalPipelineTraceKey *key, uint8_t *record)
{
    size_t word = 0;
#define PUT_WORD(value) do { put32(record + word++ * 4u, (value)); } while (0)
    PUT_WORD(key->textured);
    PUT_WORD(key->color_mask);
    PUT_WORD(key->fixed_alpha_zero);
    PUT_WORD(key->blend_enable);
    PUT_WORD(key->blend_src);
    PUT_WORD(key->blend_dst);
    PUT_WORD(key->blend_eq);
    PUT_WORD(key->depth_attachment);
    PUT_WORD(key->specialized);
    for (size_t i = 0; i < 4; i++)
        PUT_WORD(key->texture[i]);
    for (size_t i = 0; i < 4; i++)
        PUT_WORD(key->stages[i]);
    for (size_t i = 0; i < 4; i++)
        PUT_WORD(key->dot[i]);
    for (size_t stage = 0; stage < 8; stage++)
        for (size_t component = 0; component < 4; component++)
            PUT_WORD(key->combiner[stage][component]);
    for (size_t i = 0; i < 2; i++)
        PUT_WORD(key->final[i]);
    for (size_t i = 0; i < 2; i++)
        PUT_WORD(key->alpha[i]);
#undef PUT_WORD
}

static void decode_key(const uint8_t *record, BwMetalPipelineTraceKey *key)
{
    size_t word = 0;
#define GET_WORD(target) do { (target) = get32(record + word++ * 4u); } while (0)
    GET_WORD(key->textured);
    GET_WORD(key->color_mask);
    GET_WORD(key->fixed_alpha_zero);
    GET_WORD(key->blend_enable);
    GET_WORD(key->blend_src);
    GET_WORD(key->blend_dst);
    GET_WORD(key->blend_eq);
    GET_WORD(key->depth_attachment);
    GET_WORD(key->specialized);
    for (size_t i = 0; i < 4; i++)
        GET_WORD(key->texture[i]);
    for (size_t i = 0; i < 4; i++)
        GET_WORD(key->stages[i]);
    for (size_t i = 0; i < 4; i++)
        GET_WORD(key->dot[i]);
    for (size_t stage = 0; stage < 8; stage++)
        for (size_t component = 0; component < 4; component++)
            GET_WORD(key->combiner[stage][component]);
    for (size_t i = 0; i < 2; i++)
        GET_WORD(key->final[i]);
    for (size_t i = 0; i < 2; i++)
        GET_WORD(key->alpha[i]);
#undef GET_WORD
}

size_t bw_metal_pipeline_trace_payload_size(size_t entry_count)
{
    if (!entry_count ||
        entry_count > BW_METAL_PIPELINE_TRACE_MAX_ENTRIES)
        return 0;
    return entry_count * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES;
}

BwMetalPipelineTraceStatus bw_metal_pipeline_trace_encode(
    const BwMetalPipelineTraceKey *keys,
    size_t entry_count,
    uint8_t *payload,
    size_t payload_size)
{
    size_t expected = bw_metal_pipeline_trace_payload_size(entry_count);
    uint8_t temporary[BW_METAL_PIPELINE_TRACE_ENTRY_BYTES];
    if (!keys || !payload)
        return BW_METAL_PIPELINE_TRACE_ARGUMENT;
    if (!expected)
        return BW_METAL_PIPELINE_TRACE_LIMIT;
    if (payload_size != expected)
        return BW_METAL_PIPELINE_TRACE_SIZE;
    for (size_t i = 0; i < entry_count; i++)
        encode_key(&keys[i], payload + i * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES);
    for (size_t i = 1; i < entry_count; i++) {
        size_t position = i;
        memcpy(temporary,
               payload + i * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
               sizeof temporary);
        while (position &&
               memcmp(payload + (position - 1u) *
                                    BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
                      temporary, sizeof temporary) > 0) {
            memcpy(payload + position * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
                   payload + (position - 1u) *
                                 BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
                   BW_METAL_PIPELINE_TRACE_ENTRY_BYTES);
            position--;
        }
        memcpy(payload + position * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
               temporary, sizeof temporary);
    }
    for (size_t i = 1; i < entry_count; i++)
        if (!memcmp(payload + (i - 1u) * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
                    payload + i * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
                    BW_METAL_PIPELINE_TRACE_ENTRY_BYTES))
            return BW_METAL_PIPELINE_TRACE_ORDER;
    return BW_METAL_PIPELINE_TRACE_OK;
}

BwMetalPipelineTraceStatus bw_metal_pipeline_trace_decode(
    const uint8_t *payload,
    size_t payload_size,
    size_t entry_count,
    BwMetalPipelineTraceKey *keys,
    size_t key_capacity)
{
    size_t expected = bw_metal_pipeline_trace_payload_size(entry_count);
    if (!payload || !keys)
        return BW_METAL_PIPELINE_TRACE_ARGUMENT;
    if (!expected || entry_count > key_capacity)
        return BW_METAL_PIPELINE_TRACE_LIMIT;
    if (payload_size != expected)
        return BW_METAL_PIPELINE_TRACE_SIZE;
    for (size_t i = 1; i < entry_count; i++)
        if (memcmp(payload + (i - 1u) * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
                   payload + i * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
                   BW_METAL_PIPELINE_TRACE_ENTRY_BYTES) >= 0)
            return BW_METAL_PIPELINE_TRACE_ORDER;
    memset(keys, 0, entry_count * sizeof *keys);
    for (size_t i = 0; i < entry_count; i++)
        decode_key(payload + i * BW_METAL_PIPELINE_TRACE_ENTRY_BYTES,
                   &keys[i]);
    return BW_METAL_PIPELINE_TRACE_OK;
}

const char *bw_metal_pipeline_trace_status_string(
    BwMetalPipelineTraceStatus status)
{
    switch (status) {
    case BW_METAL_PIPELINE_TRACE_OK: return "compatible";
    case BW_METAL_PIPELINE_TRACE_ARGUMENT: return "invalid_argument";
    case BW_METAL_PIPELINE_TRACE_LIMIT: return "over_limit";
    case BW_METAL_PIPELINE_TRACE_SIZE: return "invalid_size";
    case BW_METAL_PIPELINE_TRACE_ORDER: return "invalid_order";
    default: return "unknown_status";
    }
}
