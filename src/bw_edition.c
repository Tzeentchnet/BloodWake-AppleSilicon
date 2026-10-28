/*
 * bw_edition.c - the edition switch (D-007, D-015). Design: agent R5,
 * docs/remaster/editions.md; wired in by D5 (docs/day1/D5.md).
 *
 * Original must stay exactly as shipped, so the design keeps Original as the
 * zero state: every fix flag starts at 0, the edition starts at Original,
 * and in Original this file writes no guest memory at all and leaves the
 * save folder exactly as the toolkit picks it. The only things it does there
 * are log one line and (with the XBE buffer) count image bytes.
 */
#include <windows.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bw_edition.h"

extern ptrdiff_t g_xbox_mem_offset;

#define BW_GUEST(va) ((uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* ── The registry ──────────────────────────────────────────────────────── */

const bw_fix_info_t g_bw_fixes[BW_FIX_COUNT + 1] = {
#define BW_FIX(id, editions, dflt, kind, logref, desc) { #id, editions, dflt, kind, logref, desc },
#include "bw_fixes.def"
#undef BW_FIX
    { NULL, 0, 0, BW_FIXKIND_IMAGE, NULL, NULL }
};

unsigned char g_bw_fix_on[BW_FIX_COUNT + 1];         /* zero: Original */

static bw_edition_t s_edition = BW_ED_ORIGINAL;
static const char *s_from = "default";
static volatile long s_hits[BW_FIX_COUNT + 1];
static unsigned char s_refused[BW_FIX_COUNT + 1];

static const char *const s_names[BW_ED_COUNT]  = { "original", "remastered" };
static const char *const s_titles[BW_ED_COUNT] = { "Original", "Remastered" };
#define BW_ED_UNKNOWN "BW_EDITION=%s is not an edition in this build (original, remastered)"

const char *bw_edition_name(bw_edition_t e)
{
    return (unsigned)e < BW_ED_COUNT ? s_names[e] : "?";
}

const char *bw_edition_title(bw_edition_t e)
{
    return (unsigned)e < BW_ED_COUNT ? s_titles[e] : "?";
}

bw_edition_t bw_edition(void)
{
    return s_edition;
}

int bw_edition_parse(const char *s, bw_edition_t *out)
{
    int i;
    if (!s || !*s) {
        *out = BW_ED_ORIGINAL;
        return 0;
    }
    for (i = 0; i < BW_ED_COUNT; i++)
        if (!_stricmp(s, s_names[i])) {
            *out = (bw_edition_t)i;
            return 0;
        }
    return -1;
}

static int find_fix(const char *name, size_t n)
{
    int i;
    for (i = 0; i < BW_FIX_COUNT; i++)
        if (strlen(g_bw_fixes[i].name) == n && !_strnicmp(g_bw_fixes[i].name, name, n))
            return i;
    return -1;
}

int bw_fix_enabled_name(const char *name)
{
    int i = name ? find_fix(name, strlen(name)) : -1;
    return i >= 0 ? g_bw_fix_on[i] : 0;
}

/* BW_FIX: all, none, +ID, -ID, ID. Only fixes registered for this edition. */
static int apply_overrides(const char *p, char *err, size_t errlen)
{
    while (p && *p) {
        const char *tok;
        size_t n;
        int on = 1, i;
        while (*p == ',' || *p == ' ')
            p++;
        if (!*p)
            break;
        if (*p == '+' || *p == '-')
            on = (*p++ == '+');
        tok = p;
        while (*p && *p != ',' && *p != ' ')
            p++;
        n = (size_t)(p - tok);
        if ((n == 3 && !_strnicmp(tok, "all", 3)) || (n == 4 && !_strnicmp(tok, "none", 4))) {
            int all = (n == 3);
            for (i = 0; i < BW_FIX_COUNT; i++)
                if (g_bw_fixes[i].editions & BW_ED_BIT(s_edition))
                    g_bw_fix_on[i] = (unsigned char)(all ? on : !on);
            continue;
        }
        i = find_fix(tok, n);
        if (i < 0) {
            _snprintf(err, errlen, "BW_FIX: no fix named '%.*s' (see src/bw_fixes.def)", (int)n, tok);
            return -1;
        }
        if (!(g_bw_fixes[i].editions & BW_ED_BIT(s_edition))) {
            _snprintf(err, errlen, "BW_FIX: %s is not a %s fix", g_bw_fixes[i].name,
                      s_titles[s_edition]);
            return -1;
        }
        g_bw_fix_on[i] = (unsigned char)on;
    }
    return 0;
}

