/*
 * Superlite Autoshut - shuts a PC down when nobody touches the keyboard or
 * mouse for a set time. One small exe, no C runtime.
 *
 *   SuperliteAutoshut.exe install [minutes]   install + start the service (admin)
 *   SuperliteAutoshut.exe set <minutes>       change the idle time (admin)
 *   SuperliteAutoshut.exe status              show what is installed
 *   SuperliteAutoshut.exe uninstall           remove everything (admin)
 *
 * Internal modes: "service" (run by Windows) and "helper" (started by the
 * service on the console desktop, as SYSTEM so customers can't end it).
 * The helper reads the session's last-input time, shows the countdown, and
 * exits with AS_EXIT_SHUTDOWN when it reaches 0; the service then shuts down.
 *
 * Build with /DAS_TEST_BUILD for a copy that never shuts down: it writes
 * what it would do to %ProgramData%\Superlite Autoshut\test.log instead.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <intrin.h>
#include "logic.h"

#define SVC_NAME     L"SuperliteAutoshut"
#define REG_KEY      L"SOFTWARE\\Superlite Autoshut"
#define EXE_NAME     L"SuperliteAutoshut.exe"
#define INSTALL_DIR  L"Superlite Autoshut"
#define AS_EXIT_SHUTDOWN 0x5D0Fu
#define AS_VERSION   L"1.0.0"

#ifdef AS_TEST_BUILD
#define SVC_DISPLAY L"Superlite Autoshut (TEST - never shuts down)"
#else
#define SVC_DISPLAY L"Superlite Autoshut"
#endif

/* ---- tiny C runtime replacements ------------------------------------- */
#pragma function(memset)
void *memset(void *d, int c, size_t n) { __stosb((unsigned char *)d, (unsigned char)c, n); return d; }
#pragma function(memcpy)
void *memcpy(void *d, const void *s, size_t n) { __movsb((unsigned char *)d, (const unsigned char *)s, n); return d; }

/* ---- console output --------------------------------------------------- */
static void out(const wchar_t *s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, n;
    if (!h || h == INVALID_HANDLE_VALUE)
        return;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s, (DWORD)lstrlenW(s), &n, NULL);
    } else { /* redirected: write UTF-8 */
        char buf[1024];
        int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof buf, NULL, NULL);
        if (len > 1)
            WriteFile(h, buf, (DWORD)(len - 1), &n, NULL);
    }
}

static void outf(const wchar_t *fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);
    va_end(ap);
    out(buf);
}

/* ---- paths, registry, test log ------------------------------------------ */
static void install_dir(wchar_t *buf, DWORD cap) {
    DWORD n = GetEnvironmentVariableW(L"ProgramW6432", buf, cap);
    if (!n || n >= cap)
        n = GetEnvironmentVariableW(L"ProgramFiles", buf, cap);
    if (!n || n >= cap)
        lstrcpyW(buf, L"C:\\Program Files");
    lstrcatW(buf, L"\\" INSTALL_DIR);
}

static void install_exe(wchar_t *buf, DWORD cap) {
    install_dir(buf, cap);
    lstrcatW(buf, L"\\" EXE_NAME);
}

static DWORD reg_get(const wchar_t *name, DWORD def) {
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v;
    return def;
}

static int reg_set(const wchar_t *name, DWORD v) {
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_KEY, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &k, NULL);
    if (r != ERROR_SUCCESS)
        return 0;
    r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static unsigned saved_minutes(void) {
    DWORD m = reg_get(L"IdleMinutes", AS_DEFAULT_MINUTES);
    return (m >= AS_MIN_MINUTES && m <= AS_MAX_MINUTES) ? m : AS_DEFAULT_MINUTES;
}

