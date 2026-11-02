#include "metal_msaa_smoke.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define COLOR_BYTES_PER_PIXEL 4u
#define DEPTH_STENCIL_BYTES_PER_PIXEL 8u

typedef struct JsonWriter {
    char *output;
    size_t capacity;
    size_t length;
    int failed;
} JsonWriter;

static const unsigned k_sample_counts[BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL] = {
    2, 4, 8
};

static void copy_string(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if (!capacity)
        return;
    if (!source)
        source = "";
    length = strlen(source);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void checks_set(BwMetalMsaaSmokeCorrectness *correctness,
                       BwMetalMsaaSmokeCheck value)
{
    correctness->clear_color = value;
    correctness->draw_color = value;
    correctness->depth_clear_reject = value;
    correctness->depth_write_reject = value;
    correctness->stencil_clear_and_reject = value;
    correctness->resolve_readback = value;
}

void bw_metal_msaa_smoke_report_init(BwMetalMsaaSmokeReport *report)
{
    size_t i;

    if (!report)
        return;
    memset(report, 0, sizeof *report);
    for (i = 0; i < BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL; i++) {
        report->results[i].sample_count = k_sample_counts[i];
        checks_set(&report->results[i].correctness,
                   BW_METAL_MSAA_SMOKE_CHECK_UNAVAILABLE);
    }
}

uint64_t bw_metal_msaa_smoke_allocated_bytes_lower_bound(
    unsigned sample_count)
{
    uint64_t pixels;
    uint64_t multisample_attachments;
    uint64_t resolve_texture;
    uint64_t readback_buffer;

    if (sample_count != 2 && sample_count != 4 && sample_count != 8)
        return 0;
    pixels = (uint64_t)BW_METAL_MSAA_SMOKE_WIDTH *
             BW_METAL_MSAA_SMOKE_HEIGHT;
    multisample_attachments =
        pixels * sample_count *
        (COLOR_BYTES_PER_PIXEL + DEPTH_STENCIL_BYTES_PER_PIXEL);
    resolve_texture = pixels * COLOR_BYTES_PER_PIXEL;
    readback_buffer =
        (uint64_t)BW_METAL_MSAA_SMOKE_READBACK_BYTES_PER_ROW *
        BW_METAL_MSAA_SMOKE_HEIGHT;
    return multisample_attachments + resolve_texture + readback_buffer;
}

static int region_is_color(const uint8_t *pixels, size_t bytes_per_row,
                           unsigned x, unsigned y,
                           unsigned width, unsigned height,
                           uint8_t red, uint8_t green,
                           uint8_t blue, uint8_t alpha)
{
    unsigned row;

    for (row = y; row < y + height; row++) {
        unsigned column;
        const uint8_t *pixel = pixels + row * bytes_per_row +
                               (size_t)x * COLOR_BYTES_PER_PIXEL;
        for (column = 0; column < width; column++) {
            if (pixel[0] != red || pixel[1] != green ||
                pixel[2] != blue || pixel[3] != alpha)
                return 0;
            pixel += COLOR_BYTES_PER_PIXEL;
        }
    }
    return 1;
}

void bw_metal_msaa_smoke_check_pixels(
    BwMetalMsaaSmokeSampleResult *result,
    const uint8_t *pixels, size_t bytes_per_row)
{
    BwMetalMsaaSmokeCorrectness *checks;
    int top_left_blue;
    int top_right_blue;
    int bottom_draw_red;
    int bottom_depth_red;
    int bottom_right_blue;

    if (!result)
        return;
    checks = &result->correctness;
    if (!pixels ||
        bytes_per_row < BW_METAL_MSAA_SMOKE_WIDTH * COLOR_BYTES_PER_PIXEL) {
        checks_set(checks, BW_METAL_MSAA_SMOKE_CHECK_FAILED);
        return;
    }

    top_left_blue = region_is_color(
        pixels, bytes_per_row, 0, 0, 4, 4, 0, 0, 255, 255);
    top_right_blue = region_is_color(
        pixels, bytes_per_row, 4, 0, 4, 4, 0, 0, 255, 255);
    bottom_draw_red = region_is_color(
        pixels, bytes_per_row, 0, 4, 2, 4, 255, 0, 0, 255);
    bottom_depth_red = region_is_color(
        pixels, bytes_per_row, 2, 4, 2, 4, 255, 0, 0, 255);
    bottom_right_blue = region_is_color(
        pixels, bytes_per_row, 4, 4, 4, 4, 0, 0, 255, 255);

    checks->clear_color = top_right_blue;
    checks->draw_color = bottom_draw_red;
    checks->depth_clear_reject = top_left_blue;
    checks->depth_write_reject = bottom_depth_red;
    checks->stencil_clear_and_reject =
        bottom_draw_red && bottom_right_blue;
    checks->resolve_readback =
        top_left_blue && top_right_blue &&
        bottom_draw_red && bottom_depth_red && bottom_right_blue;
}

int bw_metal_msaa_smoke_checks_pass(
    const BwMetalMsaaSmokeCorrectness *correctness)
{
    return correctness &&
        correctness->clear_color == BW_METAL_MSAA_SMOKE_CHECK_PASSED &&
        correctness->draw_color == BW_METAL_MSAA_SMOKE_CHECK_PASSED &&
        correctness->depth_clear_reject ==
            BW_METAL_MSAA_SMOKE_CHECK_PASSED &&
        correctness->depth_write_reject ==
            BW_METAL_MSAA_SMOKE_CHECK_PASSED &&
        correctness->stencil_clear_and_reject ==
            BW_METAL_MSAA_SMOKE_CHECK_PASSED &&
        correctness->resolve_readback ==
            BW_METAL_MSAA_SMOKE_CHECK_PASSED;
}

static void set_error(BwMetalMsaaSmokeError *error,
                      BwMetalMsaaSmokeErrorStage stage,
                      int code_available, int64_t code,
                      const char *message)
{
    memset(error, 0, sizeof *error);
    error->stage = stage;
    error->code_available = code_available != 0;
    error->code = code;
    copy_string(error->message, sizeof error->message, message);
}

void bw_metal_msaa_smoke_set_sample_error(
    BwMetalMsaaSmokeSampleResult *result,
    BwMetalMsaaSmokeErrorStage stage,
    int code_available, int64_t code, const char *message)
{
    if (!result)
        return;
    result->status = BW_METAL_MSAA_SMOKE_FAILED;
    set_error(&result->error, stage, code_available, code, message);
}

void bw_metal_msaa_smoke_set_fatal_error(
    BwMetalMsaaSmokeReport *report,
    BwMetalMsaaSmokeErrorStage stage,
    int code_available, int64_t code, const char *message)
{
    if (!report)
        return;
    set_error(&report->fatal_error, stage, code_available, code, message);
}

int bw_metal_msaa_smoke_report_succeeded(
    const BwMetalMsaaSmokeReport *report)
{
    size_t i;

    if (!report || report->fatal_error.stage != BW_METAL_MSAA_SMOKE_ERROR_NONE)
        return 0;
    for (i = 0; i < BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL; i++) {
        BwMetalMsaaSmokeStatus status = report->results[i].status;
        if (status != BW_METAL_MSAA_SMOKE_PASSED &&
            status != BW_METAL_MSAA_SMOKE_SKIPPED_UNSUPPORTED)
            return 0;
    }
    return 1;
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

static void json_append_u64(JsonWriter *writer, uint64_t value)
{
    char number[32];
    snprintf(number, sizeof number, "%" PRIu64, value);
    json_append(writer, number);
}

static void json_append_i64(JsonWriter *writer, int64_t value)
{
    char number[32];
    snprintf(number, sizeof number, "%" PRId64, value);
    json_append(writer, number);
}

static void json_append_check(JsonWriter *writer, BwMetalMsaaSmokeCheck check)
{
    switch (check) {
    case BW_METAL_MSAA_SMOKE_CHECK_UNAVAILABLE:
        json_append(writer, "null");
        break;
    case BW_METAL_MSAA_SMOKE_CHECK_FAILED:
        json_append(writer, "false");
        break;
    case BW_METAL_MSAA_SMOKE_CHECK_PASSED:
        json_append(writer, "true");
        break;
    default:
        writer->failed = 1;
        break;
    }
}

static const char *status_name(BwMetalMsaaSmokeStatus status)
{
    switch (status) {
    case BW_METAL_MSAA_SMOKE_NOT_RUN:
        return "not_run";
    case BW_METAL_MSAA_SMOKE_SKIPPED_UNSUPPORTED:
        return "skipped_unsupported";
    case BW_METAL_MSAA_SMOKE_PASSED:
        return "passed";
    case BW_METAL_MSAA_SMOKE_FAILED:
        return "failed";
    }
    return NULL;
}

static const char *error_stage_name(BwMetalMsaaSmokeErrorStage stage)
{
    switch (stage) {
    case BW_METAL_MSAA_SMOKE_ERROR_NONE:
        return "none";
    case BW_METAL_MSAA_SMOKE_ERROR_DEVICE:
        return "device";
    case BW_METAL_MSAA_SMOKE_ERROR_CAPABILITY_QUERY:
        return "capability_query";
    case BW_METAL_MSAA_SMOKE_ERROR_SHADER:
        return "shader";
    case BW_METAL_MSAA_SMOKE_ERROR_COMMAND_QUEUE:
        return "command_queue";
    case BW_METAL_MSAA_SMOKE_ERROR_PIPELINE:
        return "pipeline";
    case BW_METAL_MSAA_SMOKE_ERROR_DEPTH_STENCIL_STATE:
        return "depth_stencil_state";
    case BW_METAL_MSAA_SMOKE_ERROR_RESOURCE_ALLOCATION:
        return "resource_allocation";
    case BW_METAL_MSAA_SMOKE_ERROR_ENCODER:
        return "encoder";
    case BW_METAL_MSAA_SMOKE_ERROR_COMMAND_BUFFER:
        return "command_buffer";
    case BW_METAL_MSAA_SMOKE_ERROR_READBACK:
        return "readback";
    case BW_METAL_MSAA_SMOKE_ERROR_CORRECTNESS:
        return "correctness";
    case BW_METAL_MSAA_SMOKE_ERROR_EXCEPTION:
        return "exception";
    }
    return NULL;
}

static void json_append_error(JsonWriter *writer,
                              const BwMetalMsaaSmokeError *error)
{
    const char *stage = error_stage_name(error->stage);
    if (!stage) {
        writer->failed = 1;
        return;
    }
    json_append(writer, "{\"stage\":");
    json_append_string(writer, stage);
    json_append(writer, ",\"code\":");
    if (error->code_available)
        json_append_i64(writer, error->code);
    else
        json_append(writer, "null");
    json_append(writer, ",\"message\":");
    if (error->message[0])
        json_append_string(writer, error->message);
    else
        json_append(writer, "null");
    json_append_char(writer, '}');
}

int bw_metal_msaa_smoke_format_json(
    const BwMetalMsaaSmokeReport *report,
    char *output, size_t output_capacity)
{
    JsonWriter writer;
    unsigned passed = 0;
    unsigned skipped = 0;
    unsigned failed = 0;
    unsigned not_run = 0;
    unsigned supported = 0;
    size_t i;

    if (!report || !output || !output_capacity)
        return -1;
    writer.output = output;
    writer.capacity = output_capacity;
    writer.length = 0;
    writer.failed = 0;
    output[0] = '\0';

    for (i = 0; i < BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL; i++) {
        switch (report->results[i].status) {
        case BW_METAL_MSAA_SMOKE_NOT_RUN:
            not_run++;
            break;
        case BW_METAL_MSAA_SMOKE_SKIPPED_UNSUPPORTED:
            skipped++;
            break;
        case BW_METAL_MSAA_SMOKE_PASSED:
            passed++;
            break;
        case BW_METAL_MSAA_SMOKE_FAILED:
            failed++;
            break;
        default:
            return -1;
        }
        if (report->results[i].supported)
            supported++;
    }

    json_append(&writer,
        "{\"schema\":\"bloodwake.metal-msaa-smoke/v1\",\"status\":");
    json_append_string(&writer,
        bw_metal_msaa_smoke_report_succeeded(report) ? "passed" : "failed");
    json_append(&writer,
        ",\"diagnostic_only\":true,"
        "\"retail_integration\":\"not_implemented\",\"device_name\":");
    if (report->device_name[0])
        json_append_string(&writer, report->device_name);
    else
        json_append(&writer, "null");
    json_append(&writer,
        ",\"resources\":{\"width\":8,\"height\":8,"
        "\"color_format\":\"rgba8unorm\","
        "\"depth_stencil_format\":\"depth32float_stencil8\","
        "\"readback_bytes_per_row\":256},\"results\":[");

    for (i = 0; i < BW_METAL_MSAA_SMOKE_SAMPLE_COUNT_TOTAL; i++) {
        const BwMetalMsaaSmokeSampleResult *result = &report->results[i];
        const char *status = status_name(result->status);
        char timing[64];

        if (!status)
            return -1;
        if (i)
            json_append_char(&writer, ',');
        json_append(&writer, "{\"sample_count\":");
        json_append_u64(&writer, result->sample_count);
        json_append(&writer, ",\"status\":");
        json_append_string(&writer, status);
        json_append(&writer, ",\"supported\":");
        json_append(&writer, result->supported ? "true" : "false");
        json_append(&writer, ",\"correctness\":{\"clear_color\":");
        json_append_check(&writer, result->correctness.clear_color);
        json_append(&writer, ",\"draw_color\":");
        json_append_check(&writer, result->correctness.draw_color);
        json_append(&writer, ",\"depth_clear_reject\":");
        json_append_check(&writer, result->correctness.depth_clear_reject);
        json_append(&writer, ",\"depth_write_reject\":");
        json_append_check(&writer, result->correctness.depth_write_reject);
        json_append(&writer, ",\"stencil_clear_and_reject\":");
        json_append_check(&writer,
                          result->correctness.stencil_clear_and_reject);
        json_append(&writer, ",\"resolve_readback\":");
        json_append_check(&writer, result->correctness.resolve_readback);
        json_append(&writer, "},\"memory\":{\"resources_allocated\":");
        json_append(&writer, result->resources_allocated ? "true" : "false");
        json_append(&writer, ",\"allocated_bytes_lower_bound\":");
        json_append_u64(&writer, result->allocated_bytes_lower_bound);
        json_append(&writer, "},\"gpu_timing\":{\"available\":");
        json_append(&writer, result->gpu_timing_available ? "true" : "false");
        json_append(&writer, ",\"milliseconds\":");
        if (result->gpu_timing_available) {
            if (!isfinite(result->gpu_time_milliseconds) ||
                result->gpu_time_milliseconds < 0.0)
                return -1;
            snprintf(timing, sizeof timing, "%.6f",
                     result->gpu_time_milliseconds);
            json_append(&writer, timing);
        } else {
            json_append(&writer, "null");
        }
        json_append(&writer, "},\"error\":");
        json_append_error(&writer, &result->error);
        json_append_char(&writer, '}');
    }

    json_append(&writer, "],\"summary\":{\"supported\":");
    json_append_u64(&writer, supported);
    json_append(&writer, ",\"passed\":");
    json_append_u64(&writer, passed);
    json_append(&writer, ",\"skipped_unsupported\":");
    json_append_u64(&writer, skipped);
    json_append(&writer, ",\"failed\":");
    json_append_u64(&writer, failed);
    json_append(&writer, ",\"not_run\":");
    json_append_u64(&writer, not_run);
    json_append(&writer, "},\"fatal_error\":");
    json_append_error(&writer, &report->fatal_error);
    json_append_char(&writer, '}');

    return writer.failed ? -1 : 0;
}