int bw_edition_init(char *err, size_t errlen)
{
    const char *e = getenv("BW_EDITION");
    const char *ov = getenv("BW_FIX");
    int i, on = 0;

    if (err && errlen)
        err[0] = 0;
    memset(g_bw_fix_on, 0, sizeof g_bw_fix_on);
    memset(s_refused, 0, sizeof s_refused);
    s_edition = BW_ED_ORIGINAL;
    if (bw_edition_parse(e, &s_edition) != 0) {
        _snprintf(err, errlen, BW_ED_UNKNOWN, e);
        err[errlen - 1] = 0;
        s_edition = BW_ED_ORIGINAL;
        return -1;
    }
    s_from = (e && *e) ? "BW_EDITION" : "default";

    if (s_edition == BW_ED_ORIGINAL) {
        if (ov && *ov)
            fprintf(stderr, "[ED] BW_FIX=%s ignored: Original is exactly as shipped\n", ov);
        fprintf(stderr, "[ED] edition original (%s): as shipped, 0 of %d registered changes on\n",
                s_from, (int)BW_FIX_COUNT);
        fflush(stderr);
        return 0;
    }

    for (i = 0; i < BW_FIX_COUNT; i++)
        if (g_bw_fixes[i].editions & BW_ED_BIT(s_edition))
            g_bw_fix_on[i] = (unsigned char)(g_bw_fixes[i].dflt != 0);
    if (ov && *ov && apply_overrides(ov, err, errlen) != 0) {
        err[errlen - 1] = 0;
        memset(g_bw_fix_on, 0, sizeof g_bw_fix_on);   /* refuse whole: start nothing */
        return -1;
    }

    for (i = 0; i < BW_FIX_COUNT; i++)
        on += g_bw_fix_on[i];
    fprintf(stderr, "[ED] edition %s (%s%s%s): %d of %d registered changes on:", s_names[s_edition],
            s_from, ov && *ov ? ", BW_FIX=" : "", ov && *ov ? ov : "", on, (int)BW_FIX_COUNT);
    for (i = 0; i < BW_FIX_COUNT; i++)
        if (g_bw_fix_on[i])
            fprintf(stderr, " %s", g_bw_fixes[i].name);
    fprintf(stderr, "\n");
    fflush(stderr);
    return 0;
}

/* ── IMAGE fixes: bytes of the loaded XBE ──────────────────────────────── */

typedef struct {
    bw_fix_id_t    fix;
    uint32_t       va;
    uint32_t       len;
    const uint8_t *expect;    /* what the shipped XBE has there */
    const uint8_t *with;
} bw_image_patch_t;

/* BOAT_SPLASH_RANGE. BoatSound_ShouldPlaySplash (0x13180) computes
 * v = sqrt(r.x^2 + r.y^2) of world-matrix row +0x10 (its z factor is the 0.0
 * at 0x174820, seen), then returns true only if v >= [0x174948] (+0.7071)
 * and v <= [0x174944] (-0.7071): never, for a real v. 0x174944 is read by
 * that one instruction (fcomp at 0x131F4) and by nothing else in the image
 * (byte scan of the XBE and of the generated C, 2026-10-26). FLT_MAX makes
 * the second test always pass: splash iff v >= 0.7071 (and, as before, a NaN
 * or a boat without a render item). */
static const uint8_t k_splash_expect[4] = { 0x81, 0x04, 0x35, 0xBF };   /* -0.7071f */
static const uint8_t k_splash_with[4]   = { 0xFF, 0xFF, 0x7F, 0x7F };   /* FLT_MAX  */

