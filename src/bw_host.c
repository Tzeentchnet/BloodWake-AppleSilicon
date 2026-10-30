/* Blood Wake: the play launcher's host window.
 *
 * Blood Wake reboots itself between the title, the attract demo and a match
 * (it relaunches its own default.xbe with launch data; see sub_000DB8B9 in
 * recomp_manual.c). The port ends the process there and the next boot is a
 * new one, so each reboot used to open a new window.
 *
 * With BW_HOST=1 (tools/play.py sets it), this process runs no game. It opens
 * one "Blood Wake" window and runs each boot as a child process of the same
 * exe, with BW_PARENT_HWND naming the window. The toolkit's frame window
 * (carried fb_present.c) is then a child filling it. Between boots the window
 * is black, as the console's screen is during a reboot.
 *
 *   - follows the game's reboots: bw_relaunch.bin -> bw_launch_next.bin ->
 *     BW_LAUNCH_DATA_FILE for the next boot, as tools/play.py did;
 *   - Alt+Enter / F11: borderless fullscreen (the game's window forwards them);
 *   - closing the window closes the game (WM_CLOSE to its frame window,
 *     which ends that process through the toolkit's exit hook);
 *   - the boots' output goes to play.log (BW_HOST_LOG), one header per boot;
 *   - if the game stops without asking to reboot, the window says so and
 *     stays until closed (BW_HOST_LINGER=0: exit at once, for scripts);
 *   - BW_HOST_BOOTS (default 200) caps the boots.
 *
 * The child inherits everything else in the environment. */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bw_host.h"
#include "bw_edition.h"
#include "bw_widescreen.h"

#define BW_HOST_FULLSCREEN (WM_APP + 0x42)   /* from the carried fb_present.c */
#define BW_FRAME_CLASS     "XboxRecompFramebuffer"

static HWND s_host;
static HANDLE s_child;
static int s_quit;
static DWORD s_quit_at;
static char s_msg[512];

static HWND child_window(void)
{
    return s_host ? FindWindowExA(s_host, NULL, BW_FRAME_CLASS, NULL) : NULL;
}

