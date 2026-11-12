/*
 * Superlite Autoshut - shuts a PC down when nobody touches the keyboard or
 * mouse for a set time. One small exe, no C runtime.
 *
 *   SuperliteAutoshut.exe install [minutes]   install + start the service (admin)
 *   SuperliteAutoshut.exe set <minutes>       change the idle time (admin)
 *   SuperliteAutoshut.exe status              show the current settings
 *   SuperliteAutoshut.exe uninstall           remove everything (admin)
 *
 * Settings live in config.ini next to the installed exe. The service waits
 * on a Windows change notification for that folder, so a saved edit applies
 * at once without polling.
 *
 * Internal modes: "service" (run by Windows) and "helper" (started by the
 * service on the console desktop, as SYSTEM so customers can't end it).
 * The helper reads the session's last-input time, shows the countdown, and
 * exits with AS_EXIT_POWER when it reaches 0; the service then does the
 * configured action (shutdown / restart / sleep / hibernate).
 *
 * Build with /DAS_TEST_BUILD for a copy that never turns the PC off: it writes
 * what it would do to %ProgramData%\Superlite Autoshut\test.log instead.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <powrprof.h>
#include <intrin.h>
#include "logic.h"

#define SVC_NAME     L"SuperliteAutoshut"
#define OLD_REG_KEY  L"SOFTWARE\\Superlite Autoshut" /* 1.0.0 kept IdleMinutes here */
#define EXE_NAME     L"SuperliteAutoshut.exe"
#define INI_NAME     L"config.ini"
#define INSTALL_DIR  L"Superlite Autoshut"
#define AS_EXIT_POWER 0x5D0Fu
#define AS_VERSION   L"1.1.0"
#define DEFAULT_TITLE L"No activity detected"

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
        char buf[2048];
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

/* ---- paths ------------------------------------------------------------ */
static void install_dir(wchar_t *buf) {
    DWORD n = GetEnvironmentVariableW(L"ProgramW6432", buf, MAX_PATH);
    if (!n || n >= MAX_PATH)
        n = GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH);
    if (!n || n >= MAX_PATH)
        lstrcpyW(buf, L"C:\\Program Files");
    lstrcatW(buf, L"\\" INSTALL_DIR);
}

static void install_file(wchar_t *buf, const wchar_t *name) {
    install_dir(buf);
    lstrcatW(buf, L"\\");
    lstrcatW(buf, name);
}

/* ---- test log (test build only) ------------------------------------------ */
#ifdef AS_TEST_BUILD
static void logw(const wchar_t *fmt, ...) {
    wchar_t path[MAX_PATH], line[1024];
    char utf8[2048];
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
 * config.ini
 * ===================================================================== */
typedef struct {
    unsigned idle_minutes;
    unsigned warn_seconds;
    as_power power;
    int beep;
    int enabled;
    wchar_t title[128];
    wchar_t message[512];
    unsigned test_idle_seconds; /* test build only: [Test] IdleSeconds */
} config;

static int ini_int(const wchar_t *path, const wchar_t *sec, const wchar_t *key, int lo, int hi, int def) {
    return as_in_range_or((int)GetPrivateProfileIntW(sec, key, def, path), lo, hi, def);
}

/* "\n" typed in the ini becomes a line break on the warning. */
static void unescape_newlines(wchar_t *s) {
    wchar_t *w = s;
    for (; *s; s++) {
        if (s[0] == L'\\' && (s[1] == L'n' || s[1] == L'N')) {
            *w++ = L'\n';
            s++;
        } else {
            *w++ = *s;
        }
    }
    *w = 0;
}

static void config_load(config *c) {
    wchar_t path[MAX_PATH], buf[32];
    install_file(path, INI_NAME);
    c->idle_minutes = (unsigned)ini_int(path, L"Settings", L"IdleMinutes", AS_MIN_MINUTES, AS_MAX_MINUTES, AS_DEFAULT_MINUTES);
    c->warn_seconds = (unsigned)ini_int(path, L"Settings", L"WarnSeconds", 10, 600, AS_DEFAULT_WARN_SECONDS);
    GetPrivateProfileStringW(L"Settings", L"Action", L"shutdown", buf, 32, path);
    c->power = as_parse_power(buf);
    c->beep = ini_int(path, L"Settings", L"Beep", 0, 1, 1);
    c->enabled = ini_int(path, L"Settings", L"Enabled", 0, 1, 1);
    GetPrivateProfileStringW(L"Warning", L"Title", L"", c->title, 128, path);
    if (!c->title[0])
        lstrcpyW(c->title, DEFAULT_TITLE);
    GetPrivateProfileStringW(L"Warning", L"Message", L"", c->message, 512, path);
    if (!c->message[0])
        lstrcpynW(c->message, as_default_message(c->power), 512);
    unescape_newlines(c->title);
    unescape_newlines(c->message);
    c->test_idle_seconds = (unsigned)ini_int(path, L"Test", L"IdleSeconds", 5, 86400, 0);
}

/* Old 1.0.0 setting, used once when upgrading. */
static unsigned old_registry_minutes(void) {
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, OLD_REG_KEY, L"IdleMinutes", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS &&
        v >= AS_MIN_MINUTES && v <= AS_MAX_MINUTES)
        return v;
    return AS_DEFAULT_MINUTES;
}

