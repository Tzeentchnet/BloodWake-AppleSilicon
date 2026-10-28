/*
 * bw_state.c - a read-only window on the game's state, for tests (BW_STATE).
 *
 * BW_STATE=<file> starts a thread that, every BW_STATE_MS (default 100) ms,
 * reads the game's player list from guest memory and appends one line per
 * sample to <file>. It never writes guest memory and calls no game code, so
 * it can't change what the game does; it only saves tools from reading
 * health and position off screenshots.
 *
 * The player list (seen 2026-10-26, Players_FrameUpdate 0x0007AE80, called by
 * Game_Update with ecx = 0x001CEE58): a count at +0xC0, then 0x30-byte
 * entries from +0x08 whose first dword is the player's boat object. In the
 * boat: internal hitpoints current/max at +0x20/+0x24; front, rear, left and
 * right section hitpoints at +0x300, +0x30C, +0x318, +0x324 (their maxima
 * come from the hull definition at [+0x6C] + 0x1478.. times [+0x59C]).
 *
 * BW_STATE_DUMP=<n> also writes the first n bytes of player 1's boat as hex
 * on each line (for finding more fields, e.g. position).
 *
 * Line: t_ms players | per player: boat hp_int hp_int_max hp_f hp_r hp_l hp_rt
 *       x y height throttle rudder revs turbo   [| dump hex]
 * The section hitpoints read 0 on the player's speedboat (only big ships
 * have sections, inferred); hp_int is the health the HUD bar shows.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

#define BW_PLAYERS_VA   0x001CEE58u
#define GUEST_OK(va)    ((va) >= 0x00010000u && (va) < 0x08000000u)   /* the 128 MB the runtime maps */
/* Scene nodes and their matrices can also sit in the contiguous window at
 * 0x80000000 (64 MB, mapped by the toolkit: XBOX_CONTIG_BASE/SIZE). Seen
 * 2026-10-27: player 1's boat in mission 1, node 0x011C4060, matrix at +8
 * 0x80774000, no parent. */
#define NODE_OK(va)     (GUEST_OK(va) || ((va) >= 0x80000000u && (va) < 0x83FFFF00u))

static FILE *s_out;
static unsigned s_ms = 100, s_dump;
/* BW_STATE_WATCH=0xVA[,0xVA...]: guest words printed (unsigned decimal) at
 * the end of every sample line, after " | W <qpc ms>". Up to 8. E.g. the
 * mission clock in ms, 0x1D3FB8 (Objectives_DrawMissionClock's argument). */
static uint32_t s_watch[8];
static unsigned s_nwatch;
static LONGLONG s_qpc0;

static uint32_t rd32(uint32_t va)
{
    return *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
}

static float rdf(uint32_t va)
{
    union { uint32_t u; float f; } v;
    v.u = rd32(va);
    return v.f;
}

/* A game object's position, from its scene node (+4), the way
 * RenderItem_ComputeWorldMatrix (0x30830) builds the world matrix: the node's
 * matrix (override at +0x30, else +8) times each parent's (+0x28), row-vector
 * order (D3DXMatrixMultiply(out, current, parent) at 0xDF324). The translation
 * is row 3; the mirror flag 0x40 only negates row 0, so it doesn't move it.
 *
 * Why not +0x84/+0x88/+0x8C: that is a cache GameObj_UpdateCachedPosition
 * (0x8B610) fills, and with two or more split-screen views nothing refreshes it
 * (seen 2026-10-27, D8: every object read 0.0 in 2-player runs, real positions
 * in 1-player runs). Read-only like the rest: nothing here calls game code.
 * Returns 0 when the node chain doesn't look like guest memory; the caller then
 * falls back to the cache. */
