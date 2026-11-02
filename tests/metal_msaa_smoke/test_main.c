#include <stdio.h>
#include <string.h>

#include "metal_msaa_smoke.h"

static int s_failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        s_failures++; \
    } \
} while (0)

static void make_expected_pixels(uint8_t *pixels, size_t bytes_per_row)
{
    unsigned y;
    memset(pixels, 0, bytes_per_row * BW_METAL_MSAA_SMOKE_HEIGHT);
    for (y = 0; y < BW_METAL_MSAA_SMOKE_HEIGHT; y++) {
        unsigned x;
        for (x = 0; x < BW_METAL_MSAA_SMOKE_WIDTH; x++) {
            uint8_t *pixel = pixels + y * bytes_per_row + x * 4;
            if (y >= 4 && x < 4) {
                pixel[0] = 255;
            } else {
                pixel[2] = 255;
            }
            pixel[3] = 255;
        }
    }
}

static void set_all_checks(BwMetalMsaaSmokeCorrectness *checks,
                           BwMetalMsaaSmokeCheck value)
{
    checks->clear_color = value;
    checks->draw_color = value;
    checks->depth_clear_reject = value;
    checks->depth_write_reject = value;
    checks->stencil_clear_and_reject = value;
    checks->resolve_readback = value;
}

static void test_memory_lower_bounds(void)
{
    CHECK(bw_metal_msaa_smoke_allocated_bytes_lower_bound(1) == 0);
    CHECK(bw_metal_msaa_smoke_allocated_bytes_lower_bound(2) == 3840);
    CHECK(bw_metal_msaa_smoke_allocated_bytes_lower_bound(4) == 5376);
    CHECK(bw_metal_msaa_smoke_allocated_bytes_lower_bound(8) == 8448);
    CHECK(bw_metal_msaa_smoke_allocated_bytes_lower_bound(16) == 0);
}

static void test_correctness_checks(void)
{
    enum { ROW_BYTES = BW_METAL_MSAA_SMOKE_WIDTH * 4 };
    uint8_t pixels[ROW_BYTES * BW_METAL_MSAA_SMOKE_HEIGHT];
    BwMetalMsaaSmokeReport report;
    BwMetalMsaaSmokeSampleResult *result;

    bw_metal_msaa_smoke_report_init(&report);
    result = &report.results[0];
    make_expected_pixels(pixels, ROW_BYTES);
    bw_metal_msaa_smoke_check_pixels(result, pixels, ROW_BYTES);
    CHECK(bw_metal_msaa_smoke_checks_pass(&result->correctness));

    make_expected_pixels(pixels, ROW_BYTES);
    pixels[0 * ROW_BYTES + 4 * 4 + 0] = 255;
    bw_metal_msaa_smoke_check_pixels(result, pixels, ROW_BYTES);
    CHECK(result->correctness.clear_color ==
          BW_METAL_MSAA_SMOKE_CHECK_FAILED);
    CHECK(result->correctness.depth_clear_reject ==
          BW_METAL_MSAA_SMOKE_CHECK_PASSED);
    CHECK(result->correctness.resolve_readback ==
          BW_METAL_MSAA_SMOKE_CHECK_FAILED);

    make_expected_pixels(pixels, ROW_BYTES);
    pixels[0] = 255;
    bw_metal_msaa_smoke_check_pixels(result, pixels, ROW_BYTES);
    CHECK(result->correctness.depth_clear_reject ==
          BW_METAL_MSAA_SMOKE_CHECK_FAILED);

    make_expected_pixels(pixels, ROW_BYTES);
    pixels[4 * ROW_BYTES + 0] = 0;
    pixels[4 * ROW_BYTES + 2] = 255;
    bw_metal_msaa_smoke_check_pixels(result, pixels, ROW_BYTES);
    CHECK(result->correctness.draw_color ==
          BW_METAL_MSAA_SMOKE_CHECK_FAILED);
    CHECK(result->correctness.stencil_clear_and_reject ==
          BW_METAL_MSAA_SMOKE_CHECK_FAILED);

    make_expected_pixels(pixels, ROW_BYTES);
    pixels[4 * ROW_BYTES + 2 * 4 + 1] = 255;
    bw_metal_msaa_smoke_check_pixels(result, pixels, ROW_BYTES);
    CHECK(result->correctness.depth_write_reject ==
          BW_METAL_MSAA_SMOKE_CHECK_FAILED);

    make_expected_pixels(pixels, ROW_BYTES);
    pixels[4 * ROW_BYTES + 4 * 4 + 0] = 255;
    bw_metal_msaa_smoke_check_pixels(result, pixels, ROW_BYTES);
    CHECK(result->correctness.stencil_clear_and_reject ==
          BW_METAL_MSAA_SMOKE_CHECK_FAILED);

    bw_metal_msaa_smoke_check_pixels(result, pixels, ROW_BYTES - 1);
    CHECK(!bw_metal_msaa_smoke_checks_pass(&result->correctness));
}