/* SOUND_SHIELDHITS_NAME. The only reference to 0x174B34 is the push at
 * 0x162F5 (Audio_WeaponSoundsAndDragonClawInit -> Audio_FindWave); nothing
 * points inside the string. Same length, one byte differs (+0x10). */
static const uint8_t k_shield_expect[] = "ShieldHitsDreadnought";            /* 22 bytes with NUL */
static const uint8_t k_shield_with[]   = "ShieldHitsDreadnaught";

static const bw_image_patch_t s_image_patches[] = {
    { BW_FIX_BOAT_SPLASH_RANGE,     0x00174944u, 4, k_splash_expect, k_splash_with },
    { BW_FIX_SOUND_SHIELDHITS_NAME, 0x00174B34u, sizeof k_shield_expect,
      k_shield_expect, k_shield_with },
};
#define BW_IMAGE_PATCHES ((int)(sizeof s_image_patches / sizeof s_image_patches[0]))

/* .rdata and .data bytes in guest memory that differ from the XBE file.
 * -1 if the buffer can't be read as an XBE. */
static long image_diff(const uint8_t *x, size_t n)
{
    uint32_t base, count, hdrs, i;
    long diff = 0;
    if (!x || n < 0x180 || memcmp(x, "XBEH", 4))
        return -1;
    base  = *(const uint32_t *)(x + 0x104);
    count = *(const uint32_t *)(x + 0x11C);
    hdrs  = *(const uint32_t *)(x + 0x120) - base;
    if (hdrs > n || count > 64 || hdrs + count * 0x38u > n)
        return -1;
    for (i = 0; i < count; i++) {
        const uint8_t *h = x + hdrs + i * 0x38u;
        uint32_t va = *(const uint32_t *)(h + 4), vsize = *(const uint32_t *)(h + 8);
        uint32_t raw = *(const uint32_t *)(h + 12), rsize = *(const uint32_t *)(h + 16);
        uint32_t name = *(const uint32_t *)(h + 20) - base, len, k;
        const uint8_t *g;
        if (name + 6 > n || raw > n || rsize > n - raw)
            continue;
        if (strcmp((const char *)x + name, ".rdata") && strcmp((const char *)x + name, ".data"))
            continue;
        len = rsize < vsize ? rsize : vsize;
        g = BW_GUEST(va);
        for (k = 0; k < len; k++)
            diff += g[k] != x[raw + k];
    }
    return diff;
}

void bw_edition_apply_image_fixes(const void *xbe, size_t xbe_size)
{
    long before = image_diff((const uint8_t *)xbe, xbe_size), after, expected = 0;
    int f, p;

    for (f = 0; f < BW_FIX_COUNT; f++) {
        int ok = 1, any = 0;
        if (!g_bw_fix_on[f] || g_bw_fixes[f].kind != BW_FIXKIND_IMAGE)
            continue;
        for (p = 0; p < BW_IMAGE_PATCHES; p++) {      /* check every byte first */
            const bw_image_patch_t *ip = &s_image_patches[p];
            if ((int)ip->fix != f)
                continue;
            any = 1;
            if (memcmp(BW_GUEST(ip->va), ip->expect, ip->len)) {
                ok = 0;
                fprintf(stderr, "[ED] fix %s refused: the bytes at 0x%08X are not the shipped ones\n",
                        g_bw_fixes[f].name, ip->va);
            }
        }
        if (!any) {
            ok = 0;
            fprintf(stderr, "[ED] fix %s refused: no image patch registered for it\n",
                    g_bw_fixes[f].name);
        }
        if (!ok) {                                      /* all or nothing */
            g_bw_fix_on[f] = 0;
            s_refused[f] = 1;
            continue;
        }
        for (p = 0; p < BW_IMAGE_PATCHES; p++) {
            const bw_image_patch_t *ip = &s_image_patches[p];
            uint32_t k;
            if ((int)ip->fix != f)
                continue;
            for (k = 0; k < ip->len; k++)
                expected += ip->expect[k] != ip->with[k];
            memcpy(BW_GUEST(ip->va), ip->with, ip->len);
            fprintf(stderr, "[ED] fix %s applied: %u bytes at 0x%08X\n", g_bw_fixes[f].name,
                    ip->len, ip->va);
        }
    }
    after = image_diff((const uint8_t *)xbe, xbe_size);
    if (before >= 0)
        fprintf(stderr, "[ED] image check (%s): .rdata/.data differ from the XBE in %ld bytes "
                "before the fixes and %ld after (fixes account for %ld)%s\n",
                s_names[s_edition], before, after, expected,
                after - before == expected ? "" : "  ** MISMATCH **");
    fflush(stderr);
}