/* Write a fresh, commented config.ini (UTF-16 so any language works). */
static int config_write_default(unsigned minutes) {
    static const wchar_t head[] =
        L"; Superlite Autoshut settings.\r\n"
        L"; Edit as administrator (this folder is in Program Files) and save:\r\n"
        L"; the new settings apply by themselves, no restart needed.\r\n"
        L"\r\n"
        L"[Settings]\r\n"
        L"; Minutes with no keyboard or mouse use before the warning shows (1-1440)\r\n";
    static const wchar_t tail[] =
        L"; How long the warning counts down, in seconds (10-600)\r\n"
        L"WarnSeconds=60\r\n"
        L"; What happens when the countdown ends: shutdown, restart, sleep or hibernate\r\n"
        L"Action=shutdown\r\n"
        L"; Beep during the countdown: 1 = on, 0 = off\r\n"
        L"Beep=1\r\n"
        L"; 1 = on, 0 = paused (stays installed but never acts)\r\n"
        L"Enabled=1\r\n"
        L"\r\n"
        L"[Warning]\r\n"
        L"; Text on the warning screen. Write \\n for a new line.\r\n"
        L"; Leave Message empty to use the default text for the Action.\r\n"
        L"Title=No activity detected\r\n"
        L"Message=\r\n";
    wchar_t path[MAX_PATH], line[64];
    WORD bom = 0xFEFF;
    DWORD w;
    BOOL ok;
    HANDLE f;
    install_file(path, INI_NAME);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    wsprintfW(line, L"IdleMinutes=%u\r\n", minutes);
    ok = WriteFile(f, &bom, sizeof bom, &w, NULL) &&
         WriteFile(f, head, sizeof head - sizeof(wchar_t), &w, NULL) &&
         WriteFile(f, line, (DWORD)lstrlenW(line) * sizeof(wchar_t), &w, NULL) &&
         WriteFile(f, tail, sizeof tail - sizeof(wchar_t), &w, NULL);
    CloseHandle(f);
    return ok;
}

static int config_set_minutes(unsigned minutes) {
    wchar_t path[MAX_PATH], v[16];
    install_file(path, INI_NAME);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return config_write_default(minutes);
    wsprintfW(v, L"%u", minutes);
    return WritePrivateProfileStringW(L"Settings", L"IdleMinutes", v, path);
}

/* ========================================================================
 * Helper: lives on the console desktop, watches for input, shows warning
 * ===================================================================== */
static config g_conf;
static as_config g_cfg;
static unsigned g_secs;
static DWORD g_shown_at; /* tick count when the warning appeared */
static HFONT g_font_title, g_font_num, g_font_hint;
static int g_dpi = 96, g_msg_h;

#define CLR_BG     RGB(0x14, 0x1B, 0x24)
#define CLR_FG     RGB(0xF2, 0xF5, 0xF8)
#define CLR_WARN   RGB(0xF0, 0xA0, 0x20)
#define CLR_HINT   RGB(0xAA, 0xB6, 0xC3)
#define WIN_W      460