static int obj_world_pos(uint32_t obj, float out[3])
{
    uint32_t node = rd32(obj + 4), m, p, depth;
    float x, y, z;
    if (!NODE_OK(node))
        return 0;
    m = rd32(node + 0x30);
    if (!m)
        m = rd32(node + 8);
    if (!NODE_OK(m))
        return 0;
    x = rdf(m + 0x30);
    y = rdf(m + 0x34);
    z = rdf(m + 0x38);
    for (p = rd32(node + 0x28), depth = 0; p && depth < 16; p = rd32(p + 0x28), depth++) {
        float P[16], nx, ny, nz;
        uint32_t pm, k;
        if (!NODE_OK(p))
            return 0;
        pm = rd32(p + 0x30);
        if (!pm)
            pm = rd32(p + 8);
        if (!NODE_OK(pm))
            return 0;
        for (k = 0; k < 16; k++)
            P[k] = rdf(pm + 4 * k);
        nx = x * P[0] + y * P[4] + z * P[8] + P[12];
        ny = x * P[1] + y * P[5] + z * P[9] + P[13];
        nz = x * P[2] + y * P[6] + z * P[10] + P[14];
        x = nx;
        y = ny;
        z = nz;
    }
    out[0] = x;
    out[1] = y;
    out[2] = z;
    return 1;
}

static void obj_pos(uint32_t obj, float out[3])
{
    if (!obj_world_pos(obj, out)) {
        out[0] = rdf(obj + 0x84);
        out[1] = rdf(obj + 0x88);
        out[2] = rdf(obj + 0x8C);
    }
}

/* BW_STATE_SCAN=<seconds>: once, that long after the first player appears,
 * list every guest address holding a pointer to player 1's boat (committed
 * memory only), to find the game's object lists. Read-only like the rest. */
static unsigned s_scan_after;

static void scan_for_pointer(uint32_t target)
{
    uint32_t va = 0x00010000u, hits = 0;
    fprintf(s_out, "# scan for pointers to %08X\n", target);
    while (va < 0x04000000u && hits < 400) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t host = (uintptr_t)g_xbox_mem_offset + va;
        uint32_t len, k;
        if (!VirtualQuery((void *)host, &mbi, sizeof mbi))
            break;
        len = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - host);
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (k = 0; k + 4 <= len && va + k < 0x04000000u; k += 4)
                if (*(volatile uint32_t *)(host + k) == target) {
                    fprintf(s_out, "# hit %08X\n", va + k);
                    if (++hits >= 400)
                        break;
                }
        }
        va += len ? len : 0x1000;
    }
    fprintf(s_out, "# scan done, %u hits\n", hits);
}

/* BW_STATE_OBJECTS=<n>: every n-th sample, list the game objects. A game
 * object (seen 2026-10-26 on the player's boat) points to itself at +0x10 and
 * carries its name at +0x34 ("Barracuda_Pl..."), hitpoints at +0x20/+0x24
 * and its position at +0x84/+0x88/+0x8C, so a scan for "a dword equal to its
 * own address minus 0x10" finds them all. One "@" line per object with
 * hitpoints: @ t_ms address name hp hp_max x y height. */
static unsigned s_objects;

static void list_objects(ULONGLONG t)
{
    uint32_t va = 0x00300000u, regions = 0, committed = 0, selfp = 0;
    while (va < 0x04000000u) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t host = (uintptr_t)g_xbox_mem_offset + va;
        uint32_t len, k;
        if (!VirtualQuery((void *)host, &mbi, sizeof mbi))
            break;
        len = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - host);
        regions++;
        if (len > 0x04000000u - va)
            len = 0x04000000u - va;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            committed += len;
            for (k = 0x10; k + 0x90 <= len; k += 4) {
                uint32_t obj = va + k - 0x10, hp, hpmax, j;
                char name[33];
                if (*(volatile uint32_t *)(host + k) != obj)
                    continue;
                selfp++;
                hp = rd32(obj + 0x20);
                hpmax = rd32(obj + 0x24);
                if (hpmax == 0 || hpmax > 1000000u || hp > hpmax)
                    continue;
                for (j = 0; j < 32; j++) {
                    char c = *(volatile char *)((uintptr_t)g_xbox_mem_offset + obj + 0x34 + j);
                    if (!c)
                        break;
                    name[j] = (c > 32 && c < 127) ? c : '?';
                }
                name[j] = 0;
                if (j < 2)
                    continue;
                {
                    float pos[3];
                    obj_pos(obj, pos);
                    fprintf(s_out, "@ %llu %08X %s %u %u %.1f %.1f %.1f\n", (unsigned long long)t,
                            obj, name, hp, hpmax, pos[0], pos[1], pos[2]);
                }
            }
        }
        va += len ? len : 0x1000;
    }
    fprintf(s_out, "# objects: %u regions, %u MB committed, %u self-pointers\n",
            regions, committed >> 20, selfp);
}

