#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "bw_hook_platform.h"
#include "bw_widescreen.h"

static uint32_t guest_words[0x10000];
ptrdiff_t g_xbox_mem_offset;
BW_HOOK_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
static float ws_k = 1.0f;
static uint32_t parameters[2], expected_eax, expected_ecx, expected_edx;
static unsigned marker_count, guest_calls, summaries;

float bw_ws_k(void) { return ws_k; }
void bw_edition_add_summary(void (*fn)(void)) { assert(fn); summaries++; }

static void emit_guest(void)
{
    assert(marker_count < 2 && g_ecx == BW_WS_NOP_HEADER);
    assert(guest_words[g_esp / 4] == 0x00126300u);
    parameters[marker_count++] = g_edx;
    g_eax = g_ecx = g_edx = 0;
    g_esp += 4;
}

void (*recomp_lookup(uint32_t va))(void)
{
    assert(va == 0x00126300u);
    return emit_guest;
}

void sub_00028B30_gen(void)
{
    assert(g_eax == expected_eax && g_ecx == expected_ecx && g_edx == expected_edx);
    guest_calls++;
    g_eax = 0x11u; g_ecx = 0x22u; g_edx = 0x33u;
    g_esp += 4;
}

void sub_00028B30(void);

int main(void)
{
    const uint32_t sp = 0x10000u;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest_words;
    expected_eax = 1; expected_ecx = 2; expected_edx = 3;
    g_eax = expected_eax; g_ecx = expected_ecx; g_edx = expected_edx; g_esp = sp;
    sub_00028B30();
    assert(guest_calls == 1 && marker_count == 0 && g_esp == sp + 4);

    ws_k = 1.5f;
    expected_eax = 4; expected_ecx = 5; expected_edx = 6;
    g_eax = expected_eax; g_ecx = expected_ecx; g_edx = expected_edx; g_esp = sp;
    sub_00028B30();
    assert(guest_calls == 2 && marker_count == 2 && summaries == 1);
    assert(parameters[0] == BW_WS_MARK(BW_WS_OP_2D_BEGIN, BW_WS_WHAT_OVERLAY));
    assert(parameters[1] == BW_WS_MARK(BW_WS_OP_2D_END, BW_WS_WHAT_OVERLAY));
    assert(g_eax == 0x11u && g_ecx == 0x22u && g_edx == 0x33u && g_esp == sp + 4);
    puts("guest marker dispatch tests passed");
    return 0;
}
