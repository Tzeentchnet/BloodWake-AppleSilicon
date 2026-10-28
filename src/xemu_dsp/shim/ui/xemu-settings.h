/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/* Blood Wake: the one xemu setting the DSP reads (the JIT is not built). */
#ifndef BW_XEMU_SHIM_SETTINGS_H
#define BW_XEMU_SHIM_SETTINGS_H
#include <stdbool.h>
struct bw_xemu_config { struct { bool use_dsp_jit; } audio; };
#define g_config bw_xemu_g_config   /* not the toolkit's (static) g_config */
extern struct bw_xemu_config g_config;
#endif
