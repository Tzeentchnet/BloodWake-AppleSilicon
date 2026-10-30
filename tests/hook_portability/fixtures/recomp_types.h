#ifndef TEST_RECOMP_TYPES_H
#define TEST_RECOMP_TYPES_H

/* Synthetic declarations for the ocean driver's tests; no game code. */
#include <stddef.h>
#include <stdint.h>

extern ptrdiff_t g_xbox_mem_offset;
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern __thread uint32_t g_ebp, g_seh_ebp;
extern __thread int g_fp_top, g_fp_cmp;
extern __thread uint16_t g_fp_control_word, g_fp_cc;
int64_t recomp_fist(double value, uint16_t control, unsigned bits);

#endif