/* BW_STATE_OBJECTIVES=1: one "O" line whenever a mission objective's enabled,
 * met or failed flag changes (and once for every objective at load). The
 * objective array: count [0x1D3FE0], pointer [0x1D3FFC], 0xEC bytes each,
 * name at +0, enabled +0x24, type +0x38, order +0x3C, met +0x48, failed +0x49,
 * condition +0x50 (Objectives_FindByName, Objectives_LoadFromMission; seen
 * 2026-10-26, A3). */
static int s_objectives;
static uint32_t s_obj_arr, s_obj_n;
static uint8_t s_obj_flags[512];

static void log_objectives(ULONGLONG t)
{
    uint32_t n = rd32(0x001D3FE0), arr = rd32(0x001D3FFC), i;
    if (n > 512 || !GUEST_OK(arr) || !n) {
        s_obj_arr = 0;
        return;
    }
    if (arr != s_obj_arr || n != s_obj_n) {     /* a new mission */
        memset(s_obj_flags, 0xFF, sizeof s_obj_flags);
        s_obj_arr = arr;
        s_obj_n = n;
        fprintf(s_out, "O %llu load %u objectives\n", (unsigned long long)t, n);
    }
    for (i = 0; i < n; i++) {
        uint32_t o = arr + i * 0xEC;
        uint8_t en = *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o + 0x24);
        uint8_t met = *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o + 0x48);
        uint8_t fail = *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o + 0x49);
        uint8_t f = (uint8_t)((en ? 1 : 0) | (met ? 2 : 0) | (fail ? 4 : 0));
        if (f != s_obj_flags[i]) {
            char name[40];
            int j;
            for (j = 0; j < 39; j++) {
                char c = *(volatile char *)((uintptr_t)g_xbox_mem_offset + o + j);
                name[j] = c;
                if (!c)
                    break;
            }
            name[39] = 0;
            fprintf(s_out, "O %llu %3u %-34s en %d met %d failed %d type %d cond %d order %d\n",
                    (unsigned long long)t, i, name, en ? 1 : 0, met ? 1 : 0, fail ? 1 : 0,
                    (int)rd32(o + 0x38), (int)rd32(o + 0x50), (int)rd32(o + 0x3C));
            s_obj_flags[i] = f;
        }
    }
    /* "M" line: where the next primary objective is, like the game's radar
     * arrow (Objectives_GetNextMarker 0x7ED50): the enabled, unmet, unfailed
     * primary (type 0) with the lowest order. MOVE_TO: its point (+0xAC/+0xB0);
     * otherwise the centroid of its live targets, which Objective_GetMarkerPos
     * caches at +0xA4/+0xA8 each frame (seen 2026-10-26, A3). */
    {
        uint32_t best = 0xFFFFFFFFu;
        int best_order = 0x7FFFFFFF;
        for (i = 0; i < n; i++) {
            uint32_t o = arr + i * 0xEC;
            if (!*(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o + 0x24) ||
                *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o + 0x48) ||
                *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o + 0x49) ||
                rd32(o + 0x38) != 0)
                continue;
            if ((int)rd32(o + 0x3C) < best_order) {
                best_order = (int)rd32(o + 0x3C);
                best = i;
            }
        }
        /* "P" lines every 20 samples: unmet internal (type 3) MOVE_TO objectives
         * and their points. Many are triggers that wake held enemies when the
         * player comes close (mission 1's ProxPiranhaA..D). */
        static unsigned s_ptick;
        if ((s_ptick++ % 20) == 0) {
            for (i = 0; i < n; i++) {
                uint32_t o = arr + i * 0xEC;
                char pname[40];
                int j;
                if (!*(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o + 0x24) ||
                    *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o + 0x48) ||
                    rd32(o + 0x38) != 3 || rd32(o + 0x50) != 0)
                    continue;
                for (j = 0; j < 39; j++) {
                    pname[j] = *(volatile char *)((uintptr_t)g_xbox_mem_offset + o + j);
                    if (!pname[j] || pname[j] == ' ')
                        pname[j] = pname[j] ? '_' : 0;
                    if (!pname[j])
                        break;
                }
                pname[39] = 0;
                fprintf(s_out, "P %llu %u %s %.1f %.1f\n", (unsigned long long)t, i, pname,
                        rdf(o + 0xAC), rdf(o + 0xB0));
            }
        }
        if (best != 0xFFFFFFFFu) {
            uint32_t o = arr + best * 0xEC, cond = rd32(o + 0x50);
            float mx = cond == 0 ? rdf(o + 0xAC) : rdf(o + 0xA4);
            float my = cond == 0 ? rdf(o + 0xB0) : rdf(o + 0xA8);
            char name[40];
            int j;
            for (j = 0; j < 39; j++) {
                name[j] = *(volatile char *)((uintptr_t)g_xbox_mem_offset + o + j);
                if (!name[j] || name[j] == ' ')
                    name[j] = name[j] ? '_' : 0;
                if (!name[j])
                    break;
            }
            name[39] = 0;
            fprintf(s_out, "M %llu %u %s %u %.1f %.1f\n", (unsigned long long)t, best, name, cond, mx, my);
        }
    }
}