static int px(int v) { return MulDiv(v, g_dpi, 96); }
static int win_h(void) { return px(140) + g_msg_h + px(22); }

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

    r = rc; r.top = px(18); r.bottom = px(52); r.left += px(16); r.right -= px(16);
    SelectObject(dc, g_font_title);
    SetTextColor(dc, CLR_FG);
    DrawTextW(dc, g_conf.title, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    r = rc; r.top = px(52); r.bottom = px(140);
    SelectObject(dc, g_font_num);
    SetTextColor(dc, CLR_WARN);
    wsprintfW(num, L"%u", g_secs);
    DrawTextW(dc, num, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    r = rc; r.top = px(140); r.bottom = r.top + g_msg_h; r.left += px(20); r.right -= px(20);
    SelectObject(dc, g_font_hint);
    SetTextColor(dc, CLR_HINT);
    DrawTextW(dc, g_conf.message, -1, &r, DT_CENTER | DT_WORDBREAK);

    DeleteObject(bg);
    DeleteObject(border);
    EndPaint(h, &ps);
}

static void place_on_top(HWND h, UINT extra) {
    int w = px(WIN_W), ht = win_h();
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - ht) / 2;
    SetWindowPos(h, HWND_TOPMOST, x, y, w, ht, SWP_NOACTIVATE | extra);
}

