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

#include <stdio.h>

/* Read the file and set the environment defaults. Call first thing in the
 * process, before anything reads a switch (main.c), in the host window's
 * process too, whose environment every boot inherits. Logs one "[SET]"
 * line. */
void bw_settings_apply(void);

/* The table as JSON (bloodwake.exe --settings-schema), for the launcher. */
void bw_settings_print_schema(FILE *f);

/* The settings file's path (BW_SETTINGS or the default); NULL for none. */
const char *bw_settings_path(void);

#endif