static DWORD WINAPI state_thread(LPVOID unused)
{
    unsigned sample = 0;
    ULONGLONG t0 = GetTickCount64(), first = 0;
    int scanned = 0;
    (void)unused;
    {
        /* The time 0 of this file on the performance counter, in seconds:
         * the clock the APU's "[APU] first submit t=" line uses, so audio
         * (audio.wav starts at the first submit) and state line up. */
        LARGE_INTEGER c, f;
        QueryPerformanceCounter(&c);
        QueryPerformanceFrequency(&f);
        fprintf(s_out, "# t0_qpc %.6f\n", (double)(c.QuadPart / f.QuadPart)
                + (double)(c.QuadPart % f.QuadPart) / (double)f.QuadPart);
    }
    for (;;) {
        if (s_scan_after && !scanned) {
            uint32_t np = rd32(BW_PLAYERS_VA + 0xC0);
            if (np >= 1 && np <= 4) {
                if (!first)
                    first = GetTickCount64();
                else if (GetTickCount64() - first > s_scan_after * 1000ull) {
                    uint32_t boat = rd32(BW_PLAYERS_VA + 8);
                    const char *ranges = getenv("BW_STATE_RANGES");
                    if (GUEST_OK(boat))
                        scan_for_pointer(boat);
                    /* BW_STATE_RANGES=va:len,va:len: dump those once too */
                    while (ranges && *ranges) {
                        char *end;
                        uint32_t va = (uint32_t)strtoul(ranges, &end, 0), len = 0, k;
                        if (*end == ':')
                            len = (uint32_t)strtoul(end + 1, &end, 0);
                        for (k = 0; k < len && GUEST_OK(va + k); k += 16)
                            fprintf(s_out, "# %08X: %08X %08X %08X %08X\n", va + k,
                                    rd32(va + k), rd32(va + k + 4), rd32(va + k + 8),
                                    rd32(va + k + 12));
                        ranges = (*end == ',') ? end + 1 : "";
                    }
                    scanned = 1;
                }
            }
        }
        uint32_t n = rd32(BW_PLAYERS_VA + 0xC0), i;
        fprintf(s_out, "%llu %u", (unsigned long long)(GetTickCount64() - t0), n);
        if (n > 4)
            n = 0;                                 /* not set up yet */
        for (i = 0; i < n; i++) {
            uint32_t boat = rd32(BW_PLAYERS_VA + 8 + 0x30 * i);
            if (!GUEST_OK(boat)) {
                fprintf(s_out, " | -");
                continue;
            }
            fprintf(s_out, " | %08X %d %d %d %d %d %d", boat,
                    (int)rd32(boat + 0x20), (int)rd32(boat + 0x24),
                    (int)rd32(boat + 0x300), (int)rd32(boat + 0x30C),
                    (int)rd32(boat + 0x318), (int)rd32(boat + 0x324));
            /* position on the water and height (the scene node's world
             * translation, obj_world_pos; the +0x84 cache is stale in split
             * screen), throttle and rudder input (+0x398, +0x39C), engine
             * revs (+0x3B8), turbo meter 0..1 (+0x474): seen 2026-10-26
             * (ANALYSIS) */
            {
                float pos[3];
                obj_pos(boat, pos);
                fprintf(s_out, " %.2f %.2f %.2f %.2f %.2f %.0f %.3f",
                        pos[0], pos[1], pos[2],
                        rdf(boat + 0x398), rdf(boat + 0x39C), rdf(boat + 0x3B8),
                        rdf(boat + 0x474));
            }
        }
        if (s_dump && n) {
            uint32_t boat = rd32(BW_PLAYERS_VA + 8), k;
            if (GUEST_OK(boat)) {
                fprintf(s_out, " |");
                for (k = 0; k < s_dump; k += 4)
                    fprintf(s_out, " %08X", rd32(boat + k));
            }
        }
        if (s_nwatch) {
            /* BW_STATE_WATCH: the performance-counter time in ms (finer than
             * the tick count at the line's start), then each watched word */
            LARGE_INTEGER c, f;
            QueryPerformanceCounter(&c);
            QueryPerformanceFrequency(&f);
            fprintf(s_out, " | W %.3f", (double)(c.QuadPart - s_qpc0) * 1000.0 / (double)f.QuadPart);
            for (i = 0; i < s_nwatch; i++)
                fprintf(s_out, " %u", rd32(s_watch[i]));
        }
        fputc('\n', s_out);
        if (s_objects && (sample++ % s_objects) == 0 && rd32(BW_PLAYERS_VA + 0xC0) - 1u < 4u)
            list_objects(GetTickCount64() - t0);
        if (s_objectives)
            log_objectives(GetTickCount64() - t0);
        fflush(s_out);
        Sleep(s_ms);
    }
    return 0;
}