static void tick(HWND h) {
    unsigned idle = idle_ms(), secs = 0;
    unsigned shown = IsWindowVisible(h) ? (unsigned)(GetTickCount() - g_shown_at) : 0;
    as_action a = as_decide(&g_cfg, idle, shown, &secs);

    if (a == AS_SHUTDOWN) {
        TLOG(L"helper: countdown finished (idle %u ms), asking the service to %s", idle, as_power_name(g_conf.power));
        PostQuitMessage((int)AS_EXIT_POWER);
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
            if (g_conf.beep && as_should_beep(secs))
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
    HWND wnd;
    RECT r;

    config_load(&g_conf);
    g_cfg.idle_ms = g_conf.idle_minutes * 60u * 1000u;
    g_cfg.warn_ms = g_conf.warn_seconds * 1000u;
#ifdef AS_TEST_BUILD
    if (g_conf.test_idle_seconds)
        g_cfg.idle_ms = g_conf.test_idle_seconds * 1000u;
    TLOG(L"helper: started in session %lu, idle limit %u ms, warning %u ms, action %s, beep %d, idle now %u ms",
         WTSGetActiveConsoleSessionId(), g_cfg.idle_ms, g_cfg.warn_ms, as_power_name(g_conf.power),
         g_conf.beep, idle_ms());
#endif

    SetProcessDPIAware();
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    g_font_title = make_font(20, FW_SEMIBOLD);
    g_font_num = make_font(72, FW_BOLD);
    g_font_hint = make_font(14, FW_NORMAL);
    /* window grows to fit a long custom message (capped so it stays on screen) */
    SelectObject(dc, g_font_hint);
    r.left = 0; r.top = 0; r.right = px(WIN_W - 40); r.bottom = 0;
    DrawTextW(dc, g_conf.message, -1, &r, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    g_msg_h = r.bottom < px(20) ? px(20) : r.bottom > px(300) ? px(300) : r.bottom;
    ReleaseDC(NULL, dc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SuperliteAutoshutWarning";
    RegisterClassW(&wc);
    wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                          L"Superlite Autoshut", WS_POPUP, 0, 0, px(WIN_W), win_h(), NULL, NULL,
                          wc.hInstance, NULL);
    if (!wnd)
        return 1;
    tick(wnd);
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

static void power_off(as_power p) {
#ifdef AS_TEST_BUILD
    TLOG(L"service: TEST BUILD - the real build would %s the PC now", as_power_name(p));
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
    /* Sleep/hibernate turned off on this PC? Shut down instead of staying on. */
    if ((p == AS_POWER_SLEEP || p == AS_POWER_HIBERNATE) &&
        SetSuspendState(p == AS_POWER_HIBERNATE, FALSE, FALSE))
        return;
    InitiateSystemShutdownExW(NULL, NULL, 0, TRUE, p == AS_POWER_RESTART,
                              SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
#endif
}

static void kill_helper(HANDLE *helper) {
    if (*helper) {
        TerminateProcess(*helper, 0);
        WaitForSingleObject(*helper, 2000);
        CloseHandle(*helper);
        *helper = NULL;
    }
}

static void WINAPI svc_main(DWORD argc, LPWSTR *argv) {
    HANDLE helper = NULL, watch, waits[4];
    DWORD helper_session = 0xFFFFFFFF, session, r, code, n, w_watch, w_helper;
    wchar_t dir[MAX_PATH];
    config conf;
    (void)argc; (void)argv;

    g_ssh = RegisterServiceCtrlHandlerExW(SVC_NAME, svc_ctrl, NULL);
    if (!g_ssh)
        return;
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_session_changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    install_dir(dir);
    watch = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE |
                                         FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
    config_load(&conf);
    set_state(SERVICE_RUNNING);
    TLOG(L"service: started, idle %u min, action %s, enabled %d", conf.idle_minutes,
         as_power_name(conf.power), conf.enabled);

    for (;;) {
        session = WTSGetActiveConsoleSessionId();
        if (helper && session != helper_session) /* user switched / console moved */
            kill_helper(&helper);
        if (!helper && conf.enabled && session != 0xFFFFFFFF) {
            helper = launch_helper(session);
            helper_session = session;
        }
        /* No polling: sleep until a stop, a session change, a config.ini
           change or the helper exits. Without a helper (but enabled), retry every 2 s. */
        n = 0;
        waits[n++] = g_stop;
        waits[n++] = g_session_changed;
        w_watch = w_helper = 0xFFFFFFFF;
        if (watch != INVALID_HANDLE_VALUE) { w_watch = n; waits[n++] = watch; }
        if (helper) { w_helper = n; waits[n++] = helper; }
        r = WaitForMultipleObjects(n, waits, FALSE, (helper || !conf.enabled) ? INFINITE : 2000);
        if (r == WAIT_OBJECT_0)
            break;
        if (r == WAIT_OBJECT_0 + w_watch) {
            /* editors save in several steps: let them finish, then reload */
            if (WaitForSingleObject(g_stop, 700) == WAIT_OBJECT_0)
                break;
            FindNextChangeNotification(watch);
            config_load(&conf);
            TLOG(L"service: config.ini changed: idle %u min, action %s, enabled %d", conf.idle_minutes,
                 as_power_name(conf.power), conf.enabled);
            kill_helper(&helper); /* restarted next loop with the new settings */
        } else if (r == WAIT_OBJECT_0 + w_helper) {
            code = 0;
            GetExitCodeProcess(helper, &code);
            CloseHandle(helper);
            helper = NULL;
            TLOG(L"service: helper exited with code 0x%lX", code);
            if (code == AS_EXIT_POWER) {
                config_load(&conf);
                power_off(conf.power);
                /* shutdown/restart: Windows stops us soon. sleep: continue after wake. */
                if (WaitForSingleObject(g_stop, (conf.power == AS_POWER_SLEEP || conf.power == AS_POWER_HIBERNATE)
                                                    ? 5000 : 60000) == WAIT_OBJECT_0)
                    break;
            } else if (WaitForSingleObject(g_stop, 1000) == WAIT_OBJECT_0) {
                break; /* killed by someone: restart it after 1 s */
            }
        }
    }
    kill_helper(&helper);
    if (watch != INVALID_HANDLE_VALUE)
        FindCloseChangeNotification(watch);
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

static void print_settings(void) {
    config c;
    wchar_t path[MAX_PATH];
    install_file(path, INI_NAME);
    config_load(&c);
    outf(L"Settings:  %s\r\n", path);
    outf(L"  Idle time   %u minute%s\r\n", c.idle_minutes, c.idle_minutes == 1 ? L"" : L"s");
    outf(L"  Warning     %u seconds, beep %s\r\n", c.warn_seconds, c.beep ? L"on" : L"off");
    outf(L"  Action      %s\r\n", as_power_name(c.power));
    outf(L"  Enabled     %s\r\n", c.enabled ? L"yes" : L"no (paused)");
#ifdef AS_TEST_BUILD
    out(L"TEST BUILD: it never turns the PC off, it only logs to %ProgramData%\\Superlite Autoshut\\test.log\r\n");
#endif
}

static int cmd_install(int argc, wchar_t **argv) {
    unsigned minutes = 0;
    int have_minutes = 0;
    wchar_t self[MAX_PATH], dir[MAX_PATH], target[MAX_PATH], ini[MAX_PATH], bin[MAX_PATH + 16];
    SC_HANDLE m, s;
    SERVICE_DESCRIPTIONW desc;
    SERVICE_FAILURE_ACTIONSW fa;
    SC_ACTION acts[3];
    int ok;

    if (argc >= 3) {
        if (!as_parse_minutes(argv[2], &minutes)) {
            out(L"The idle time must be a whole number of minutes from 1 to 1440.\r\n");
            return 2;
        }
        have_minutes = 1;
    }
    if (!(m = open_scm()))
        return 5;

    s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    if (s)
        stop_service(s);

    GetModuleFileNameW(NULL, self, MAX_PATH);
    install_dir(dir);
    install_file(target, EXE_NAME);
    install_file(ini, INI_NAME);
    CreateDirectoryW(dir, NULL);
    if (lstrcmpiW(self, target) != 0 && !CopyFileW(self, target, FALSE)) {
        outf(L"Could not copy the program to %s (error %lu).\r\n", target, GetLastError());
        if (s) CloseServiceHandle(s);
        CloseServiceHandle(m);
        return 1;
    }

    /* Keep an existing config.ini; only change IdleMinutes when given. */
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES)
        ok = config_write_default(have_minutes ? minutes : old_registry_minutes());
    else
        ok = have_minutes ? config_set_minutes(minutes) : 1;
    if (!ok) {
        outf(L"Could not write %s.\r\n", ini);
        if (s) CloseServiceHandle(s);
        CloseServiceHandle(m);
        return 1;
    }
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, OLD_REG_KEY, KEY_WOW64_64KEY, 0); /* moved to config.ini */

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
    desc.lpDescription = L"Shuts this PC down when nobody uses the keyboard or mouse for the set time. Settings: config.ini in its folder.";
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
    out(L"Superlite Autoshut " AS_VERSION L" is installed and running.\r\n");
    print_settings();
    return 0;
}

static int cmd_set(int argc, wchar_t **argv) {
    unsigned minutes;
    if (argc < 3 || !as_parse_minutes(argv[2], &minutes)) {
        out(L"Usage: SuperliteAutoshut.exe set <minutes>   (1 to 1440)\r\n");
        return 2;
    }
    if (!config_set_minutes(minutes)) {
        out(GetLastError() == ERROR_ACCESS_DENIED
            ? L"Run this as administrator (right-click PowerShell > Run as administrator).\r\n"
            : L"Could not save config.ini. Is Superlite Autoshut installed?\r\n");
        return 1;
    }
    outf(L"Idle time set to %u minute%s. It applies right away.\r\n", minutes, minutes == 1 ? L"" : L"s");
    return 0;
}

static int cmd_uninstall(void) {
    wchar_t self[MAX_PATH], dir[MAX_PATH], target[MAX_PATH], ini[MAX_PATH];
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
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, OLD_REG_KEY, KEY_WOW64_64KEY, 0);

    GetModuleFileNameW(NULL, self, MAX_PATH);
    install_dir(dir);
    install_file(target, EXE_NAME);
    install_file(ini, INI_NAME);
    DeleteFileW(ini);
    if (lstrcmpiW(self, target) == 0) {
        /* A running exe can't delete itself: a hidden cmd removes it 2 s
           after we exit. (Not "delete at reboot": a reinstall before the
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
    outf(L"Superlite Autoshut " AS_VERSION L"\r\nService:   %s\r\n", state);
    print_settings();
    return 0;
}

static int usage(void) {
    out(L"Superlite Autoshut " AS_VERSION L" - shuts the PC down when nobody uses it.\r\n\r\n"
        L"  SuperliteAutoshut.exe install [minutes]   install and start (default 10 minutes)\r\n"
        L"  SuperliteAutoshut.exe set <minutes>       change the idle time\r\n"
        L"  SuperliteAutoshut.exe status              show the current settings\r\n"
        L"  SuperliteAutoshut.exe uninstall           remove it\r\n\r\n"
        L"All settings are in config.ini next to the installed exe:\r\n"
        L"  C:\\Program Files\\Superlite Autoshut\\config.ini\r\n"
        L"Install, set and uninstall need an administrator PowerShell or Command Prompt.\r\n");
#ifdef AS_TEST_BUILD
    out(L"\r\nTEST BUILD: it never turns the PC off, it only logs.\r\n");
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
