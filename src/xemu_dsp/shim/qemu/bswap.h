/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/* Blood Wake: the two little-endian helpers xemu's DSP uses (x86 host). */
#ifndef BW_XEMU_SHIM_BSWAP_H
#define BW_XEMU_SHIM_BSWAP_H
#include <stdint.h>
#include <string.h>
static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void stl_le_p(void *p, uint32_t v) { memcpy(p, &v, 4); }
#endif
