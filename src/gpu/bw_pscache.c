/*
 * bw_pscache.c - persistent cache of the combiner pixel shaders' bytecode
 * (agent T1, 2026-10-27; every edition, lead's call under D-014).
 *
 * Why: the D3D11 back end turns each new register-combiner state into an
 * HLSL pixel shader (bw_rc_hlsl.c) and compiles it with D3DCompile at O3 on
 * the device thread when a draw first needs it. Each compile takes 15-60 ms,
 * and a mission's first seconds need about 20 (mission 1, seen): the ~100 ms
 * frames at the start of gameplay (runs *-t1-q-seq-on / -off, T1.md "Second
 * task"). With this cache a shader compiles once per machine; later runs load
 * the bytecode.
 *
 * Same pixels: an entry holds exactly the bytes D3DCompile produced for that
 * shader text with the same compiler and flags; BW_GPU_PSCACHE_VERIFY=1
 * compiles on every hit too and compares.
 *
 * Keyed by the generated HLSL text itself (FNV-1a 64 plus a second 64-bit
 * hash and its length), which spells out every part of the combiner key,
 * the Remastered/VFX variant bits and any baked-in setting; and tagged with
 * the compiler (d3dcompiler_47.dll's size and date), target and flags. So a
 * code change, a new compiler or a different setting just misses and
 * compiles.
 *
 * Where: %LOCALAPPDATA%\xboxrecomp\shadercache\ps_<hash>.bin, one file per
 * shader, written to a temporary file and renamed (several copies of the
 * game may run at once). Derived from the game's own combiner state on this
 * machine: never in the repository, never shipped. BW_GPU_PSCACHE=<folder>
 * uses another folder, BW_GPU_PSCACHE=0 turns the cache off. A corrupt,
 * truncated or mismatched file is skipped (the shader compiles).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_pscache.h"

#define PSC_MAGIC 0x53505742u           /* "BWPS" */
#define PSC_VERSION 1u

typedef struct {
    uint32_t magic, version;
    uint64_t tag;                       /* compiler, target, flags */
    uint64_t h1, h2;                    /* the shader text */
    uint32_t textlen, blobsize;
    uint64_t blobsum;
} PscHeader;

typedef struct {
    uint64_t h1, h2;
    uint32_t textlen, size;
    void *blob;
} PscEntry;

static PscEntry *s_ent;
static unsigned s_n, s_cap;
static char s_dir[MAX_PATH];
static int s_on = -1, s_verify = -1;
static uint64_t s_tag;
static unsigned s_loaded, s_skipped, s_hits, s_compiled, s_stored, s_logged;
static double s_compile_ms, s_hit_ms;

static uint64_t fnv64(const void *p, size_t n, uint64_t h)
{
    const uint8_t *q = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ q[i]) * 0x100000001B3ull;
    return h;
}

static uint64_t h_a(const void *p, size_t n) { return fnv64(p, n, 0xCBF29CE484222325ull); }
static uint64_t h_b(const void *p, size_t n) { return fnv64(p, n, 0x84222325CBF29CE4ull) ^ (uint64_t)n << 40; }

/* The compiler's identity, the target and the flags (get_rc_shader's). */
static uint64_t make_tag(void)
{
    char path[MAX_PATH], buf[MAX_PATH + 128];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
    memset(&fa, 0, sizeof fa);
    path[0] = 0;
    if (m && GetModuleFileNameA(m, path, sizeof path))
        GetFileAttributesExA(path, GetFileExInfoStandard, &fa);
    _snprintf(buf, sizeof buf, "bwps%u|ps_5_0|ps_main|O3|%lu|%lu|%lu|%lu", PSC_VERSION,
              (unsigned long)fa.nFileSizeLow, (unsigned long)fa.nFileSizeHigh,
              (unsigned long)fa.ftLastWriteTime.dwLowDateTime,
              (unsigned long)fa.ftLastWriteTime.dwHighDateTime);
    buf[sizeof buf - 1] = 0;
    return h_a(buf, strlen(buf));
}

static int add(uint64_t h1, uint64_t h2, uint32_t textlen, const void *blob, uint32_t size)
{
    void *copy;
    if (s_n == s_cap) {
        unsigned nc = s_cap ? s_cap * 2 : 256;
        PscEntry *ne = (PscEntry *)realloc(s_ent, nc * sizeof *ne);
        if (!ne)
            return 0;
        s_ent = ne;
        s_cap = nc;
    }
    copy = malloc(size);
    if (!copy)
        return 0;
    memcpy(copy, blob, size);
    s_ent[s_n].h1 = h1; s_ent[s_n].h2 = h2;
    s_ent[s_n].textlen = textlen; s_ent[s_n].size = size;
    s_ent[s_n].blob = copy;
    s_n++;
    return 1;
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    PscHeader h;
    void *blob = NULL;
    int ok = 0;
    if (!f)
        return 0;
    if (fread(&h, 1, sizeof h, f) == sizeof h && h.magic == PSC_MAGIC && h.version == PSC_VERSION
            && h.tag == s_tag && h.blobsize > 0 && h.blobsize < (4u << 20)
            && (blob = malloc(h.blobsize)) != NULL
            && fread(blob, 1, h.blobsize, f) == h.blobsize
            && fnv64(blob, h.blobsize, 0xCBF29CE484222325ull) == h.blobsum)
        ok = add(h.h1, h.h2, h.textlen, blob, h.blobsize);
    free(blob);
    fclose(f);
    return ok;
}

