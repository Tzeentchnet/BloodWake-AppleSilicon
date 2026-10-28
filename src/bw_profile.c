/*
 * bw_profile.c - Blood Wake sampling profiler (our code, not the toolkit's).
 *
 * BW_PROFILE=<file> starts a thread that, about once a millisecond, suspends
 * every other thread of the process for a moment, reads its instruction
 * pointer and resumes it. At the end of the run (clean exit, run limit or
 * crash: bw_profile_dump) the samples are resolved to function names with
 * DbgHelp (the Release build carries a PDB for this) and written as:
 *   - per thread: samples, share of the run, and its top functions;
 *   - overall: the top functions and the top modules.
 * A thread sitting in a wait shows up in ntdll's wait functions, so "busy"
 * and "waiting" read straight off the table.
 *
 * BW_PROFILE_FROM / BW_PROFILE_TO (seconds since start) limit sampling to a
 * window, e.g. the logo movies or a stretch of gameplay.
 *
 * Flat profile only (no call stacks): enough to say where the time goes.
 */
#include <windows.h>
#include <timeapi.h>   /* timeBeginPeriod / timeEndPeriod */
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_profile.h"

#define BWP_MAX_THREADS 128
#define BWP_HASH_BITS 20
#define BWP_HASH (1u << BWP_HASH_BITS)

typedef struct {
    uint64_t rip;
    uint32_t tidx;      /* thread index + 1; 0 = empty slot */
    uint32_t count;
} BwpSlot;

static BwpSlot *s_tab;
static DWORD s_tid[BWP_MAX_THREADS];
static HANDLE s_th[BWP_MAX_THREADS];
static uint64_t s_th_start[BWP_MAX_THREADS];
static uint64_t s_th_samples[BWP_MAX_THREADS];
static int s_nth;
static DWORD s_self;
static volatile LONG s_running, s_stopped, s_dumped;
static char s_out[MAX_PATH];
static double s_from, s_to;
static ULONGLONG s_t0;
static uint64_t s_ticks, s_dropped;

typedef LONG (NTAPI *NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);

static uint64_t thread_start_address(HANDLE h)
{
    static NtQIT q;
    uint64_t addr = 0;
    if (!q)
        q = (NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                  "NtQueryInformationThread");
    if (q)
        q(h, 9 /* ThreadQuerySetWin32StartAddress */, &addr, sizeof(addr), NULL);
    return addr;
}

static void refresh_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            int known = 0;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == s_self)
                continue;
            for (int i = 0; i < s_nth; i++)
                if (s_tid[i] == te.th32ThreadID) { known = 1; break; }
            if (!known && s_nth < BWP_MAX_THREADS) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                      THREAD_QUERY_INFORMATION, FALSE,
                                      te.th32ThreadID);
                if (h) {
                    s_tid[s_nth] = te.th32ThreadID;
                    s_th[s_nth] = h;
                    s_th_start[s_nth] = thread_start_address(h);
                    s_nth++;
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void record(int t, uint64_t rip)
{
    uint64_t hsh = (rip * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)t << 56);
    uint32_t i = (uint32_t)(hsh >> (64 - BWP_HASH_BITS));
    for (uint32_t n = 0; n < 64; n++, i = (i + 1) & (BWP_HASH - 1)) {
        BwpSlot *s = &s_tab[i];
        if (s->tidx == 0) {
            s->rip = rip;
            s->tidx = (uint32_t)t + 1;
            s->count = 1;
            return;
        }
        if (s->rip == rip && s->tidx == (uint32_t)t + 1) {
            s->count++;
            return;
        }
    }
    s_dropped++;
}

/* BW_PROFILE_CALLERS=1 (D7, 2026-10-27): for a sample outside this exe
 * (ntdll waits, ucrtbase's strchr, d3d11), walk the sampled thread's stack
 * with the x64 unwind data until the first frame inside the exe, and count
 * the pair (outside function, that caller). It answers "who waits" and "who
 * calls strchr", which the flat profile can't. Off by default: the walk
 * takes the loader's function-table lock (shared), and a thread suspended
 * while holding it exclusively would stall the sampler. */
