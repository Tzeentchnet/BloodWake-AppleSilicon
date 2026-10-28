/*
 * bw_framelog.c - per-frame event log and long-frame stack sampler (agent P2b,
 * 2026-10-27; a test aid in every edition, inert unless BW_FRAMELOG is set).
 *
 * Why: P1 saw one ~35 ms frame about every second in mission 3 when the
 * game may run more than 3 KickOff segments ahead of the NV2A walk, and
 * nothing said what the game thread was waiting on in that frame.
 *
 * BW_FRAMELOG=<file> records, on the host performance counter:
 *   - the game thread: each drawn frame (Graphics_HandleMessage message 8,
 *     begin and end), Game_Update, every KickOff (time waited, segments out
 *     at entry) and every wait of D3D's for a GPU time (0x12CB40);
 *   - the NV2A thread: every segment walked (and how long), each back-end
 *     flip (and its present), flip holds, and the GPU's completion of each
 *     flip where the back end measures it (BW_GPU_LATENCY).
 * and, unless BW_FRAMELOG_SAMPLE=0, samples both threads' call stacks every
 * BW_FRAMELOG_SAMPLE microseconds (default 500): the sampler suspends the
 * thread for a moment and unwinds its host stack (each guest call is a host
 * call, so the exe frames read as the game's own call chain; the driver's
 * and the system's frames are kept too). At the end of the run (bw_fl_dump,
 * from the run limit or a crash) it writes the events, the samples and one
 * symbol line per distinct address as text; the grouping (long frames
 * against all frames, percentiles) is done offline by
 * tools/bw_framelog.py. Up to BW_FRAMELOG_MAXEV events (4 million) and
 * 2.5 minutes of samples are kept.
 *
 * Nothing here reads or writes guest state: it only reads the clock and the
 * threads' host registers and stacks.
 */
#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_framelog.h"

int g_bw_fl_on;

typedef struct {
    int64_t t;
    uint32_t type, a, b, c;
    uint32_t tid;
} FlEv;

static FlEv *s_ev;
static volatile LONG s_nev;
static LONG s_maxev;
static char s_path[MAX_PATH];
static LARGE_INTEGER s_freq;
static volatile LONG s_dumped;

/* ---- the sampler ---- */
#define FL_DEPTH 16
typedef struct {
    int64_t t;
    uint64_t leaf;              /* the sampled instruction pointer */
    uint64_t pc[FL_DEPTH];      /* the frames' return addresses, innermost first (any module) */
    uint8_t role, n;
} FlSample;

static FlSample *s_smp;
static volatile LONG s_nsmp;
static LONG s_maxsmp;
static HANDLE s_th[2];
static DWORD s_tid[2];
static volatile LONG s_run;
static unsigned s_sample_us = 500;
static uint64_t s_img_lo, s_img_hi;

unsigned long long bw_fl_ticks(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned long long)c.QuadPart;
}

unsigned bw_fl_us_since(unsigned long long t0)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    if (!s_freq.QuadPart)
        QueryPerformanceFrequency(&s_freq);
    return (unsigned)((double)((long long)c.QuadPart - (long long)t0) * 1e6 / (double)s_freq.QuadPart);
}

void bw_fl_event_(unsigned type, unsigned a, unsigned b, unsigned c)
{
    LONG i;
    LARGE_INTEGER t;
    if (!s_ev)
        return;
    i = InterlockedIncrement(&s_nev) - 1;
    if (i >= s_maxev)
        return;
    QueryPerformanceCounter(&t);
    s_ev[i].t = t.QuadPart;
    s_ev[i].type = type;
    s_ev[i].a = a;
    s_ev[i].b = b;
    s_ev[i].c = c;
    s_ev[i].tid = GetCurrentThreadId();
}