#ifdef AS_TEST_BUILD
static void logw(const wchar_t *fmt, ...) {
    wchar_t path[MAX_PATH], line[512];
    char utf8[1024];
    SYSTEMTIME t;
    va_list ap;
    int n;
    HANDLE f;
    DWORD w;
    if (!GetEnvironmentVariableW(L"ProgramData", path, MAX_PATH))
        lstrcpyW(path, L"C:\\ProgramData");
    lstrcatW(path, L"\\" INSTALL_DIR);
    CreateDirectoryW(path, NULL);
    lstrcatW(path, L"\\test.log");
    GetLocalTime(&t);
    n = wsprintfW(line, L"%04u-%02u-%02u %02u:%02u:%02u [pid %lu] ", t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    va_start(ap, fmt);
    n += wvsprintfW(line + n, fmt, ap);
    va_end(ap);
    lstrcatW(line, L"\r\n");
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, NULL, NULL);
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (n > 1)
            WriteFile(f, utf8, (DWORD)(n - 1), &w, NULL);
        CloseHandle(f);
    }
}
#define TLOG(...) logw(__VA_ARGS__)
#else
#define TLOG(...) ((void)0)
#endif

/* ========================================================================
 * Helper: lives on the console desktop, watches for input, shows warning
 * ===================================================================== */
static as_config g_cfg;
static HWND g_wnd;
static unsigned g_secs;
static HFONT g_font_title, g_font_num, g_font_hint;
static int g_dpi = 96;

#define CLR_BG     RGB(0x14, 0x1B, 0x24)
#define CLR_FG     RGB(0xF2, 0xF5, 0xF8)
#define CLR_WARN   RGB(0xF0, 0xA0, 0x20)
#define CLR_HINT   RGB(0xAA, 0xB6, 0xC3)

static int px(int v) { return MulDiv(v, g_dpi, 96); }

static unsigned idle_ms(void) {
    LASTINPUTINFO li;
    li.cbSize = sizeof li;
    li.dwTime = 0;
    if (!GetLastInputInfo(&li))
        return 0; /* can't tell: treat as active, never shut down by mistake */
    return (unsigned)(GetTickCount() - li.dwTime);
}

