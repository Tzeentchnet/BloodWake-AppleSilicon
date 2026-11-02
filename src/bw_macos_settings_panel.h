#ifndef BW_MACOS_SETTINGS_PANEL_H
#define BW_MACOS_SETTINGS_PANEL_H

#include <stddef.h>

#include "bw_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BW_MACOS_SETTINGS_UNSUPPORTED = 0,
    BW_MACOS_SETTINGS_RESTART,
    BW_MACOS_SETTINGS_LIVE,
    BW_MACOS_SETTINGS_FRAME
} bw_macos_settings_apply_kind_t;

typedef enum {
    BW_MACOS_SETTINGS_SAVE_OK = 0,
    BW_MACOS_SETTINGS_SAVE_INVALID = -1,
    BW_MACOS_SETTINGS_SAVE_WRITE_FAILED = -2,
    BW_MACOS_SETTINGS_SAVE_APPLY_FAILED = -3
} bw_macos_settings_save_result_t;

/* Model helpers are independent of AppKit so their persistence and runtime
 * dispatch policy can be tested without opening an interactive window. */
bw_macos_settings_apply_kind_t
bw_macos_settings_classify(const bw_setting_info_t *info);
int bw_macos_settings_save_value(const bw_setting_info_t *info,
                                 const char *value,
                                 char *err, size_t errlen);

/* These entry points must be called on the process main thread. Toggle returns
 * one when the panel is open, zero when it was closed, and -1 on failure. */
int bw_macos_settings_panel_toggle(void);
int bw_macos_settings_panel_is_open(void);
void bw_macos_settings_panel_close(void);

#ifdef BW_MACOS_SETTINGS_PANEL_TESTING
typedef struct {
    int controller_created;
    int panel_created;
    int visible;
    int main_thread;
    int no_pause_notice;
    size_t row_count;
} bw_macos_settings_panel_test_state_t;

typedef struct {
    int enabled;
    bw_macos_settings_apply_kind_t apply_kind;
    char value[128];
    char status[512];
    char support_reason[512];
} bw_macos_settings_panel_test_row_t;

typedef void (*bw_macos_settings_panel_test_lifecycle_fn)(int open,
                                                          void *context);

/* Narrow, test-only access to the actual controller and AppKit controls. */
void bw_macos_settings_panel_test_set_lifecycle_callback(
    bw_macos_settings_panel_test_lifecycle_fn callback, void *context);
int bw_macos_settings_panel_test_window_server_available(void);
int bw_macos_settings_panel_test_state(
    bw_macos_settings_panel_test_state_t *out);
int bw_macos_settings_panel_test_row(
    const char *section, const char *key,
    bw_macos_settings_panel_test_row_t *out);
int bw_macos_settings_panel_test_select(const char *section, const char *key,
                                        const char *value);
int bw_macos_settings_panel_test_visible_error(char *message,
                                               size_t message_len);
void bw_macos_settings_panel_test_dismiss_error(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
