/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/* Blood Wake: minimal stand-in for QEMU's osdep.h, enough for xemu's DSP. */
#ifndef BW_XEMU_SHIM_OSDEP_H
#define BW_XEMU_SHIM_OSDEP_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef _MSC_VER
typedef ptrdiff_t ssize_t;   /* POSIX type xemu uses; 64-bit signed like LP64's */
#endif
#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))
#define g_free(p) free(p)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#endif