static HFONT make_font(int size, int weight) {
    return CreateFontW(-px(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    RECT rc, r;
    HDC dc = BeginPaint(h, &ps);
    HBRUSH bg = CreateSolidBrush(CLR_BG), border = CreateSolidBrush(CLR_WARN);
    wchar_t num[16];
    int i, b = px(3);

    GetClientRect(h, &rc);
    FillRect(dc, &rc, bg);
    for (i = 0; i < b; i++) {
        r = rc;
        InflateRect(&r, -i, -i);
        FrameRect(dc, &r, border);
    }
    SetBkMode(dc, TRANSPARENT);

    r = rc; r.top = px(20); r.bottom = px(52);
    SelectObject(dc, g_font_title);
    SetTextColor(dc, CLR_FG);
    DrawTextW(dc, L"No activity detected", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    r = rc; r.top = px(52); r.bottom = px(140);
    SelectObject(dc, g_font_num);
    SetTextColor(dc, CLR_WARN);
    wsprintfW(num, L"%u", g_secs);
    DrawTextW(dc, num, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    r = rc; r.top = px(140); r.bottom = rc.bottom - px(14); r.left += px(20); r.right -= px(20);
    SelectObject(dc, g_font_hint);
    SetTextColor(dc, CLR_HINT);
    DrawTextW(dc, L"This PC will shut down.\nMove the mouse or press any key to cancel.", -1, &r,
              DT_CENTER | DT_WORDBREAK);

    DeleteObject(bg);
    DeleteObject(border);
    EndPaint(h, &ps);
}

static void place_on_top(HWND h, UINT extra) {
    int w = px(460), ht = px(210);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - ht) / 2;
    SetWindowPos(h, HWND_TOPMOST, x, y, w, ht, SWP_NOACTIVATE | extra);
}

static DWORD g_shown_at; /* tick count when the warning appeared */

static void tick(HWND h) {
    unsigned idle = idle_ms(), secs = 0;
    unsigned shown = IsWindowVisible(h) ? (unsigned)(GetTickCount() - g_shown_at) : 0;
    as_action a = as_decide(&g_cfg, idle, shown, &secs);

    if (a == AS_SHUTDOWN) {
        TLOG(L"helper: countdown finished (idle %u ms), asking the service to shut down", idle);
        PostQuitMessage((int)AS_EXIT_SHUTDOWN);
        return;
    }
    if (a == AS_WARN) {
        if (!IsWindowVisible(h)) {
            TLOG(L"helper: warning shown (idle %u ms)", idle);
            g_secs = 0;
            g_shown_at = GetTickCount();
            place_on_top(h, SWP_SHOWWINDOW);
        } else {
            place_on_top(h, SWP_NOMOVE | SWP_NOSIZE); /* stay above games that grab topmost */
        }
        if (secs != g_secs) {
            g_secs = secs;
            InvalidateRect(h, NULL, FALSE);
            UpdateWindow(h);
            if (as_should_beep(secs))
                Beep(secs <= 5 ? 1200 : 880, 120);
        }
    } else if (IsWindowVisible(h)) {
        TLOG(L"helper: warning cancelled by input");
        ShowWindow(h, SW_HIDE);
    }
    SetTimer(h, 1, as_next_check_ms(&g_cfg, idle), NULL);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER: tick(h); return 0;
    case WM_PAINT: paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_CLOSE: return 0; /* not closable */
    }
    return DefWindowProcW(h, m, wp, lp);
}

static int run_helper(void) {
    WNDCLASSW wc;
    MSG msg;
    HDC dc;
    DWORD idle_min = saved_minutes();
    DWORD warn_s = reg_get(L"WarnSeconds", AS_DEFAULT_WARN_SECONDS);
    if (warn_s < 10 || warn_s > 600)
        warn_s = AS_DEFAULT_WARN_SECONDS;
    g_cfg.idle_ms = idle_min * 60u * 1000u;
    g_cfg.warn_ms = warn_s * 1000u;
#ifdef AS_TEST_BUILD
    { DWORD t = reg_get(L"TestIdleSeconds", 0); if (t >= 5) g_cfg.idle_ms = t * 1000u; }
    TLOG(L"helper: started in session %lu, idle limit %u ms, warning %u ms, idle now %u ms",
         WTSGetActiveConsoleSessionId(), g_cfg.idle_ms, g_cfg.warn_ms, idle_ms());
#endif

    SetProcessDPIAware();
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    g_font_title = make_font(20, FW_SEMIBOLD);
    g_font_num = make_font(72, FW_BOLD);
    g_font_hint = make_font(14, FW_NORMAL);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SuperliteAutoshutWarning";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                            L"Superlite Autoshut", WS_POPUP, 0, 0, px(460), px(210), NULL, NULL,
                            wc.hInstance, NULL);
    if (!g_wnd)
        return 1;
    tick(g_wnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    return (int)msg.wParam;
}

/* ========================================================================
 * Service: keeps the helper running in the console session
 * ===================================================================== */
static SERVICE_STATUS_HANDLE g_ssh;
static SERVICE_STATUS g_st;
static HANDLE g_stop, g_session_changed;

static void set_state(DWORD s) {
    g_st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_st.dwCurrentState = s;
    g_st.dwControlsAccepted = s == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
    g_st.dwWaitHint = 3000;
    SetServiceStatus(g_ssh, &g_st);
}

static DWORD WINAPI svc_ctrl(DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx) {
    (void)type; (void)data; (void)ctx;
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        set_state(SERVICE_STOP_PENDING);
        SetEvent(g_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        SetEvent(g_session_changed);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/* Start "<this exe> helper" on the given session's desktop, as SYSTEM. */
static HANDLE launch_helper(DWORD session) {
    HANDLE tok = NULL, dup = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
    BOOL ok = FALSE;

    memset(&pi, 0, sizeof pi);
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return NULL;
    wsprintfW(cmd, L"\"%s\" helper", exe);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &tok) &&
        DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &dup) &&
        SetTokenInformation(dup, TokenSessionId, &session, sizeof session)) {
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.lpDesktop = L"winsta0\\default";
        ok = CreateProcessAsUserW(dup, exe, cmd, NULL, NULL, FALSE,
                                  CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi);
    }
    if (!ok)
        TLOG(L"service: could not start the helper in session %lu (error %lu)", session, GetLastError());
    if (tok) CloseHandle(tok);
    if (dup) CloseHandle(dup);
    if (!ok)
        return NULL;
    TLOG(L"service: helper started in session %lu, pid %lu", session, pi.dwProcessId);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void shut_down_pc(void) {
#ifdef AS_TEST_BUILD
    TLOG(L"service: TEST BUILD - the real build would shut the PC down now");
#else
    HANDLE t;
    TOKEN_PRIVILEGES tp;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) {
        if (LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(t, FALSE, &tp, 0, NULL, NULL);
        }
        CloseHandle(t);
    }
    InitiateSystemShutdownExW(NULL, NULL, 0, TRUE, FALSE,
                              SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
#endif
}

static void WINAPI svc_main(DWORD argc, LPWSTR *argv) {
    HANDLE helper = NULL, waits[3];
    DWORD helper_session = 0xFFFFFFFF, session, r, code;
    (void)argc; (void)argv;

    g_ssh = RegisterServiceCtrlHandlerExW(SVC_NAME, svc_ctrl, NULL);
    if (!g_ssh)
        return;
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_session_changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    set_state(SERVICE_RUNNING);
    TLOG(L"service: started, idle time %u min", saved_minutes());

    for (;;) {
        session = WTSGetActiveConsoleSessionId();
        if (helper && session != helper_session) { /* user switched / console moved */
            TerminateProcess(helper, 0);
            CloseHandle(helper);
            helper = NULL;
        }
        if (!helper && session != 0xFFFFFFFF) {
            helper = launch_helper(session);
            helper_session = session;
        }
        waits[0] = g_stop;
        waits[1] = g_session_changed;
        waits[2] = helper;
        /* No polling: sleep until a stop, a session change or the helper exits.
           Without a helper, retry every 2 s. */
        r = WaitForMultipleObjects(helper ? 3 : 2, waits, FALSE, helper ? INFINITE : 2000);
        if (r == WAIT_OBJECT_0)
            break;
        if (r == WAIT_OBJECT_0 + 2) {
            code = 0;
            GetExitCodeProcess(helper, &code);
            CloseHandle(helper);
            helper = NULL;
            TLOG(L"service: helper exited with code 0x%lX", code);
            if (code == AS_EXIT_SHUTDOWN) {
                shut_down_pc();
                if (WaitForSingleObject(g_stop, 60000) == WAIT_OBJECT_0)
                    break;
            } else if (WaitForSingleObject(g_stop, 1000) == WAIT_OBJECT_0) {
                break; /* killed by someone: restart it after 1 s */
            }
        }
    }
    if (helper) {
        TerminateProcess(helper, 0);
        CloseHandle(helper);
    }
    TLOG(L"service: stopped");
    set_state(SERVICE_STOPPED);
}

/* ========================================================================
 * Command line: install / set / status / uninstall
 * ===================================================================== */
static SC_HANDLE open_scm(void) {
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!m)
        out(GetLastError() == ERROR_ACCESS_DENIED
            ? L"Run this as administrator (right-click PowerShell > Run as administrator).\r\n"
            : L"Could not open the Windows service manager.\r\n");
    return m;
}

static void stop_service(SC_HANDLE s) {
    SERVICE_STATUS st;
    int i;
    if (!QueryServiceStatus(s, &st) || st.dwCurrentState == SERVICE_STOPPED)
        return;
    ControlService(s, SERVICE_CONTROL_STOP, &st);
    for (i = 0; i < 100; i++) { /* up to 10 s */
        if (QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_STOPPED)
            break;
        Sleep(100);
    }
    Sleep(300); /* let the helper exit so its exe is unlocked */
}

static int cmd_install(int argc, wchar_t **argv) {
    unsigned minutes;
    wchar_t self[MAX_PATH], dir[MAX_PATH], target[MAX_PATH], bin[MAX_PATH + 16];
    SC_HANDLE m, s;
    SERVICE_DESCRIPTIONW desc;
    SERVICE_FAILURE_ACTIONSW fa;
    SC_ACTION acts[3];

    minutes = saved_minutes();
    if (argc >= 3 && !as_parse_minutes(argv[2], &minutes)) {
        out(L"The idle time must be a whole number of minutes from 1 to 1440.\r\n");
        return 2;
    }
    if (!(m = open_scm()))
        return 5;

    s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    if (s)
        stop_service(s);

    GetModuleFileNameW(NULL, self, MAX_PATH);
    install_dir(dir, MAX_PATH);
    install_exe(target, MAX_PATH);
    CreateDirectoryW(dir, NULL);
    if (lstrcmpiW(self, target) != 0 && !CopyFileW(self, target, FALSE)) {
        outf(L"Could not copy the program to %s (error %lu).\r\n", target, GetLastError());
        if (s) CloseServiceHandle(s);
        CloseServiceHandle(m);
        return 1;
    }
    if (!reg_set(L"IdleMinutes", minutes)) {
        out(L"Could not save the idle time in the registry.\r\n");
        if (s) CloseServiceHandle(s);
        CloseServiceHandle(m);
        return 1;
    }

    wsprintfW(bin, L"\"%s\" service", target);
    if (s) {
        ChangeServiceConfigW(s, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             bin, NULL, NULL, NULL, NULL, NULL, SVC_DISPLAY);
    } else {
        s = CreateServiceW(m, SVC_NAME, SVC_DISPLAY, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                           SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin, NULL, NULL, NULL, NULL, NULL);
        if (!s) {
            outf(L"Could not create the service (error %lu).\r\n", GetLastError());
            CloseServiceHandle(m);
            return 1;
        }
    }
    desc.lpDescription = L"Shuts this PC down when nobody uses the keyboard or mouse for the set time.";
    ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &desc);
    acts[0].Type = acts[1].Type = acts[2].Type = SC_ACTION_RESTART;
    acts[0].Delay = acts[1].Delay = 1000;
    acts[2].Delay = 5000;
    memset(&fa, 0, sizeof fa);
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = acts;
    ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    if (!StartServiceW(s, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        outf(L"Installed, but the service did not start (error %lu).\r\n", GetLastError());
        CloseServiceHandle(s);
        CloseServiceHandle(m);
        return 1;
    }
    CloseServiceHandle(s);
    CloseServiceHandle(m);
    outf(L"Superlite Autoshut is installed and running.\r\n"
         L"This PC shuts down after %u minute%s with no keyboard or mouse use.\r\n",
         minutes, minutes == 1 ? L"" : L"s");
#ifdef AS_TEST_BUILD
    out(L"TEST BUILD: it never shuts down. It logs to %ProgramData%\\Superlite Autoshut\\test.log\r\n");
#endif
    return 0;
}

static int cmd_set(int argc, wchar_t **argv) {
    unsigned minutes;
    SC_HANDLE m, s;
    if (argc < 3 || !as_parse_minutes(argv[2], &minutes)) {
        out(L"Usage: SuperliteAutoshut.exe set <minutes>   (1 to 1440)\r\n");
        return 2;
    }
    if (!(m = open_scm()))
        return 5;
    s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    if (!s) {
        out(L"Superlite Autoshut is not installed. Run: SuperliteAutoshut.exe install <minutes>\r\n");
        CloseServiceHandle(m);
        return 1;
    }
    if (!reg_set(L"IdleMinutes", minutes)) {
        out(L"Could not save the idle time in the registry.\r\n");
        CloseServiceHandle(s);
        CloseServiceHandle(m);
        return 1;
    }
    stop_service(s); /* restart so the helper reads the new time */
    StartServiceW(s, 0, NULL);
    CloseServiceHandle(s);
    CloseServiceHandle(m);
    outf(L"Idle time set to %u minute%s.\r\n", minutes, minutes == 1 ? L"" : L"s");
    return 0;
}

static int cmd_uninstall(void) {
    wchar_t self[MAX_PATH], dir[MAX_PATH], target[MAX_PATH];
    SC_HANDLE m, s;
    if (!(m = open_scm()))
        return 5;
    s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    if (s) {
        stop_service(s);
        DeleteService(s);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(m);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, REG_KEY, KEY_WOW64_64KEY, 0);

    GetModuleFileNameW(NULL, self, MAX_PATH);
    install_dir(dir, MAX_PATH);
    install_exe(target, MAX_PATH);
    if (lstrcmpiW(self, target) == 0) {
        /* A running exe can't delete itself: a hidden cmd removes the folder
           2 s after we exit. (Not "delete at reboot": a reinstall before the
           reboot would then lose its exe.) */
        wchar_t cmd[MAX_PATH * 2 + 96];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        wsprintfW(cmd, L"cmd.exe /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\" & rmdir \"%s\"", target, dir);
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    } else {
        DeleteFileW(target);
        RemoveDirectoryW(dir);
    }
    out(L"Superlite Autoshut is removed.\r\n");
    return 0;
}

static int cmd_status(void) {
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), s = NULL;
    SERVICE_STATUS st;
    const wchar_t *state = L"not installed";
    if (m && (s = OpenServiceW(m, SVC_NAME, SERVICE_QUERY_STATUS)) != NULL && QueryServiceStatus(s, &st))
        state = st.dwCurrentState == SERVICE_RUNNING ? L"running"
              : st.dwCurrentState == SERVICE_STOPPED ? L"stopped" : L"starting or stopping";
    if (s) CloseServiceHandle(s);
    if (m) CloseServiceHandle(m);
    outf(L"Superlite Autoshut " AS_VERSION L"\r\nService:   %s\r\nIdle time: %u minute%s\r\n",
         state, saved_minutes(), saved_minutes() == 1 ? L"" : L"s");
#ifdef AS_TEST_BUILD
    out(L"TEST BUILD: it never shuts down, it only logs.\r\n");
#endif
    return 0;
}