static void sample_one(int role)
{
    CONTEXT ctx;
    LONG i;
    FlSample *s;
    LARGE_INTEGER t;
    if (!s_th[role] || SuspendThread(s_th[role]) == (DWORD)-1)
        return;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(s_th[role], &ctx)) {
        ResumeThread(s_th[role]);
        return;
    }
    i = InterlockedIncrement(&s_nsmp) - 1;
    if (i >= s_maxsmp) {
        ResumeThread(s_th[role]);
        return;
    }
    s = &s_smp[i];
    QueryPerformanceCounter(&t);
    s->t = t.QuadPart;
    s->leaf = ctx.Rip;
    s->role = (uint8_t)role;
    s->n = 0;
    {
        CONTEXT c = ctx;
        int depth;
        for (depth = 0; depth < 48 && c.Rip && s->n < FL_DEPTH; depth++) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn;
            if (depth)
                s->pc[s->n++] = c.Rip;
            fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
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
        }
    }
    ResumeThread(s_th[role]);
}

static DWORD WINAPI sampler(LPVOID arg)
{
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                          TIMER_ALL_ACCESS);
    (void)arg;
    while (InterlockedCompareExchange(&s_run, 1, 1)) {
        LARGE_INTEGER due;
        sample_one(0);
        sample_one(1);
        due.QuadPart = -(LONGLONG)s_sample_us * 10;
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, 20);
        else
            Sleep(1);
    }
    return 0;
}

void bw_fl_thread(int role)
{
    DWORD me = GetCurrentThreadId();
    if (!g_bw_fl_on || role < 0 || role > 1 || s_tid[role] == me)
        return;
    if (s_th[role])
        CloseHandle(s_th[role]);
    s_th[role] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                            FALSE, me);
    s_tid[role] = me;
}

void bw_fl_init(void)
{
    const char *e = getenv("BW_FRAMELOG");
    HMODULE m = GetModuleHandleA(NULL);
    if (!e || !*e)
        return;
    snprintf(s_path, sizeof s_path, "%s", e);
    QueryPerformanceFrequency(&s_freq);
    s_maxev = (e = getenv("BW_FRAMELOG_MAXEV")) && *e ? atol(e) : 4000000;
    s_ev = (FlEv *)calloc((size_t)s_maxev, sizeof(FlEv));
    if (!s_ev)
        return;
    if ((e = getenv("BW_FRAMELOG_SAMPLE")) && *e)
        s_sample_us = (unsigned)atol(e);
    if (m) {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)m;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const uint8_t *)m + dos->e_lfanew);
        s_img_lo = (uint64_t)(uintptr_t)m;
        s_img_hi = s_img_lo + nt->OptionalHeader.SizeOfImage;
    }
    g_bw_fl_on = 1;
    if (s_sample_us) {
        s_maxsmp = 2 * (LONG)(1000000u / (s_sample_us < 100 ? 100 : s_sample_us)) * 150;  /* 2.5 min */
        s_smp = (FlSample *)calloc((size_t)s_maxsmp, sizeof(FlSample));
        if (s_smp) {
            HANDLE h;
            s_run = 1;
            h = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
            if (h) {
                SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
                CloseHandle(h);
            } else
                s_run = 0;
        }
    }
    fprintf(stderr, "[BW] frame log on -> %s (stacks every %u us)\n", s_path, s_sample_us);
}

/* ---- the report ----
 * Written raw, aggregated offline (tools/bw_framelog.py): the events, the
 * samples (time, thread, leaf, exe frames as image offsets) and one symbol
 * line per distinct address. */

static double ms_of(int64_t t) { return (double)t * 1000.0 / (double)s_freq.QuadPart; }

static void sym_of(uint64_t addr, char *out, size_t n)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    IMAGEHLP_MODULE64 mi;
    HANDLE proc = GetCurrentProcess();
    memset(buf, 0, sizeof buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    mi.SizeOfStruct = sizeof mi;
    if (SymFromAddr(proc, addr, &disp, si)) {
        if (SymGetModuleInfo64(proc, addr, &mi) && addr - mi.BaseOfImage != addr - s_img_lo)
            snprintf(out, n, "%s!%s", mi.ModuleName, si->Name);
        else
            snprintf(out, n, "%s", si->Name);
    } else if (SymGetModuleInfo64(proc, addr, &mi))
        snprintf(out, n, "%s!+0x%llx", mi.ModuleName, (unsigned long long)(addr - mi.BaseOfImage));
    else
        snprintf(out, n, "0x%llx", (unsigned long long)addr);
}