void bw_state_start(void)
{
    const char *path = getenv("BW_STATE"), *e;
    HANDLE h;
    if (!path || !*path || !g_xbox_mem_offset)
        return;
    if ((e = getenv("BW_STATE_MS")) && atoi(e) > 0)
        s_ms = (unsigned)atoi(e);
    if ((e = getenv("BW_STATE_DUMP")) && strtoul(e, NULL, 0) > 0)
        s_dump = (unsigned)strtoul(e, NULL, 0) & ~3u;   /* hex (0x5A4) or decimal */
    if ((e = getenv("BW_STATE_OBJECTS")) && atoi(e) > 0)
        s_objects = (unsigned)atoi(e);
    if ((e = getenv("BW_STATE_SCAN")) && atoi(e) > 0)
        s_scan_after = (unsigned)atoi(e);
    if ((e = getenv("BW_STATE_OBJECTIVES")) && atoi(e) > 0)
        s_objectives = 1;
    if ((e = getenv("BW_STATE_WATCH")) && *e) {
        char *end;
        while (*e && s_nwatch < 8) {
            uint32_t va = (uint32_t)strtoul(e, &end, 0);
            if (end == e)
                break;
            if (GUEST_OK(va))
                s_watch[s_nwatch++] = va & ~3u;
            e = (*end == ',') ? end + 1 : end;
        }
    }
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        s_qpc0 = c.QuadPart;
    }
    /* Append: the game reboots itself between menus and missions, and each
     * boot is a new process. "#" marks where each boot starts. */
    s_out = fopen(path, "a");
    if (!s_out)
        return;
    fprintf(s_out, "# boot %lu\n", (unsigned long)GetCurrentProcessId());
    h = CreateThread(NULL, 0, state_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
    fprintf(stderr, "[BW] state readout every %u ms -> %s%s\n", s_ms, path,
            s_dump ? " (with boat dump)" : "");
}
