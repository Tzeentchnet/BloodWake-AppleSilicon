#include "metal_msaa_capability.h"

#include <stdio.h>
#include <string.h>

static const unsigned k_sample_counts[BW_METAL_MSAA_SAMPLE_COUNT_TOTAL] = { 2, 4, 8 };

typedef struct JsonWriter {
    char *output;
    size_t capacity;
    size_t length;
    int failed;
} JsonWriter;

static void copy_string(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void set_error(BwMetalMsaaProbeResult *result,
                      BwMetalMsaaErrorStage stage,
                      unsigned sample_count,
                      const char *message)
{
    result->succeeded = 0;
    result->error_stage = stage;
    result->error_sample_count = sample_count;
    copy_string(result->error_message, sizeof result->error_message, message);
}

int bw_metal_msaa_probe(const BwMetalMsaaProbeSource *source,
                        BwMetalMsaaProbeResult *result)
{
    size_t i;

    if (!result)
        return -1;
    memset(result, 0, sizeof *result);
    if (!source) {
        set_error(result, BW_METAL_MSAA_ERROR_DEVICE, 0,
                  "Metal probe source is unavailable");
        return -1;
    }
    if (source->device_error) {
        set_error(result, BW_METAL_MSAA_ERROR_DEVICE, 0,
                  source->device_error[0] ? source->device_error :
                  "Metal device query failed");
        return -1;
    }
    if (!source->device_name || !source->device_name[0]) {
        set_error(result, BW_METAL_MSAA_ERROR_DEVICE, 0,
                  "Metal device name is unavailable");
        return -1;
    }
    if (strlen(source->device_name) >= sizeof result->device_name) {
        set_error(result, BW_METAL_MSAA_ERROR_DEVICE, 0,
                  "Metal device name exceeds probe limit");
        return -1;
    }
    copy_string(result->device_name, sizeof result->device_name,
                source->device_name);

    for (i = 0; i < BW_METAL_MSAA_SAMPLE_COUNT_TOTAL; i++) {
        BwMetalMsaaQueryState state;
        char query_error[BW_METAL_MSAA_ERROR_CAPACITY] = { 0 };

        if (!source->query) {
            result->sample_states[i] = BW_METAL_MSAA_SAMPLE_ERROR;
            set_error(result, BW_METAL_MSAA_ERROR_QUERY, k_sample_counts[i],
                      "Metal sample-count query is unavailable");
            return -1;
        }
        state = source->query(source->query_context, k_sample_counts[i],
                              query_error, sizeof query_error);
        query_error[sizeof query_error - 1] = '\0';
        if (state == BW_METAL_MSAA_SUPPORTED) {
            result->sample_states[i] = BW_METAL_MSAA_SAMPLE_SUPPORTED;
        } else if (state == BW_METAL_MSAA_UNSUPPORTED) {
            result->sample_states[i] = BW_METAL_MSAA_SAMPLE_UNSUPPORTED;
        } else {
            result->sample_states[i] = BW_METAL_MSAA_SAMPLE_ERROR;
            set_error(result, BW_METAL_MSAA_ERROR_QUERY, k_sample_counts[i],
                      query_error[0] ? query_error :
                      "Metal sample-count query failed");
            return -1;
        }
    }

    result->succeeded = 1;
    return 0;
}

static void json_append_char(JsonWriter *writer, char value)
{
    if (writer->failed)
        return;
    if (writer->length + 1 >= writer->capacity) {
        writer->failed = 1;
        return;
    }
    writer->output[writer->length++] = value;
    writer->output[writer->length] = '\0';
}

static void json_append(JsonWriter *writer, const char *value)
{
    while (*value)
        json_append_char(writer, *value++);
}

static void json_append_string(JsonWriter *writer, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    json_append_char(writer, '"');
    while (*cursor) {
        char escaped[7];
        switch (*cursor) {
        case '"':
            json_append(writer, "\\\"");
            break;
        case '\\':
            json_append(writer, "\\\\");
            break;
        case '\b':
            json_append(writer, "\\b");
            break;
        case '\f':
            json_append(writer, "\\f");
            break;
        case '\n':
            json_append(writer, "\\n");
            break;
        case '\r':
            json_append(writer, "\\r");
            break;
        case '\t':
            json_append(writer, "\\t");
            break;
        default:
            if (*cursor < 0x20) {
                snprintf(escaped, sizeof escaped, "\\u%04x",
                         (unsigned)*cursor);
                json_append(writer, escaped);
            } else {
                json_append_char(writer, (char)*cursor);
            }
            break;
        }
        cursor++;
    }
    json_append_char(writer, '"');
}

static const char *sample_state_name(BwMetalMsaaSampleState state)
{
    switch (state) {
    case BW_METAL_MSAA_NOT_QUERIED:
        return "not_queried";
    case BW_METAL_MSAA_SAMPLE_UNSUPPORTED:
        return "unsupported";
    case BW_METAL_MSAA_SAMPLE_SUPPORTED:
        return "supported";
    case BW_METAL_MSAA_SAMPLE_ERROR:
        return "error";
    }
    return NULL;
}

static const char *error_stage_name(BwMetalMsaaErrorStage stage)
{
    switch (stage) {
    case BW_METAL_MSAA_ERROR_NONE:
        return "none";
    case BW_METAL_MSAA_ERROR_DEVICE:
        return "device";
    case BW_METAL_MSAA_ERROR_QUERY:
        return "query";
    }
    return NULL;
}

int bw_metal_msaa_format_json(const BwMetalMsaaProbeResult *result,
                              char *output, size_t output_capacity)
{
    JsonWriter writer;
    const char *states[BW_METAL_MSAA_SAMPLE_COUNT_TOTAL];
    const char *stage;
    size_t i;

    if (!result || !output || !output_capacity)
        return -1;
    writer.output = output;
    writer.capacity = output_capacity;
    writer.length = 0;
    writer.failed = 0;
    output[0] = '\0';

    for (i = 0; i < BW_METAL_MSAA_SAMPLE_COUNT_TOTAL; i++) {
        states[i] = sample_state_name(result->sample_states[i]);
        if (!states[i])
            return -1;
    }
    stage = error_stage_name(result->error_stage);
    if (!stage)
        return -1;

    json_append(&writer, "{\"schema_version\":1,\"status\":");
    json_append_string(&writer, result->succeeded ? "ok" : "error");
    json_append(&writer, ",\"device_name\":");
    if (result->device_name[0])
        json_append_string(&writer, result->device_name);
    else
        json_append(&writer, "null");
    json_append(&writer, ",\"sample_counts\":{\"2\":");
    json_append_string(&writer, states[0]);
    json_append(&writer, ",\"4\":");
    json_append_string(&writer, states[1]);
    json_append(&writer, ",\"8\":");
    json_append_string(&writer, states[2]);
    json_append(&writer,
                "},\"bw_vfx_msaa_policy\":\"unsupported\",\"error\":{\"stage\":");
    json_append_string(&writer, stage);
    json_append(&writer, ",\"sample_count\":");
    if (result->error_sample_count) {
        char sample_count[16];
        snprintf(sample_count, sizeof sample_count, "%u",
                 result->error_sample_count);
        json_append(&writer, sample_count);
    } else {
        json_append(&writer, "null");
    }
    json_append(&writer, ",\"message\":");
    if (result->error_message[0])
        json_append_string(&writer, result->error_message);
    else
        json_append(&writer, "null");
    json_append(&writer, "}}");

    return writer.failed ? -1 : 0;
}