static void fit_child(void)
{
    HWND c = child_window();
    RECT r;
    if (!c)
        return;
    GetClientRect(s_host, &r);
    SetWindowPos(c, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

/* Borderless fullscreen on the window's monitor, and back (as the toolkit's
 * own window does it). */
static void toggle_fullscreen(void)
{
    static int full;
    static RECT saved;
    static LONG_PTR saved_style;
    HWND h = s_host;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
}

static void request_quit(void)
{
    HWND c = child_window();
    if (!s_quit) {
        s_quit = 1;
        s_quit_at = GetTickCount();
    }
    if (c)
        PostMessageA(c, WM_CLOSE, 0, 0);
    else if (s_child)
        TerminateProcess(s_child, 0);
}

static LRESULT CALLBACK host_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case BW_HOST_FULLSCREEN:
        toggle_fullscreen();
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {
            if (!(l & (1 << 30)))
                toggle_fullscreen();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) {
            toggle_fullscreen();
            return 0;
        }
        break;
    case WM_SIZE:
        fit_child();
        return 0;
    case WM_SETFOCUS: {
        HWND c = child_window();
        if (c)
            SetFocus(c);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (s_msg[0]) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(220, 220, 220));
            InflateRect(&r, -24, -24);
            DrawTextA(dc, s_msg, -1, &r, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        if (s_child)
            request_quit();
        else
            s_quit = 1, DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        s_host = NULL;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static int env_int(const char *name, int dflt)
{
    const char *e = getenv(name);
    return e && *e ? atoi(e) : dflt;
}

int bw_host_wanted(void)
{
    const char *e = getenv("BW_HOST");
    return e && e[0] == '1';
}

static HWND make_window(const char *title, float aspect)
{
    WNDCLASSA wc;
    RECT r, work;
    int scale = env_int("RECOMP_WINDOW_SCALE", 2);
    int fw = aspect > 4.0f / 3.0f + 0.001f ? (int)(480.0f * aspect + 0.5f) : 640;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = host_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "BloodWakeHost";
    RegisterClassA(&wc);
    if (scale < 1 || scale > 8)
        scale = 2;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = work.top = 0;
        work.right = 1920;
        work.bottom = 1080;
    }
    /* The game's frame is 640x480 (Remastered widescreen: 480 high at the
     * picture's aspect); the same sizing as the toolkit's window. */
    for (;; scale--) {
        r.left = r.top = 0;
        r.right = fw * scale;
        r.bottom = 480 * scale;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (scale <= 1 || (r.right - r.left <= work.right - work.left
                           && r.bottom - r.top <= work.bottom - work.top))
            break;
    }
    return CreateWindowExA(0, "BloodWakeHost", title,
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
                           work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                           work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void log_line(HANDLE log, const char *text)
{
    DWORD n;
    if (log != INVALID_HANDLE_VALUE)
        WriteFile(log, text, (DWORD)strlen(text), &n, NULL);
}

int bw_host_main(void)
{
    char exe[MAX_PATH], cmd[MAX_PATH + 4], line[600], next[MAX_PATH];
    const char *log_name = getenv("BW_HOST_LOG");
    int boots = env_int("BW_HOST_BOOTS", 200), linger = env_int("BW_HOST_LINGER", 1);
    int boot, result = 0;
    SECURITY_ATTRIBUTES sa;
    HANDLE log, nul;

    GetModuleFileNameA(NULL, exe, sizeof exe);
    _snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    cmd[sizeof cmd - 1] = 0;
    GetFullPathNameA("bw_launch_next.bin", sizeof next, next, NULL);
    if (!log_name || !*log_name)
        log_name = "play.log";

    /* The edition (D-015): every boot inherits BW_EDITION from this
     * process's environment. Check it here once, so a wrong name says so in
     * the window instead of failing boot after boot. Original keeps the
     * plain title. */
    {
        bw_edition_t ed;
        static char title[64];
        char err[256];
        int bad = bw_edition_init(err, sizeof err) != 0;   /* BW_EDITION and BW_FIX */
        ed = bw_edition();
        if (!bad)
            bw_ws_init(0, 0);           /* the window's shape (Remastered widescreen) */
        if (bad || ed == BW_ED_ORIGINAL)
            _snprintf(title, sizeof title, "Blood Wake");
        else
            _snprintf(title, sizeof title, "Blood Wake (%s)", bw_edition_title(ed));
        title[sizeof title - 1] = 0;
        s_host = make_window(title, bw_ws_aspect());
        {
            /* RECOMP_FULLSCREEN=1 (the launcher's "Fullscreen"): start in
             * borderless fullscreen, as the toolkit's own window does. */
            const char *fs = getenv("RECOMP_FULLSCREEN");
            if (s_host && fs && *fs && *fs != '0')
                toggle_fullscreen();
        }
        if (s_host && bad) {
            _snprintf(s_msg, sizeof s_msg, "%s\n\n"
                      "Editions: original or remastered (play.cmd --edition remastered).\n"
                      "Close this window to finish.", err);
            s_msg[sizeof s_msg - 1] = 0;
            boots = 0;                       /* start no boot; the window says why */
        }
    }
    if (!s_host) {
        fprintf(stderr, "[HOST] could not open the window\n");
        return 1;
    }
    {
        char hv[32];
        _snprintf(hv, sizeof hv, "%llu", (unsigned long long)(uintptr_t)s_host);
        hv[sizeof hv - 1] = 0;
        SetEnvironmentVariableA("BW_HOST", NULL);
        SetEnvironmentVariableA("BW_PARENT_HWND", hv);
    }

    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    log = CreateFileA(log_name, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);

    for (boot = 1; boot <= boots && !s_quit; boot++) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        DWORD code = 0;

        DeleteFileA("bw_relaunch.bin");
        _snprintf(line, sizeof line, "\r\n=== [HOST] boot %d%s ===\r\n", boot,
                  boot > 1 ? " (the game's own reboot)" : "");
        line[sizeof line - 1] = 0;
        log_line(log, line);
        printf("Boot %d%s ...\n", boot, boot > 1 ? " (the game's own reboot)" : "");
        fflush(stdout);

        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = log;
        si.hStdError = log;
        if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            _snprintf(s_msg, sizeof s_msg, "Could not start the game (error %lu).",
                      (unsigned long)GetLastError());
            result = 1;
            break;
        }
        CloseHandle(pi.hThread);
        s_child = pi.hProcess;
        for (;;) {
            DWORD r = MsgWaitForMultipleObjects(1, &s_child, FALSE, s_quit ? 250 : INFINITE,
                                                QS_ALLINPUT);
            if (r == WAIT_OBJECT_0)
                break;
            pump();
            if (s_quit && GetTickCount() - s_quit_at > 5000)
                TerminateProcess(s_child, 0);      /* it didn't close itself */
        }
        GetExitCodeProcess(s_child, &code);
        CloseHandle(s_child);
        s_child = NULL;
        if (s_quit)
            break;
        if (GetFileAttributesA("bw_relaunch.bin") == INVALID_FILE_ATTRIBUTES) {
            /* The game stopped without asking to reboot: a console never
             * does that, so it is a crash or a failure. */
            _snprintf(s_msg, sizeof s_msg,
                      "The game stopped (exit code 0x%08lX) without asking to reboot.\n\n"
                      "Its log is %s in the repo folder.\nClose this window to finish.",
                      (unsigned long)code, log_name);
            s_msg[sizeof s_msg - 1] = 0;
            _snprintf(line, sizeof line, "\r\n=== [HOST] boot %d stopped, exit code 0x%08lX ===\r\n",
                      boot, (unsigned long)code);
            line[sizeof line - 1] = 0;
            log_line(log, line);
            printf("The game exited (code 0x%08lX).\n", (unsigned long)code);
            result = code ? (int)code : 0;
            break;
        }
        /* Keep the request out of the next boot's way: it writes a new one. */
        MoveFileExA("bw_relaunch.bin", next, MOVEFILE_REPLACE_EXISTING);
        SetEnvironmentVariableA("BW_LAUNCH_DATA_FILE", next);
    }
    if (boot > boots && !s_quit && boots > 0)
        _snprintf(s_msg, sizeof s_msg, "Stopped after %d boots (BW_HOST_BOOTS).", boots);

    if (log != INVALID_HANDLE_VALUE)
        CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (s_host && !s_quit && s_msg[0] && linger) {
        SetWindowTextA(s_host, "Blood Wake - stopped");
        InvalidateRect(s_host, NULL, TRUE);
        while (s_host && !s_quit) {
            MSG msg;
            if (GetMessageA(&msg, NULL, 0, 0) <= 0)
                break;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    if (s_host)
        DestroyWindow(s_host);
    return result;
}
#else
#include <stdio.h>
#include <stdlib.h>
#include "bw_host.h"

int bw_host_wanted(void)
{
    const char *e = getenv("BW_HOST");
    return e && e[0] == '1';
}

int bw_host_main(void)
{
    fprintf(stderr, "[HOST] the embedded Windows host is unavailable on this platform; "
                    "use the native boot supervisor\n");
    return 1;
}
#endif