typedef struct {
    uint64_t rip, caller;
    uint32_t tidx;
    uint32_t count;
} BwpCSlot;
#define BWP_CHASH (1u << 16)
static BwpCSlot *s_ctab;
static int s_callers;
static uint64_t s_img_lo, s_img_hi;

static void callers_init(void)
{
    const char *e = getenv("BW_PROFILE_CALLERS");
    HMODULE m = GetModuleHandleA(NULL);
    s_callers = e && *e == '1';
    if (!s_callers || !m)
        return;
    {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)m;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const uint8_t *)m + dos->e_lfanew);
        s_img_lo = (uint64_t)(uintptr_t)m;
        s_img_hi = s_img_lo + nt->OptionalHeader.SizeOfImage;
    }
    s_ctab = (BwpCSlot *)calloc(BWP_CHASH, sizeof(BwpCSlot));
    if (!s_ctab)
        s_callers = 0;
}

static void record_caller(int t, const CONTEXT *ctx)
{
    CONTEXT c = *ctx;
    uint64_t caller = 0;
    int depth;
    for (depth = 0; depth < 32 && c.Rip; depth++) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!fn) {
            if (!c.Rsp)
                break;
            c.Rip = *(const DWORD64 *)(uintptr_t)c.Rsp;     /* a leaf: return address on top */
            c.Rsp += 8;
        } else {
            PVOID hd = NULL;
            DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &hd, &ef, NULL);
        }
        if (c.Rip >= s_img_lo && c.Rip < s_img_hi) {
            caller = c.Rip;
            break;
        }
    }
    {
        uint64_t h = (ctx->Rip * 0x9E3779B97F4A7C15ull) ^ (caller * 0xC2B2AE3D27D4EB4Full)
                     ^ ((uint64_t)t << 56);
        uint32_t i = (uint32_t)(h >> 48) & (BWP_CHASH - 1), n;
        for (n = 0; n < 64; n++, i = (i + 1) & (BWP_CHASH - 1)) {
            BwpCSlot *s = &s_ctab[i];
            if (!s->tidx) {
                s->rip = ctx->Rip; s->caller = caller; s->tidx = (uint32_t)t + 1; s->count = 1;
                return;
            }
            if (s->rip == ctx->Rip && s->caller == caller && s->tidx == (uint32_t)t + 1) {
                s->count++;
                return;
            }
        }
    }
}

