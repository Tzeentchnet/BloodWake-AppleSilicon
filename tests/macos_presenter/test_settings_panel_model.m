#include <stdio.h>
#include <string.h>

#include "bw_macos_settings_panel.h"

static int s_errors;
static int s_validate_calls;
static int s_write_calls;
static int s_apply_calls;
static int s_fail_validate;
static int s_fail_write;
static int s_fail_apply;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL settings panel model: %s\n", message);
        s_errors++;
    }
}

static void reset_calls(void)
{
    s_validate_calls = 0;
    s_write_calls = 0;
    s_apply_calls = 0;
    s_fail_validate = 0;
    s_fail_write = 0;
    s_fail_apply = 0;
}

size_t bw_settings_count(void)
{
    return 0;
}

int bw_settings_get_info(size_t index, bw_setting_info_t *out)
{
    (void)index;
    (void)out;
    return -1;
}

int bw_settings_get_option(size_t setting_index, size_t option_index,
                           bw_setting_option_info_t *out)
{
    (void)setting_index;
    (void)option_index;
    (void)out;
    return -1;
}

const char *bw_settings_path(void)
{
    return NULL;
}

int bw_settings_validate_value(const char *section, const char *key,
                               const char *value, char *err, size_t errlen)
{
    s_validate_calls++;
    check(!strcmp(section, "test") && !strcmp(key, "choice"),
          "validation receives the schema identity");
    if (s_fail_validate || strcmp(value, "on")) {
        snprintf(err, errlen, "invalid choice");
        return -1;
    }
    return 0;
}

int bw_settings_write_value(const char *section, const char *key,
                            const char *value, char *err, size_t errlen)
{
    s_write_calls++;
    check(!strcmp(section, "test") && !strcmp(key, "choice") &&
          !strcmp(value, "on"), "writer receives the validated choice");
    if (s_fail_write) {
        snprintf(err, errlen, "disk is read-only");
        return -1;
    }
    return 0;
}

int bw_settings_runtime_get(const char *section, const char *key,
                            char *value, size_t value_len,
                            char *err, size_t errlen)
{
    (void)section;
    (void)key;
    (void)value;
    (void)value_len;
    (void)err;
    (void)errlen;
    return -1;
}

int bw_settings_runtime_apply(const char *section, const char *key,
                              const char *value, char *err, size_t errlen)
{
    s_apply_calls++;
    check(!strcmp(section, "test") && !strcmp(key, "choice") &&
          !strcmp(value, "on"), "runtime apply receives the persisted choice");
    if (s_fail_apply) {
        snprintf(err, errlen, "runtime refused");
        return -1;
    }
    return 0;
}

static bw_setting_info_t info_with_mode(const char *mode, int supported)
{
    bw_setting_info_t info;
    memset(&info, 0, sizeof info);
    info.section = "test";
    info.key = "choice";
    info.label = "Choice";
    info.default_value = "off";
    info.apply_mode = mode;
    info.support_reason = "not implemented";
    info.supported = supported;
    return info;
}

int main(void)
{
    char error[512];
    bw_setting_info_t info;
    int result;

    @autoreleasepool {
        info = info_with_mode("restart", 0);
        check(bw_macos_settings_classify(&info) == BW_MACOS_SETTINGS_UNSUPPORTED,
              "unsupported capability overrides the advertised mode");
        info = info_with_mode("unsupported", 1);
        check(bw_macos_settings_classify(&info) == BW_MACOS_SETTINGS_UNSUPPORTED,
              "explicit unsupported mode is unavailable");
        info = info_with_mode("restart", 1);
        check(bw_macos_settings_classify(&info) == BW_MACOS_SETTINGS_RESTART,
              "restart mode is classified");
        info = info_with_mode("live", 1);
        check(bw_macos_settings_classify(&info) == BW_MACOS_SETTINGS_LIVE,
              "live mode is classified");
        info = info_with_mode("frame", 1);
        check(bw_macos_settings_classify(&info) == BW_MACOS_SETTINGS_FRAME,
              "frame-boundary mode is classified");

        reset_calls();
        info = info_with_mode("restart", 0);
        result = bw_macos_settings_save_value(&info, "on", error, sizeof error);
        check(result == BW_MACOS_SETTINGS_SAVE_INVALID &&
              !s_validate_calls && !s_write_calls && !s_apply_calls,
              "unsupported settings are rejected before persistence");
        check(strstr(error, "not implemented") != NULL,
              "unsupported reason is returned to the UI");

        reset_calls();
        info = info_with_mode("restart", 1);
        s_fail_validate = 1;
        result = bw_macos_settings_save_value(&info, "bad", error, sizeof error);
        check(result == BW_MACOS_SETTINGS_SAVE_INVALID &&
              s_validate_calls == 1 && !s_write_calls && !s_apply_calls,
              "invalid choices never reach persistence or runtime apply");

        reset_calls();
        s_fail_write = 1;
        result = bw_macos_settings_save_value(&info, "on", error, sizeof error);
        check(result == BW_MACOS_SETTINGS_SAVE_WRITE_FAILED &&
              s_validate_calls == 1 && s_write_calls == 1 && !s_apply_calls,
              "write errors are returned and never applied");
        check(strstr(error, "read-only") != NULL,
              "actionable writer error reaches the UI");

        reset_calls();
        result = bw_macos_settings_save_value(&info, "on", error, sizeof error);
        check(result == BW_MACOS_SETTINGS_SAVE_OK &&
              s_validate_calls == 1 && s_write_calls == 1 && !s_apply_calls,
              "restart-required settings are saved without runtime apply");

        reset_calls();
        info = info_with_mode("live", 1);
        result = bw_macos_settings_save_value(&info, "on", error, sizeof error);
        check(result == BW_MACOS_SETTINGS_SAVE_OK &&
              s_write_calls == 1 && s_apply_calls == 1,
              "live settings are persisted and applied");

        reset_calls();
        info = info_with_mode("frame", 1);
        result = bw_macos_settings_save_value(&info, "on", error, sizeof error);
        check(result == BW_MACOS_SETTINGS_SAVE_OK &&
              s_write_calls == 1 && s_apply_calls == 1,
              "frame-boundary settings use the runtime apply API");

        reset_calls();
        info = info_with_mode("live", 1);
        s_fail_apply = 1;
        result = bw_macos_settings_save_value(&info, "on", error, sizeof error);
        check(result == BW_MACOS_SETTINGS_SAVE_APPLY_FAILED &&
              s_write_calls == 1 && s_apply_calls == 1,
              "runtime failure is distinct after a successful atomic save");
        check(strstr(error, "Saved for the next launch") != NULL &&
              strstr(error, "runtime refused") != NULL,
              "runtime failure explains that persistence succeeded");
    }
    return s_errors ? 1 : 0;
}