#define FL_SYMS (1u << 16)
static uint64_t *s_symset;

static void sym_note(FILE *f, uint64_t addr)
{
    uint32_t h = (uint32_t)((addr * 0x9E3779B97F4A7C15ull) >> 48), i;
    char nm[300];
    for (i = 0; i < FL_SYMS; i++, h = (h + 1) & (FL_SYMS - 1)) {
        if (s_symset[h] == addr + 1)
            return;
        if (!s_symset[h]) {
            s_symset[h] = addr + 1;
            sym_of(addr, nm, sizeof nm);
            fprintf(f, "Y %llx %s\n", (unsigned long long)addr, nm);
            return;
        }
    }
}

void bw_fl_dump(const char *why)
{
    FILE *f;
    LONG n, k, ns;
    if (!g_bw_fl_on || InterlockedExchange(&s_dumped, 1))
        return;
    InterlockedExchange(&s_run, 0);
    Sleep(5);
    n = s_nev < s_maxev ? s_nev : s_maxev;
    ns = s_nsmp < s_maxsmp ? s_nsmp : s_maxsmp;
    f = fopen(s_path, "w");
    if (!f)
        return;
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    fprintf(f, "# BW_FRAMELOG (%s): %ld events (%ld dropped), %ld stack samples every %u us;"
               " t in ms on the host counter\n",
            why, (long)n, (long)(s_nev - n), (long)ns, s_sample_us);
    fprintf(f, "# game thread %lu, NV2A thread %lu, image base %llx\n", (unsigned long)s_tid[0],
            (unsigned long)s_tid[1], (unsigned long long)s_img_lo);
    fprintf(f, "# E t thread type a b c; types: 1 frame 2 frame_end(steps) 3 upd 4 upd_end"
               " 5 kick(us,out,seq) 6 bot(us,fence,gputime) 10 walk(put,us,get) 11 flip(us,presented)"
               " 12 hold 13 hold_end(us) 14 gpudone(flip,us,inflight) 15 gpuwait(us,inflight)\n");
    fprintf(f, "# S t role leaf frames... (role 0 game, 1 NV2A; leaf and frames as absolute hex addresses,"
               " frames innermost first); Y address symbol\n");
    for (k = 0; k < n; k++) {
        const FlEv *e = &s_ev[k];
        fprintf(f, "E %.3f %c %u %u %u %u\n", ms_of(e->t), e->tid == s_tid[0] ? 'G' : e->tid == s_tid[1] ? 'N' : '?',
                e->type, e->a, e->b, e->c);
    }
    if (s_smp && ns) {
        HANDLE proc = GetCurrentProcess();
        s_symset = (uint64_t *)calloc(FL_SYMS, sizeof(uint64_t));
        for (k = 0; k < ns; k++) {
            const FlSample *s = &s_smp[k];
            int j;
            fprintf(f, "S %.3f %u %llx", ms_of(s->t), s->role, (unsigned long long)s->leaf);
            for (j = 0; j < s->n; j++)
                fprintf(f, " %llx", (unsigned long long)s->pc[j]);
            fputc('\n', f);
        }
        if (s_symset) {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(proc, NULL, TRUE);
            SymRefreshModuleList(proc);
            for (k = 0; k < ns; k++) {
                const FlSample *s = &s_smp[k];
                int j;
                sym_note(f, s->leaf);
                for (j = 0; j < s->n; j++)
                    sym_note(f, s->pc[j]);
            }
            free(s_symset);
        }
    }
    fclose(f);
    fprintf(stderr, "[BW] frame log written: %s\n", s_path);
}