static void test_report_logic(void)
{
    BwMetalMsaaSmokeReport report;

    bw_metal_msaa_smoke_report_init(&report);
    CHECK(report.results[0].sample_count == 2);
    CHECK(report.results[1].sample_count == 4);
    CHECK(report.results[2].sample_count == 8);
    CHECK(!bw_metal_msaa_smoke_report_succeeded(&report));

    report.results[0].status = BW_METAL_MSAA_SMOKE_SKIPPED_UNSUPPORTED;
    report.results[1].status = BW_METAL_MSAA_SMOKE_SKIPPED_UNSUPPORTED;
    report.results[2].status = BW_METAL_MSAA_SMOKE_SKIPPED_UNSUPPORTED;
    CHECK(bw_metal_msaa_smoke_report_succeeded(&report));

    report.results[1].supported = 1;
    report.results[1].status = BW_METAL_MSAA_SMOKE_PASSED;
    set_all_checks(&report.results[1].correctness,
                   BW_METAL_MSAA_SMOKE_CHECK_PASSED);
    CHECK(bw_metal_msaa_smoke_report_succeeded(&report));

    report.results[1].status = BW_METAL_MSAA_SMOKE_FAILED;
    CHECK(!bw_metal_msaa_smoke_report_succeeded(&report));

    report.results[1].status = BW_METAL_MSAA_SMOKE_PASSED;
    bw_metal_msaa_smoke_set_fatal_error(
        &report, BW_METAL_MSAA_SMOKE_ERROR_DEVICE,
        0, 0, "injected fatal error");
    CHECK(!bw_metal_msaa_smoke_report_succeeded(&report));
}

static void test_fixed_shape_json(void)
{
    BwMetalMsaaSmokeReport report;
    BwMetalMsaaSmokeSampleResult *two;
    BwMetalMsaaSmokeSampleResult *eight;
    char output[8192];
    char too_small[32];
    const char *expected =
        "{\"schema\":\"bloodwake.metal-msaa-smoke/v1\",\"status\":\"failed\","
        "\"diagnostic_only\":true,\"retail_integration\":\"not_implemented\","
        "\"device_name\":\"Fake \\\"GPU\\\"\\\\line\","
        "\"resources\":{\"width\":8,\"height\":8,"
        "\"color_format\":\"rgba8unorm\","
        "\"depth_stencil_format\":\"depth32float_stencil8\","
        "\"readback_bytes_per_row\":256},\"results\":["
        "{\"sample_count\":2,\"status\":\"passed\",\"supported\":true,"
        "\"correctness\":{\"clear_color\":true,\"draw_color\":true,"
        "\"depth_clear_reject\":true,\"depth_write_reject\":true,"
        "\"stencil_clear_and_reject\":true,\"resolve_readback\":true},"
        "\"memory\":{\"resources_allocated\":true,"
        "\"allocated_bytes_lower_bound\":3840},"
        "\"gpu_timing\":{\"available\":true,\"milliseconds\":0.125000},"
        "\"error\":{\"stage\":\"none\",\"code\":null,\"message\":null}},"
        "{\"sample_count\":4,\"status\":\"skipped_unsupported\","
        "\"supported\":false,\"correctness\":{\"clear_color\":null,"
        "\"draw_color\":null,\"depth_clear_reject\":null,"
        "\"depth_write_reject\":null,\"stencil_clear_and_reject\":null,"
        "\"resolve_readback\":null},"
        "\"memory\":{\"resources_allocated\":false,"
        "\"allocated_bytes_lower_bound\":0},"
        "\"gpu_timing\":{\"available\":false,\"milliseconds\":null},"
        "\"error\":{\"stage\":\"none\",\"code\":null,\"message\":null}},"
        "{\"sample_count\":8,\"status\":\"failed\",\"supported\":true,"
        "\"correctness\":{\"clear_color\":false,\"draw_color\":false,"
        "\"depth_clear_reject\":false,\"depth_write_reject\":false,"
        "\"stencil_clear_and_reject\":false,\"resolve_readback\":false},"
        "\"memory\":{\"resources_allocated\":true,"
        "\"allocated_bytes_lower_bound\":8448},"
        "\"gpu_timing\":{\"available\":false,\"milliseconds\":null},"
        "\"error\":{\"stage\":\"correctness\",\"code\":null,"
        "\"message\":\"bad \\\"pixel\\\"\\n\"}}],"
        "\"summary\":{\"supported\":2,\"passed\":1,"
        "\"skipped_unsupported\":1,\"failed\":1,\"not_run\":0},"
        "\"fatal_error\":{\"stage\":\"none\",\"code\":null,"
        "\"message\":null}}";

    bw_metal_msaa_smoke_report_init(&report);
    snprintf(report.device_name, sizeof report.device_name,
             "Fake \"GPU\"\\line");

    two = &report.results[0];
    two->supported = 1;
    two->status = BW_METAL_MSAA_SMOKE_PASSED;
    set_all_checks(&two->correctness, BW_METAL_MSAA_SMOKE_CHECK_PASSED);
    two->resources_allocated = 1;
    two->allocated_bytes_lower_bound =
        bw_metal_msaa_smoke_allocated_bytes_lower_bound(2);
    two->gpu_timing_available = 1;
    two->gpu_time_milliseconds = 0.125;

    report.results[1].status = BW_METAL_MSAA_SMOKE_SKIPPED_UNSUPPORTED;

    eight = &report.results[2];
    eight->supported = 1;
    eight->resources_allocated = 1;
    eight->allocated_bytes_lower_bound =
        bw_metal_msaa_smoke_allocated_bytes_lower_bound(8);
    set_all_checks(&eight->correctness, BW_METAL_MSAA_SMOKE_CHECK_FAILED);
    bw_metal_msaa_smoke_set_sample_error(
        eight, BW_METAL_MSAA_SMOKE_ERROR_CORRECTNESS,
        0, 0, "bad \"pixel\"\n");

    CHECK(bw_metal_msaa_smoke_format_json(
        &report, output, sizeof output) == 0);
    CHECK(strcmp(output, expected) == 0);
    CHECK(bw_metal_msaa_smoke_format_json(
        &report, too_small, sizeof too_small) == -1);
}

int main(void)
{
    test_memory_lower_bounds();
    test_correctness_checks();
    test_report_logic();
    test_fixed_shape_json();
    return s_failures ? 1 : 0;
}