void bw_pscache_init(void)
{
    const char *e = getenv("BW_GPU_PSCACHE");
    char pat[MAX_PATH + 16];
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    LARGE_INTEGER t0, t1, fq;
    if (s_on >= 0)
        return;
    s_on = 0;
    if (e && e[0] == '0' && !e[1]) {
        fprintf(stderr, "[PSCACHE] off (BW_GPU_PSCACHE=0): every pixel shader compiles\n");
        return;
    }
    if (e && *e)
        _snprintf(s_dir, sizeof s_dir, "%s", e);
    else if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, s_dir)))
        strncat(s_dir, "\\xboxrecomp\\shadercache", sizeof s_dir - strlen(s_dir) - 1);
    else
        return;
    s_dir[sizeof s_dir - 1] = 0;
    SHCreateDirectoryExA(NULL, s_dir, NULL);
    s_tag = make_tag();
    s_on = 1;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    _snprintf(pat, sizeof pat, "%s\\ps_*.bin", s_dir);
    pat[sizeof pat - 1] = 0;
    fh = FindFirstFileA(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            char path[MAX_PATH * 2];
            _snprintf(path, sizeof path, "%s\\%s", s_dir, fd.cFileName);
            path[sizeof path - 1] = 0;
            if (load_file(path))
                s_loaded++;
            else
                s_skipped++;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    QueryPerformanceCounter(&t1);
    fprintf(stderr, "[PSCACHE] %u pixel shaders loaded from %s (%u skipped: other compiler or"
                    " version, or damaged) in %.1f ms\n", s_loaded, s_dir, s_skipped,
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
}

int bw_pscache_verify(void)
{
    if (s_verify < 0) {
        const char *e = getenv("BW_GPU_PSCACHE_VERIFY");
        s_verify = e && e[0] == '1';
    }
    return s_verify;
}

const void *bw_pscache_find(const char *src, size_t len, size_t *size)
{
    uint64_t h1, h2;
    unsigned i;
    if (s_on <= 0 || !s_n)
        return NULL;
    h1 = h_a(src, len);
    h2 = h_b(src, len);
    for (i = 0; i < s_n; i++)
        if (s_ent[i].h1 == h1 && s_ent[i].h2 == h2 && s_ent[i].textlen == (uint32_t)len) {
            *size = s_ent[i].size;
            return s_ent[i].blob;
        }
    return NULL;
}

void bw_pscache_store(const char *src, size_t len, const void *blob, size_t size)
{
    PscHeader h;
    char tmp[MAX_PATH * 2], fin[MAX_PATH * 2];
    size_t have = 0;
    FILE *f;
    int ok;
    if (s_on <= 0 || !blob || !size || size >= (4u << 20))
        return;
    memset(&h, 0, sizeof h);
    h.magic = PSC_MAGIC;
    h.version = PSC_VERSION;
    h.tag = s_tag;
    h.h1 = h_a(src, len);
    h.h2 = h_b(src, len);
    h.textlen = (uint32_t)len;
    h.blobsize = (uint32_t)size;
    h.blobsum = fnv64(blob, size, 0xCBF29CE484222325ull);
    if (bw_pscache_find(src, len, &have) == NULL)
        add(h.h1, h.h2, h.textlen, blob, h.blobsize);
    _snprintf(fin, sizeof fin, "%s\\ps_%016llX.bin", s_dir, (unsigned long long)h.h1);
    _snprintf(tmp, sizeof tmp, "%s\\ps_%016llX.%lu.%lu.tmp", s_dir, (unsigned long long)h.h1,
              (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    fin[sizeof fin - 1] = 0;
    tmp[sizeof tmp - 1] = 0;
    f = fopen(tmp, "wb");
    if (!f)
        return;
    ok = fwrite(&h, 1, sizeof h, f) == sizeof h && fwrite(blob, 1, h.blobsize, f) == h.blobsize;
    ok = (fclose(f) == 0) && ok;
    if (!ok || !MoveFileExA(tmp, fin, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
    else
        s_stored++;
}

void bw_pscache_note(int compiled, double ms)
{
    if (compiled) {
        s_compiled++;
        s_compile_ms += ms;
    } else {
        s_hits++;
        s_hit_ms += ms;
    }
    if (s_logged++ < 200 || (s_compiled + s_hits) % 100 == 0)
        fprintf(stderr, "[PSCACHE] pixel shader %s in %.1f ms; this run: %u compiled in game (%.0f ms),"
                        " %u from the cache (%.1f ms), %u stored\n",
                compiled ? "compiled" : "from the cache", ms, s_compiled, s_compile_ms, s_hits, s_hit_ms,
                s_stored);
}