static int usage(void) {
    out(L"Superlite Autoshut " AS_VERSION L" - shuts the PC down when nobody uses it.\r\n\r\n"
        L"  SuperliteAutoshut.exe install [minutes]   install and start (default 10 minutes)\r\n"
        L"  SuperliteAutoshut.exe set <minutes>       change the idle time\r\n"
        L"  SuperliteAutoshut.exe status              show the current state\r\n"
        L"  SuperliteAutoshut.exe uninstall           remove it\r\n\r\n"
        L"Install, set and uninstall need an administrator PowerShell or Command Prompt.\r\n");
#ifdef AS_TEST_BUILD
    out(L"\r\nTEST BUILD: it never shuts down, it only logs.\r\n");
#endif
    return 0;
}

static int run(int argc, wchar_t **argv) {
    const wchar_t *c = argc >= 2 ? argv[1] : L"";
    if (!lstrcmpiW(c, L"service")) {
        SERVICE_TABLE_ENTRYW table[2];
        table[0].lpServiceName = SVC_NAME;
        table[0].lpServiceProc = svc_main;
        table[1].lpServiceName = NULL;
        table[1].lpServiceProc = NULL;
        return StartServiceCtrlDispatcherW(table) ? 0 : 1;
    }
    if (!lstrcmpiW(c, L"helper"))    return run_helper();
    if (!lstrcmpiW(c, L"install"))   return cmd_install(argc, argv);
    if (!lstrcmpiW(c, L"set"))       return cmd_set(argc, argv);
    if (!lstrcmpiW(c, L"uninstall")) return cmd_uninstall();
    if (!lstrcmpiW(c, L"status"))    return cmd_status();
    usage();
    { /* double-clicked from Explorer: keep the window open so the text can be read */
        DWORD list[2];
        if (GetConsoleProcessList(list, 2) == 1) {
            wchar_t b[4];
            DWORD n;
            out(L"\r\nPress Enter to close.");
            ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), b, 4, &n, NULL);
        }
    }
    return 0;
}

void entry(void) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = run(argv ? argc : 0, argv);
    if (argv)
        LocalFree(argv);
    ExitProcess((UINT)rc);
}