static DWORD WINAPI sampler(LPVOID arg)
{
    ULONGLONG last_refresh = 0;
    (void)arg;
    timeBeginPeriod(1);
    while (InterlockedCompareExchange(&s_running, 1, 1)) {
        ULONGLONG now = GetTickCount64();
        double secs = (now - s_t0) / 1000.0;
        if (now - last_refresh >= 200) {
            refresh_threads();
            last_refresh = now;
        }
        if (secs >= s_from && (s_to <= 0 || secs < s_to)) {
            s_ticks++;
            for (int t = 0; t < s_nth; t++) {
                CONTEXT ctx;
                if (SuspendThread(s_th[t]) == (DWORD)-1)
                    continue;
                ctx.ContextFlags = s_callers ? CONTEXT_FULL : CONTEXT_CONTROL;
                if (GetThreadContext(s_th[t], &ctx)) {
                    record(t, ctx.Rip);
                    if (s_callers && (ctx.Rip < s_img_lo || ctx.Rip >= s_img_hi))
                        record_caller(t, &ctx);
                    s_th_samples[t]++;
                }
                ResumeThread(s_th[t]);
            }
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    InterlockedExchange(&s_stopped, 1);
    return 0;
}

void bw_profile_init(void)
{
    const char *e = getenv("BW_PROFILE");
    const char *from = getenv("BW_PROFILE_FROM");
    const char *to = getenv("BW_PROFILE_TO");
    HANDLE h;
    if (!e || !*e)
        return;
    snprintf(s_out, sizeof(s_out), "%s", strcmp(e, "1") ? e : "profile.txt");
    s_from = from ? atof(from) : 0.0;
    s_to = to ? atof(to) : 0.0;
    s_tab = (BwpSlot *)calloc(BWP_HASH, sizeof(BwpSlot));
    if (!s_tab)
        return;
    callers_init();
    s_t0 = GetTickCount64();
    s_running = 1;
    h = CreateThread(NULL, 0, sampler, NULL, 0, &s_self);
    if (!h) {
        s_running = 0;
        return;
    }
    SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(h);
    atexit(bw_profile_atexit);
    fprintf(stderr, "[BW] profiler on -> %s (window %.0f..%s s)\n", s_out, s_from,
            s_to > 0 ? to : "end");
}

/* ---- report ------------------------------------------------------------ */

typedef struct {
    char name[160];
    uint32_t tidx;      /* 0 = all threads */
    uint64_t count;
} BwpAgg;

static int agg_cmp(const void *a, const void *b)
{
    uint64_t x = ((const BwpAgg *)a)->count, y = ((const BwpAgg *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void symbol_name(HANDLE proc, uint64_t addr, char *out, size_t n)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    IMAGEHLP_MODULE64 mi;
    memset(buf, 0, sizeof(buf));
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    mi.SizeOfStruct = sizeof(mi);
    if (!SymGetModuleInfo64(proc, addr, &mi))
        mi.ModuleName[0] = 0;
    if (SymFromAddr(proc, addr, &disp, si))
        snprintf(out, n, "%s!%s", mi.ModuleName[0] ? mi.ModuleName : "?", si->Name);
    else if (mi.ModuleName[0])
        snprintf(out, n, "%s!+0x%llx", mi.ModuleName,
                 (unsigned long long)(addr - mi.BaseOfImage));
    else
        snprintf(out, n, "0x%llx", (unsigned long long)addr);
}

/* Add count under (tidx, name), in a small linear-probe table of names. */
static void agg_add(BwpAgg *agg, size_t cap, size_t *used, uint32_t tidx,
                    const char *name, uint64_t count)
{
    uint64_t h = 1469598103934665603ull ^ tidx;
    for (const char *p = name; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    for (size_t i = h % cap, n = 0; n < cap; n++, i = (i + 1) % cap) {
        if (agg[i].count == 0 && agg[i].name[0] == 0) {
            snprintf(agg[i].name, sizeof(agg[i].name), "%s", name);
            agg[i].tidx = tidx;
            agg[i].count = count;
            (*used)++;
            return;
        }
        if (agg[i].tidx == tidx && !strcmp(agg[i].name, name)) {
            agg[i].count += count;
            return;
        }
    }
}

void bw_profile_dump(const char *why)
{
    HANDLE proc = GetCurrentProcess();
    FILE *f;
    size_t cap = 1u << 17, used = 0, mused = 0;
    BwpAgg *agg, *mod;
    uint64_t total = 0;

    if (!s_tab || InterlockedExchange(&s_dumped, 1))
        return;
    InterlockedExchange(&s_running, 0);
    for (int i = 0; i < 200 && !s_stopped; i++)
        Sleep(5);

    f = fopen(s_out, "w");
    if (!f)
        return;
    agg = (BwpAgg *)calloc(cap, sizeof(BwpAgg));
    mod = (BwpAgg *)calloc(4096, sizeof(BwpAgg));
    if (!agg || !mod) {
        fclose(f);
        return;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, NULL, TRUE);

    for (uint32_t i = 0; i < BWP_HASH; i++) {
        BwpSlot *s = &s_tab[i];
        char name[160], modname[160];
        IMAGEHLP_MODULE64 mi;
        if (!s->tidx)
            continue;
        symbol_name(proc, s->rip, name, sizeof(name));
        agg_add(agg, cap, &used, s->tidx, name, s->count);
        agg_add(agg, cap, &used, 0, name, s->count);
        mi.SizeOfStruct = sizeof(mi);
        snprintf(modname, sizeof(modname), "%s",
                 SymGetModuleInfo64(proc, s->rip, &mi) ? mi.ModuleName : "?");
        agg_add(mod, 4096, &mused, s->tidx, modname, s->count);
        agg_add(mod, 4096, &mused, 0, modname, s->count);
        total += s->count;
    }
    qsort(agg, cap, sizeof(BwpAgg), agg_cmp);
    qsort(mod, 4096, sizeof(BwpAgg), agg_cmp);

    fprintf(f, "Blood Wake profile (%s): %llu samples over %llu ticks (~1 ms each), "
               "%d threads, %llu dropped, window %.0f..%.0f s\n\n",
            why, (unsigned long long)total, (unsigned long long)s_ticks, s_nth,
            (unsigned long long)s_dropped, s_from, s_to);

    fprintf(f, "== Threads (samples = ticks the thread was sampled; 100%% = always seen)\n");
    for (int t = 0; t < s_nth; t++) {
        char start[160];
        if (!s_th_samples[t])
            continue;
        symbol_name(proc, s_th_start[t], start, sizeof(start));
        fprintf(f, "T%-3d tid %-6lu %8llu samples  start %s\n", t,
                (unsigned long)s_tid[t], (unsigned long long)s_th_samples[t], start);
        /* top modules and functions for this thread */
        for (int k = 0, shown = 0; k < 4096 && mod[k].count && shown < 6; k++)
            if (mod[k].tidx == (uint32_t)t + 1) {
                fprintf(f, "      module %5.1f%%  %s\n",
                        100.0 * mod[k].count / s_th_samples[t], mod[k].name);
                shown++;
            }
        for (size_t k = 0, shown = 0; k < cap && agg[k].count && shown < 25; k++)
            if (agg[k].tidx == (uint32_t)t + 1) {
                fprintf(f, "      %5.1f%%  %s\n",
                        100.0 * agg[k].count / s_th_samples[t], agg[k].name);
                shown++;
            }
    }
    fprintf(f, "\n== All threads: top modules\n");
    for (int k = 0, shown = 0; k < 4096 && mod[k].count && shown < 15; k++)
        if (mod[k].tidx == 0) {
            fprintf(f, "  %5.1f%%  %s\n", 100.0 * mod[k].count / total, mod[k].name);
            shown++;
        }
    if (s_callers && s_ctab) {
        size_t ccap = 1u << 16, cused = 0;
        BwpAgg *cagg = (BwpAgg *)calloc(ccap, sizeof(BwpAgg));
        if (cagg) {
            for (uint32_t i = 0; i < BWP_CHASH; i++) {
                BwpCSlot *s = &s_ctab[i];
                char a[160], b[160], ab[160];
                if (!s->tidx)
                    continue;
                symbol_name(proc, s->rip, a, sizeof(a));
                if (s->caller)
                    symbol_name(proc, s->caller, b, sizeof(b));
                else
                    snprintf(b, sizeof(b), "(no exe frame)");
                snprintf(ab, sizeof(ab), "%.70s <- %.80s", a, b);
                agg_add(cagg, ccap, &cused, s->tidx, ab, s->count);
            }
            qsort(cagg, ccap, sizeof(BwpAgg), agg_cmp);
            fprintf(f, "\n== Outside the exe, by first exe caller (BW_PROFILE_CALLERS)\n");
            for (int t = 0; t < s_nth; t++) {
                int shown = 0;
                if (!s_th_samples[t])
                    continue;
                for (size_t k = 0; k < ccap && cagg[k].count && shown < 20; k++)
                    if (cagg[k].tidx == (uint32_t)t + 1) {
                        if (!shown)
                            fprintf(f, "T%d\n", t);
                        fprintf(f, "      %5.1f%%  %s\n",
                                100.0 * cagg[k].count / s_th_samples[t], cagg[k].name);
                        shown++;
                    }
            }
            free(cagg);
        }
    }
    /* BW_PROFILE_LINES=<name> (P1, 2026-10-27): the samples of every function
     * whose name contains <name>, by source line. Inlined code resolves to
     * its own function's lines, so a long function's hot spots show even
     * when the compiler folded its callees into it. */
    {
        const char *want = getenv("BW_PROFILE_LINES");
        size_t lcap = 1u << 14, lused = 0;
        BwpAgg *lagg = want && *want ? (BwpAgg *)calloc(lcap, sizeof(BwpAgg)) : NULL;
        if (lagg) {
            uint64_t ltotal = 0;
            for (uint32_t i = 0; i < BWP_HASH; i++) {
                BwpSlot *s = &s_tab[i];
                char name[160], key[200];
                IMAGEHLP_LINE64 ln;
                DWORD ldisp = 0;
                if (!s->tidx)
                    continue;
                symbol_name(proc, s->rip, name, sizeof(name));
                if (!strstr(name, want))
                    continue;
                memset(&ln, 0, sizeof ln);
                ln.SizeOfStruct = sizeof ln;
                if (SymGetLineFromAddr64(proc, s->rip, &ldisp, &ln) && ln.FileName) {
                    const char *b = strrchr(ln.FileName, 92);   /* backslash */
                    snprintf(key, sizeof key, "%s:%lu", b ? b + 1 : ln.FileName,
                             (unsigned long)ln.LineNumber);
                } else
                    snprintf(key, sizeof key, "%s (no line)", name);
                agg_add(lagg, lcap, &lused, 0, key, s->count);
                ltotal += s->count;
            }
            qsort(lagg, lcap, sizeof(BwpAgg), agg_cmp);
            fprintf(f, "\n== Lines of '%s' (BW_PROFILE_LINES): %llu samples\n", want,
                    (unsigned long long)ltotal);
            for (size_t k = 0; k < lcap && lagg[k].count && k < 50; k++)
                fprintf(f, "  %5.1f%%  %s\n", 100.0 * lagg[k].count / (ltotal ? ltotal : 1),
                        lagg[k].name);
            free(lagg);
        }
    }
    /* The hottest instruction addresses, with their offset in the function:
     * which loop inside a long function is spinning. */
    {
        typedef struct { uint64_t rip; uint32_t count; } Hot;
        Hot hot[40];
        int nhot = 0;
        memset(hot, 0, sizeof(hot));
        for (uint32_t i = 0; i < BWP_HASH; i++) {
            BwpSlot *s = &s_tab[i];
            int j;
            if (!s->tidx)
                continue;
            if (nhot < 40) {
                hot[nhot].rip = s->rip;
                hot[nhot].count = s->count;
                nhot++;
                continue;
            }
            for (j = 0; j < 40; j++)
                if (hot[j].count < s->count) {
                    int k, low = j;
                    for (k = 0; k < 40; k++)
                        if (hot[k].count < hot[low].count) low = k;
                    hot[low].rip = s->rip;
                    hot[low].count = s->count;
                    break;
                }
        }
        fprintf(f, "\n== Hottest instruction addresses (any thread)\n");
        for (int pass = 0; pass < nhot; pass++) {
            int best = -1;
            for (int j = 0; j < nhot; j++)
                if (hot[j].count && (best < 0 || hot[j].count > hot[best].count))
                    best = j;
            if (best < 0)
                break;
            {
                char buf[sizeof(SYMBOL_INFO) + 256];
                SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
                DWORD64 disp = 0;
                memset(buf, 0, sizeof(buf));
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 255;
                if (SymFromAddr(proc, hot[best].rip, &disp, si))
                    fprintf(f, "  %5.1f%%  %s+0x%llx\n", 100.0 * hot[best].count / total,
                            si->Name, (unsigned long long)disp);
                else
                    fprintf(f, "  %5.1f%%  0x%llx\n", 100.0 * hot[best].count / total,
                            (unsigned long long)hot[best].rip);
            }
            hot[best].count = 0;
        }
    }
    fprintf(f, "\n== All threads: top functions\n");
    for (size_t k = 0, shown = 0; k < cap && agg[k].count && shown < 80; k++)
        if (agg[k].tidx == 0) {
            fprintf(f, "  %5.1f%%  %s\n", 100.0 * agg[k].count / total, agg[k].name);
            shown++;
        }
    fclose(f);
    fprintf(stderr, "[BW] profile written: %s (%llu samples)\n", s_out,
            (unsigned long long)total);
    SymCleanup(proc);
    free(agg);
    free(mod);
}

void bw_profile_atexit(void)
{
    bw_profile_dump("exit");
}
