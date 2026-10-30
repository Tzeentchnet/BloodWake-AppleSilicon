/*
 * bw_debugflags.c - turn on the developers' own debug switches, for tests.
 *
 * Blood Wake keeps a block of debug bytes at 0x1E68B0..0x1E691F that the
 * shipped game never sets (ANALYSIS "Naming batch 1", seen 2026-10-26). Game
 * code still reads them, e.g.:
 *   0x1E690B  win the mission while pad 1 holds L + R + Black
 *             (Objectives_Update -> Objectives_DebugCompleteAll 0x7D420)
 *   0x1E6909  mission clock on the HUD
 *   0x1E68F8  objective debug text
 *
 * BW_DEBUG_BYTES=0x1E690B=1,0x1E6909=1 writes those bytes now and again every
 * 500 ms (so a reboot's fresh .bss or a reset can't undo them). Only addresses
 * inside that block are accepted; anything else is refused and logged. Off
 * unless the variable is set, so a normal run plays exactly as shipped.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/win32_compat.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern ptrdiff_t g_xbox_mem_offset;

#define DBG_FIRST 0x001E68B0u
#define DBG_LAST  0x001E691Fu
#define DBG_MAX   16

static struct { uint32_t va; uint8_t val; } s_set[DBG_MAX];
static int s_n;

static DWORD WINAPI debugflags_thread(LPVOID unused)
{
    int i;
    (void)unused;
    for (;;) {
        for (i = 0; i < s_n; i++)
            *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + s_set[i].va) = s_set[i].val;
        Sleep(500);
    }
    return 0;
}

void bw_debugflags_start(void)
{
    const char *p = getenv("BW_DEBUG_BYTES");
    HANDLE h;
    int i;
    if (!p || !*p || !g_xbox_mem_offset)
        return;
    while (*p && s_n < DBG_MAX) {
        char *end;
        uint32_t va = (uint32_t)strtoul(p, &end, 0), val = 1;
        if (end == p)
            break;
        if (*end == '=')
            val = (uint32_t)strtoul(end + 1, &end, 0);
        if (va >= DBG_FIRST && va <= DBG_LAST) {
            s_set[s_n].va = va;
            s_set[s_n].val = (uint8_t)val;
            s_n++;
        } else {
            fprintf(stderr, "[BW] BW_DEBUG_BYTES: 0x%08X is outside the debug block, ignored\n", va);
        }
        p = end;
        while (*p == ',' || *p == ' ')
            p++;
    }
    if (!s_n)
        return;
    h = CreateThread(NULL, 0, debugflags_thread, NULL, 0, NULL);
    if (!h) {
        fprintf(stderr, "[BW] could not start the debug-byte thread\n");
        return;
    }
    CloseHandle(h);
    for (i = 0; i < s_n; i++)
        fprintf(stderr, "[BW] debug byte 0x%08X = %u\n", s_set[i].va, s_set[i].val);
}