/* ── Saves ───────────────────────────────────────────────────────────────
 *
 * One save disk per edition (D-015; lead's call 2026-10-27):
 *   Original    %LOCALAPPDATA%\xboxrecomp                  (as always; untouched)
 *   Remastered  %LOCALAPPDATA%\xboxrecomp\editions\remastered
 *   (and so on: <root>\editions\<name> for every edition after Original)
 * Each holds its own UDATA (saves), TDATA (settings), Z: cache and partition
 * images. Original keeps the root, so its saves are never moved; the
 * "editions" folder beside them is invisible to the guest (only UDATA, TDATA,
 * CACHE, Cache and Partition*.img are mapped). On an edition's first boot
 * (no marker file and no UDATA in its folder) Original's UDATA and TDATA are
 * copied in: a plain one-way file copy between our own folders, through a
 * temporary folder renamed into place, so a second process starting at the
 * same moment can't see half a copy. */

static int dir_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int copy_tree(const char *from, const char *to, int *files)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    int ok = 1;
    if (!CreateDirectoryA(to, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    _snprintf(pat, sizeof pat, "%s\\*", from);
    pat[sizeof pat - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        _snprintf(src, sizeof src, "%s\\%s", from, fd.cFileName);
        _snprintf(dst, sizeof dst, "%s\\%s", to, fd.cFileName);
        src[sizeof src - 1] = dst[sizeof dst - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok &= copy_tree(src, dst, files);
        else if (CopyFileA(src, dst, TRUE))
            (*files)++;
        else
            ok = 0;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

static void remove_tree(const char *p)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[MAX_PATH], sub[MAX_PATH];
    _snprintf(pat, sizeof pat, "%s\\*", p);
    pat[sizeof pat - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            _snprintf(sub, sizeof sub, "%s\\%s", p, fd.cFileName);
            sub[sizeof sub - 1] = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(sub);
            else
                DeleteFileA(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(p);
}

/* Copy <from>\UDATA and <from>\TDATA into <to>, once. */
static void import_original(const char *from, const char *to)
{
    static const char *subs[2] = { "UDATA", "TDATA" };
    char marker[MAX_PATH], dst[MAX_PATH], tmp[MAX_PATH], src[MAX_PATH];
    int k, files = 0, moved = 0;
    FILE *f;

    _snprintf(marker, sizeof marker, "%s\\bw_edition_saves.txt", to);
    _snprintf(dst, sizeof dst, "%s\\UDATA", to);
    marker[sizeof marker - 1] = dst[sizeof dst - 1] = 0;
    if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES || dir_exists(dst)) {
        fprintf(stderr, "[ED] saves: %s (this edition's own)\n", to);
        return;
    }
    for (k = 0; k < 2; k++) {
        _snprintf(src, sizeof src, "%s\\%s", from, subs[k]);
        _snprintf(dst, sizeof dst, "%s\\%s", to, subs[k]);
        _snprintf(tmp, sizeof tmp, "%s\\%s.import-%lu", to, subs[k],
                  (unsigned long)GetCurrentProcessId());
        src[sizeof src - 1] = dst[sizeof dst - 1] = tmp[sizeof tmp - 1] = 0;
        if (!dir_exists(src) || dir_exists(dst))
            continue;
        if (copy_tree(src, tmp, &files) && MoveFileA(tmp, dst))
            moved++;
        else
            remove_tree(tmp);                 /* someone else won, or a copy failed */
    }
    f = fopen(marker, "w");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "Blood Wake %s saves. First boot %04u-%02u-%02u %02u:%02u:%02u.\n"
                   "Imported from Original (one-way copy, Original untouched): %s, %d files, "
                   "%d folders.\n", s_titles[s_edition], st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, from, files, moved);
        fclose(f);
    }
    fprintf(stderr, "[ED] saves: first %s boot: copied Original's saves from %s into %s "
            "(%d files; Original's folder only read)\n", s_titles[s_edition], from, to, files);
}

const char *bw_edition_prepare_saves(void)
{
    static char root[MAX_PATH];
    static char user_env[MAX_PATH + 32];
    char base[MAX_PATH], orig_user[MAX_PATH], user[MAX_PATH];
    const char *ov = getenv("BW_EDITION_SAVE_ROOT");
    const char *rs = getenv("RECOMP_SAVE_DIR");

    if (s_edition == BW_ED_ORIGINAL) {
        fprintf(stderr, "[ED] saves: Original's folder, as before the switch (%s)\n",
                rs && *rs ? rs : "%LOCALAPPDATA%\\xboxrecomp");
        return NULL;                  /* the toolkit's default, exactly as today */
    }
    if (ov && *ov)
        _snprintf(base, sizeof base, "%s", ov);
    else if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        strncat(base, "\\xboxrecomp", sizeof base - strlen(base) - 1);
    else
        _snprintf(base, sizeof base, "SaveData");
    base[sizeof base - 1] = 0;
    _snprintf(root, sizeof root, "%s\\editions\\%s", base, s_names[s_edition]);
    root[sizeof root - 1] = 0;
    SHCreateDirectoryExA(NULL, root, NULL);

    /* Where Original keeps UDATA/TDATA, and where this edition will. */
    if (rs && *rs) {
        _snprintf(orig_user, sizeof orig_user, "%s", rs);
        _snprintf(user, sizeof user, "%s\\editions\\%s", rs, s_names[s_edition]);
        orig_user[sizeof orig_user - 1] = user[sizeof user - 1] = 0;
        SHCreateDirectoryExA(NULL, user, NULL);
        _snprintf(user_env, sizeof user_env, "%s", user);
        user_env[sizeof user_env - 1] = 0;
        _putenv_s("RECOMP_SAVE_DIR", user_env);   /* read by xbox_path_init */
    } else {
        char la[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la)))
            _snprintf(orig_user, sizeof orig_user, "%s\\xboxrecomp", la);
        else
            _snprintf(orig_user, sizeof orig_user, "SaveData");
        _snprintf(user, sizeof user, "%s", root);
        orig_user[sizeof orig_user - 1] = user[sizeof user - 1] = 0;
    }
    import_original(orig_user, user);
    fprintf(stderr, "[ED] saves: %s disk %s (saves and settings in %s)\n", s_titles[s_edition],
            root, user);
    fflush(stderr);
    return root;
}

/* ── Counters and the summary ──────────────────────────────────────────── */

void bw_fix_note_hit(bw_fix_id_t id)
{
    if ((unsigned)id < (unsigned)BW_FIX_COUNT)
        InterlockedIncrement(&s_hits[id]);
}

static void (*s_summaries[8])(void);
static int s_nsum;

void bw_edition_add_summary(void (*fn)(void))
{
    int i;
    for (i = 0; i < s_nsum; i++)
        if (s_summaries[i] == fn)
            return;
    if (s_nsum < 8)
        s_summaries[s_nsum++] = fn;
}

void bw_edition_summary(void)
{
    static volatile long done;
    int i;
    if (InterlockedExchange(&done, 1))
        return;
    for (i = 0; i < BW_FIX_COUNT; i++)
        fprintf(stderr, "[ED] summary %s: %s %s, %ld hits\n", s_names[s_edition],
                g_bw_fixes[i].name, s_refused[i] ? "REFUSED" : g_bw_fix_on[i] ? "on" : "off",
                s_hits[i]);
    for (i = 0; i < s_nsum; i++)
        s_summaries[i]();
    fflush(stderr);
}
