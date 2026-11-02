/*
 * bw_settings.h - the settings file (a modern PC game's options), agent D5.
 *
 * The player picks the edition, display, frame rate, graphics, audio and other
 * options in the launcher (tools/launcher.pyw), which writes
 *   %LOCALAPPDATA%\BloodWake\settings.ini
 * On macOS/POSIX the default is
 *   $XDG_CONFIG_HOME/BloodWake/settings.ini, or
 *   $HOME/.config/BloodWake/settings.ini when XDG_CONFIG_HOME is unset.
 * Every boot reads it first thing and turns each choice into the switches
 * the port already has (BW_EDITION, BW_FIX tokens, BW_VFX_*, BW_HIFPS,
 * BW_ASPECT, RECOMP_WINDOW_SCALE, ...), each only where the environment
 * hasn't set it: environment variables still win, so tests stay exact.
 *
 *   BW_SETTINGS=<file>   read this file instead
 *   BW_SETTINGS=none     read no file (tools/bw_run.py sets this by default)
 *
 * No file means nothing changes: the game runs exactly as before. And the
 * edition's own rules still apply: Original ignores every change made
 * for a later edition, whatever the file says (bw_edition.c).
 *
 * The table of settings in bw_settings.c is the one source of truth: the
 * launcher reads it from the exe (bloodwake.exe --settings-schema) and builds
 * its tabs from it.
 */
#ifndef BW_SETTINGS_H
#define BW_SETTINGS_H

#include <stddef.h>
#include <stdio.h>

typedef struct {
    const char *value;
    const char *label;
} bw_setting_option_info_t;

typedef struct {
    const char *tab;
    const char *section;
    const char *key;
    const char *label;
    const char *tip;
    const char *default_value;
    const char *editions;
    const char *availability;
    const char *apply_mode;
    const char *support_reason;
    size_t option_count;
    int supported;
} bw_setting_info_t;

typedef int (*bw_setting_runtime_get_fn)(char *value, size_t value_len,
                                         void *context, char *err, size_t errlen);
typedef int (*bw_setting_runtime_apply_fn)(const char *value, void *context,
                                           char *err, size_t errlen);

/* Read the file and set the environment defaults. Call first thing in the
 * process, before anything reads a switch (main.c), in the host window's
 * process too, whose environment every boot inherits. Logs one "[SET]"
 * line. */
void bw_settings_apply(void);

/* The table as JSON (bloodwake.exe --settings-schema), for the launcher. */
void bw_settings_print_schema(FILE *f);

/* The settings file's path (BW_SETTINGS or the default); NULL for none. */
const char *bw_settings_path(void);

/* Schema enumeration and strict choice validation for native settings UIs.
 * Returned strings point into the static table and remain valid for the
 * process lifetime. */
size_t bw_settings_count(void);
int bw_settings_get_info(size_t index, bw_setting_info_t *out);
int bw_settings_get_option(size_t setting_index, size_t option_index,
                           bw_setting_option_info_t *out);
int bw_settings_validate_value(const char *section, const char *key,
                               const char *value, char *err, size_t errlen);

/* Atomically update one known setting in the configured INI file. Existing
 * unknown sections and [advanced] entries are preserved. Returns 0 on
 * success or -1 with an actionable error. */
int bw_settings_write_value(const char *section, const char *key,
                            const char *value, char *err, size_t errlen);

/* Live-capable subsystems register one owner for each setting they implement.
 * The callback owns its thread/frame-boundary policy: a frame-boundary setting
 * must queue or atomically publish the value rather than mutate active GPU
 * state from the UI thread. Restart-only and unavailable settings cannot be
 * registered or applied through this API. */
int bw_settings_runtime_register(const char *section, const char *key,
                                 bw_setting_runtime_get_fn get_current,
                                 bw_setting_runtime_apply_fn apply,
                                 void *context, char *err, size_t errlen);
int bw_settings_runtime_get(const char *section, const char *key,
                            char *value, size_t value_len,
                            char *err, size_t errlen);
int bw_settings_runtime_apply(const char *section, const char *key,
                              const char *value, char *err, size_t errlen);

#endif
