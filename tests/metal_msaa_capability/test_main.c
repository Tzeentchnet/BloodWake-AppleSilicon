#include <stdio.h>
#include <string.h>

#include "metal_msaa_capability.h"

typedef struct FakeQuery {
    BwMetalMsaaQueryState states[BW_METAL_MSAA_SAMPLE_COUNT_TOTAL];
    unsigned requested[BW_METAL_MSAA_SAMPLE_COUNT_TOTAL];
    size_t call_count;
} FakeQuery;

static int s_failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        s_failures++; \
    } \
} while (0)

static BwMetalMsaaQueryState fake_query(
    void *context, unsigned sample_count, char *error, size_t error_capacity)
{
    FakeQuery *fake = context;
    BwMetalMsaaQueryState state = fake->states[fake->call_count];
    fake->requested[fake->call_count++] = sample_count;
    if (state == BW_METAL_MSAA_QUERY_ERROR)
        snprintf(error, error_capacity, "injected failure for %ux", sample_count);
    return state;
}

static void check_json(const BwMetalMsaaProbeResult *result,
                       const char *expected)
{
    char output[2048];
    CHECK(bw_metal_msaa_format_json(result, output, sizeof output) == 0);
    CHECK(strcmp(output, expected) == 0);
}

static void test_supported_and_unsupported(void)
{
    FakeQuery fake = {
        { BW_METAL_MSAA_SUPPORTED, BW_METAL_MSAA_UNSUPPORTED,
          BW_METAL_MSAA_SUPPORTED },
        { 0 },
        0
    };
    BwMetalMsaaProbeSource source = {
        "Fake GPU", NULL, fake_query, &fake
    };
    BwMetalMsaaProbeResult result;

    CHECK(bw_metal_msaa_probe(&source, &result) == 0);
    CHECK(result.succeeded);
    CHECK(fake.call_count == 3);
    CHECK(fake.requested[0] == 2);
    CHECK(fake.requested[1] == 4);
    CHECK(fake.requested[2] == 8);
    check_json(
        &result,
        "{\"schema_version\":1,\"status\":\"ok\",\"device_name\":\"Fake GPU\","
        "\"sample_counts\":{\"2\":\"supported\",\"4\":\"unsupported\","
        "\"8\":\"supported\"},\"bw_vfx_msaa_policy\":\"unsupported\","
        "\"error\":{\"stage\":\"none\",\"sample_count\":null,\"message\":null}}");
}

static void test_query_error(void)
{
    FakeQuery fake = {
        { BW_METAL_MSAA_UNSUPPORTED, BW_METAL_MSAA_QUERY_ERROR,
          BW_METAL_MSAA_SUPPORTED },
        { 0 },
        0
    };
    BwMetalMsaaProbeSource source = {
        "Fake GPU", NULL, fake_query, &fake
    };
    BwMetalMsaaProbeResult result;

    CHECK(bw_metal_msaa_probe(&source, &result) == -1);
    CHECK(!result.succeeded);
    CHECK(fake.call_count == 2);
    CHECK(result.error_stage == BW_METAL_MSAA_ERROR_QUERY);
    CHECK(result.error_sample_count == 4);
    check_json(
        &result,
        "{\"schema_version\":1,\"status\":\"error\",\"device_name\":\"Fake GPU\","
        "\"sample_counts\":{\"2\":\"unsupported\",\"4\":\"error\","
        "\"8\":\"not_queried\"},\"bw_vfx_msaa_policy\":\"unsupported\","
        "\"error\":{\"stage\":\"query\",\"sample_count\":4,"
        "\"message\":\"injected failure for 4x\"}}");
}

static void test_device_error(void)
{
    BwMetalMsaaProbeSource source = {
        NULL, "MTLCreateSystemDefaultDevice returned no Metal device", NULL, NULL
    };
    BwMetalMsaaProbeResult result;

    CHECK(bw_metal_msaa_probe(&source, &result) == -1);
    CHECK(!result.succeeded);
    CHECK(result.error_stage == BW_METAL_MSAA_ERROR_DEVICE);
    check_json(
        &result,
        "{\"schema_version\":1,\"status\":\"error\",\"device_name\":null,"
        "\"sample_counts\":{\"2\":\"not_queried\",\"4\":\"not_queried\","
        "\"8\":\"not_queried\"},\"bw_vfx_msaa_policy\":\"unsupported\","
        "\"error\":{\"stage\":\"device\",\"sample_count\":null,"
        "\"message\":\"MTLCreateSystemDefaultDevice returned no Metal device\"}}");
}

static void test_json_escaping_and_bounds(void)
{
    FakeQuery fake = {
        { BW_METAL_MSAA_UNSUPPORTED, BW_METAL_MSAA_UNSUPPORTED,
          BW_METAL_MSAA_UNSUPPORTED },
        { 0 },
        0
    };
    BwMetalMsaaProbeSource source = {
        "Fake \"GPU\"\\line\n2", NULL, fake_query, &fake
    };
    BwMetalMsaaProbeResult result;
    char too_small[16];

    CHECK(bw_metal_msaa_probe(&source, &result) == 0);
    check_json(
        &result,
        "{\"schema_version\":1,\"status\":\"ok\","
        "\"device_name\":\"Fake \\\"GPU\\\"\\\\line\\n2\","
        "\"sample_counts\":{\"2\":\"unsupported\",\"4\":\"unsupported\","
        "\"8\":\"unsupported\"},\"bw_vfx_msaa_policy\":\"unsupported\","
        "\"error\":{\"stage\":\"none\",\"sample_count\":null,\"message\":null}}");
    CHECK(bw_metal_msaa_format_json(&result, too_small,
                                    sizeof too_small) == -1);
}

int main(void)
{
    test_supported_and_unsupported();
    test_query_error();
    test_device_error();
    test_json_escaping_and_bounds();
    return s_failures ? 1 : 0;
}
