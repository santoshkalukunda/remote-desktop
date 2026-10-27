// RemoteDesk main window: shows this desk's ID, keeps the relay registration
// alive, and starts outgoing connections.
#include "app.h"
#include "crypto.h"
#include <cstdio>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

std::atomic<DWORD> g_clipIgnoreSeq{ 0 };
HWND g_mainWnd = nullptr;

static const wchar_t* MAIN_CLASS = L"RemoteDeskMain";
static const char* DEFAULT_RELAY = "127.0.0.1:21116";

// ---------------------------------------------------------------- settings
static std::mutex g_setMx;
static std::wstring g_ini;
static std::wstring g_cfg;   // config.ini next to the exe, written by setup.exe (read-only in MSIX)
static std::string g_id, g_relay, g_pwSalt, g_pwHash;

static std::string cfg_get(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[512];
    GetPrivateProfileStringW(section, key, L"", buf, 512, g_cfg.c_str());
    return narrow(buf);
}

static std::string ini_get(const wchar_t* key) {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"RemoteDesk", key, L"", buf, 512, g_ini.c_str());
    return narrow(buf);
}
static void ini_set(const wchar_t* key, const std::string& v) {
    WritePrivateProfileStringW(L"RemoteDesk", key, widen(v).c_str(), g_ini.c_str());
}

static void load_settings() {
    wchar_t appdata[MAX_PATH] = L".";
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    std::wstring dir = std::wstring(appdata) + L"\\RemoteDesk";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_ini = dir + L"\\settings.ini";

    std::lock_guard<std::mutex> lk(g_setMx);
    g_id = ini_get(L"Id");
    if (g_id.size() != 9 || digits_only(g_id) != g_id) { g_id = random_id(); ini_set(L"Id", g_id); }
    // The relay chosen in the installer (config.ini next to the exe) wins once each time it
    // changes, so installing with a new relay takes effect; later edits in the app stick.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_cfg = exe;
    g_cfg = g_cfg.substr(0, g_cfg.find_last_of(L'\\') + 1) + L"config.ini";
    const std::string installed = cfg_get(L"RemoteDesk", L"Relay");
    g_relay = ini_get(L"Relay");
    if (!installed.empty() && installed != ini_get(L"InstallerRelay")) {
        g_relay = installed;
        ini_set(L"Relay", g_relay);
        ini_set(L"InstallerRelay", installed);
    }
    if (g_relay.empty()) g_relay = DEFAULT_RELAY;
    g_pwSalt = ini_get(L"PasswordSalt");
    g_pwHash = ini_get(L"PasswordHash");
}

// Accepted in setup.exe (config.ini) or on the first-run screen (settings.ini).
static bool consent_given() {
    return cfg_get(L"Consent", L"PrivacyPolicy") == POLICY_VERSION || ini_get(L"AcceptedPolicy") == POLICY_VERSION;
}

static void save_consent() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char when[32];
    snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    ini_set(L"AcceptedPolicy", POLICY_VERSION);
    ini_set(L"Permissions", "screen,control,clipboard,network,keepawake");
    ini_set(L"AcceptedOn", when);
}

static void new_id() {
    std::lock_guard<std::mutex> lk(g_setMx);
    g_id = random_id();
    ini_set(L"Id", g_id);
}

std::string my_id() {
    std::lock_guard<std::mutex> lk(g_setMx);
    return g_id;
}

bool relay_address(std::string& host, uint16_t& port) {
    std::string r;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        r = g_relay;
    }
    size_t c = r.rfind(':');
    host = r.substr(0, c);
    port = DEFAULT_RELAY_PORT;
    if (c != std::string::npos) {
        int p = atoi(r.c_str() + c + 1);
        if (p > 0 && p < 65536) port = (uint16_t)p;
    }
    return !host.empty();
}

static void set_relay(const std::string& r) {
    std::lock_guard<std::mutex> lk(g_setMx);
    g_relay = r.empty() ? DEFAULT_RELAY : r;
    ini_set(L"Relay", g_relay);
}

bool password_is_set() {
    std::lock_guard<std::mutex> lk(g_setMx);
    return !g_pwHash.empty();
}

bool password_matches(const std::string& pw) {
    std::string salt, hash;
    {
        std::lock_guard<std::mutex> lk(g_setMx);
        salt = g_pwSalt; hash = g_pwHash;
    }
    return !hash.empty() && equal_ct(hash_password(pw, salt), hash);
}

static void set_password(const std::string& pw) {
    std::string salt = pw.empty() ? "" : random_hex(16);
    std::string hash = pw.empty() ? "" : hash_password(pw, salt);
    std::lock_guard<std::mutex> lk(g_setMx);
    g_pwSalt = salt; g_pwHash = hash;
    ini_set(L"PasswordSalt", salt);
    ini_set(L"PasswordHash", hash);
}

void post_text(UINT msg, const std::wstring& text) {
    auto* p = new std::wstring(text);
    if (!PostMessageW(g_mainWnd, msg, 0, (LPARAM)p)) delete p;
}

SOCKET relay_open(const std::string& command) {
    std::string host;
    uint16_t port;
    if (!relay_address(host, port)) return INVALID_SOCKET;
    SOCKET s = tcp_connect(host, port);
    if (s != INVALID_SOCKET && !send_line(s, command)) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

// ------------------------------------------------- relay control connection
static std::atomic<SOCKET> g_ctrlSock{ INVALID_SOCKET };
static std::atomic<int> g_relayGen{ 0 };

static void wait_retry(int ms, int gen) {
    for (int t = 0; t < ms && g_relayGen == gen; t += 100) Sleep(100);
}

static void control_thread() {
    for (;;) {
        const int gen = g_relayGen;
        std::string host;
        uint16_t port;
        relay_address(host, port);
        const std::wstring where = widen(host + ":" + std::to_string(port));
        post_text(WM_APP_STATUS, L"Connecting to relay " + where + L"...");

        SOCKET s = relay_open("REG " + my_id());
        if (s == INVALID_SOCKET) {
            post_text(WM_APP_STATUS, L"Offline: relay " + where + L" unreachable");
            wait_retry(3000, gen);
            continue;
        }
        g_ctrlSock = s;
        std::string acc, line;
        LineResult r = read_line(s, acc, line, 15000);
        if (r == LineResult::Ok && line == "TAKEN") {   // ID clash: pick a new one
            new_id();
            PostMessageW(g_mainWnd, WM_APP_ID, 0, 0);
        } else if (r == LineResult::Ok && line == "OK") {
            post_text(WM_APP_STATUS, L"Online: ready for connections (relay " + where + L")");
            for (;;) {
                r = read_line(s, acc, line, 20000);
                if (r == LineResult::Timeout) {
                    if (!send_line(s, "PING")) break;
                } else if (r == LineResult::Error) {
                    break;
                } else if (line.rfind("CALL ", 0) == 0) {
                    std::thread(host_session, line.substr(5)).detach();
                }
            }
            post_text(WM_APP_STATUS, L"Offline: lost connection to relay " + where);
        } else {
            post_text(WM_APP_STATUS, L"Offline: relay " + where + L" refused registration");
        }
        g_ctrlSock = INVALID_SOCKET;
        closesocket(s);
        wait_retry(2000, gen);
    }
}

// ---------------------------------------------------------------- window
enum {
    IDC_ID = 100, IDC_STATUS, IDC_PW, IDC_PW_SET, IDC_PW_STATE, IDC_SESSIONS, IDC_DISCONNECT,
    IDC_REMOTE, IDC_REMOTE_PW, IDC_CONNECT, IDC_INFO, IDC_RELAY, IDC_RELAY_SAVE, IDC_COPY_ID,
    IDC_STATIC_BASE = 200,
};
constexpr UINT_PTR TIMER_COPIED = 1;   // restores the Copy button label
enum Font { F_NORMAL, F_BIG, F_MEDIUM, F_SMALL, F_COUNT };

struct CtlDef {
    int id;
    const wchar_t* cls;
    const wchar_t* text;
    DWORD style;
    int x, y, w, h;
    Font font;
};

static const CtlDef CONTROLS[] = {
    { 0, L"BUTTON", L"This Desk", BS_GROUPBOX, 12, 8, 302, 272, F_NORMAL },
    { 0, L"STATIC", L"Your address", 0, 28, 32, 270, 18, F_NORMAL },
    { IDC_ID, L"STATIC", L"", SS_NOPREFIX, 28, 52, 196, 42, F_BIG },
    { IDC_COPY_ID, L"BUTTON", L"Copy", WS_TABSTOP, 228, 60, 70, 28, F_NORMAL },
    { IDC_STATUS, L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 28, 96, 270, 18, F_SMALL },
    { 0, L"STATIC", L"Password for unattended access", 0, 28, 128, 270, 18, F_NORMAL },
    { IDC_PW, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 28, 148, 180, 24, F_NORMAL },
    { IDC_PW_SET, L"BUTTON", L"Set", WS_TABSTOP, 214, 148, 84, 24, F_NORMAL },
    { IDC_PW_STATE, L"STATIC", L"", 0, 28, 176, 270, 30, F_SMALL },
    { IDC_SESSIONS, L"STATIC", L"", 0, 28, 208, 270, 32, F_NORMAL },
    { IDC_DISCONNECT, L"BUTTON", L"Disconnect all", WS_TABSTOP | WS_DISABLED, 28, 242, 140, 28, F_NORMAL },

    { 0, L"BUTTON", L"Remote Desk", BS_GROUPBOX, 326, 8, 302, 272, F_NORMAL },
    { 0, L"STATIC", L"Remote address", 0, 342, 32, 270, 18, F_NORMAL },
    { IDC_REMOTE, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 342, 52, 270, 32, F_MEDIUM },
    { 0, L"STATIC", L"Password (optional, for unattended access)", 0, 342, 94, 270, 18, F_NORMAL },
    { IDC_REMOTE_PW, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 342, 114, 270, 24, F_NORMAL },
    { IDC_CONNECT, L"BUTTON", L"Connect", WS_TABSTOP | BS_DEFPUSHBUTTON, 342, 148, 270, 36, F_MEDIUM },
    { IDC_INFO, L"STATIC", L"Leave the password empty to ask the remote user to accept.", SS_NOPREFIX, 342, 194, 270, 76, F_SMALL },

    { 0, L"STATIC", L"Relay server", 0, 12, 296, 84, 20, F_NORMAL },
    { IDC_RELAY, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 96, 292, 218, 24, F_NORMAL },
    { IDC_RELAY_SAVE, L"BUTTON", L"Save", WS_TABSTOP, 326, 292, 84, 24, F_NORMAL },
    { 0, L"STATIC", L"host:port, same on both computers", 0, 420, 296, 210, 20, F_SMALL },
};
constexpr int CLIENT_W = 640, CLIENT_H = 330;

static HFONT g_fonts[F_COUNT];
static HWND g_ctl[sizeof(CONTROLS) / sizeof(CONTROLS[0])];
static HBRUSH g_bg;

static HWND ctl(int id) { return GetDlgItem(g_mainWnd, id); }

static std::wstring get_text(int id) {
    int n = GetWindowTextLengthW(ctl(id));
    std::wstring s(n, 0);
    GetWindowTextW(ctl(id), &s[0], n + 1);
    return s;
}

static void make_fonts(UINT dpi) {
    struct { int pt; int weight; } spec[F_COUNT] = { { 9, FW_NORMAL }, { 24, FW_BOLD }, { 13, FW_NORMAL }, { 8, FW_NORMAL } };
    for (int i = 0; i < F_COUNT; ++i) {
        if (g_fonts[i]) DeleteObject(g_fonts[i]);
        g_fonts[i] = CreateFontW(-MulDiv(spec[i].pt, dpi, 72), 0, 0, 0, spec[i].weight, 0, 0, 0, DEFAULT_CHARSET,
                                 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
}

static void layout(UINT dpi) {
    make_fonts(dpi);
    for (size_t i = 0; i < sizeof(CONTROLS) / sizeof(CONTROLS[0]); ++i) {
        const CtlDef& c = CONTROLS[i];
        SetWindowPos(g_ctl[i], nullptr, MulDiv(c.x, dpi, 96), MulDiv(c.y, dpi, 96), MulDiv(c.w, dpi, 96),
                     MulDiv(c.h, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(g_ctl[i], WM_SETFONT, (WPARAM)g_fonts[c.font], TRUE);
    }
}

static void update_id() {
    SetWindowTextW(ctl(IDC_ID), widen(format_id(my_id())).c_str());
}

static void update_pw_state() {
    SetWindowTextW(ctl(IDC_PW_STATE), password_is_set()
        ? L"Unattended access: ON (anyone with the password can connect)"
        : L"Unattended access: off (every connection must be accepted)");
}

static void on_connect() {
    std::string id = digits_only(narrow(get_text(IDC_REMOTE)));
    if (id.size() != 9) {
        MessageBoxW(g_mainWnd, L"Enter the 9-digit address of the remote desk.", L"RemoteDesk", MB_ICONINFORMATION);
        SetFocus(ctl(IDC_REMOTE));
        return;
    }
    if (id == my_id()) {
        MessageBoxW(g_mainWnd,
                    L"That is this computer's own address.\n\n"
                    L"Enter the address shown on the other computer. To test, run RemoteDesk on a second "
                    L"computer that uses the same relay server, and connect from one to the other.",
                    L"RemoteDesk", MB_ICONINFORMATION);
        SetFocus(ctl(IDC_REMOTE));
        return;
    }
    std::string pw = narrow(get_text(IDC_REMOTE_PW));
    SetWindowTextW(ctl(IDC_REMOTE_PW), L"");
    std::thread(viewer_session, id, pw).detach();
}

static void on_set_password() {
    std::string pw = narrow(get_text(IDC_PW));
    if (!pw.empty() && pw.size() < 8) {
        MessageBoxW(g_mainWnd, L"Use at least 8 characters for the unattended access password.",
                    L"RemoteDesk", MB_ICONWARNING);
        return;
    }
    set_password(pw);
    SetWindowTextW(ctl(IDC_PW), L"");
    update_pw_state();
}

static void on_copy_id() {
    clipboard_set(g_mainWnd, widen(my_id()));
    SetWindowTextW(ctl(IDC_COPY_ID), L"Copied!");
    SetTimer(g_mainWnd, TIMER_COPIED, 1500, nullptr);
}

static void on_save_relay() {
    set_relay(narrow(get_text(IDC_RELAY)));
    std::string host;
    uint16_t port;
    relay_address(host, port);
    SetWindowTextW(ctl(IDC_RELAY), widen(host + ":" + std::to_string(port)).c_str());
    ++g_relayGen;
    SOCKET s = g_ctrlSock;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);   // control thread reconnects
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_mainWnd = hwnd;
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        for (size_t i = 0; i < sizeof(CONTROLS) / sizeof(CONTROLS[0]); ++i) {
            const CtlDef& c = CONTROLS[i];
            int id = c.id ? c.id : IDC_STATIC_BASE + (int)i;
            g_ctl[i] = CreateWindowExW(0, c.cls, c.text, WS_CHILD | WS_VISIBLE | c.style, 0, 0, 0, 0, hwnd,
                                       (HMENU)(INT_PTR)id, hi, nullptr);
        }
        UINT dpi = GetDpiForWindow(hwnd);
        layout(dpi);
        RECT r{ 0, 0, MulDiv(CLIENT_W, dpi, 96), MulDiv(CLIENT_H, dpi, 96) };
        AdjustWindowRectExForDpi(&r, GetWindowLongW(hwnd, GWL_STYLE), FALSE, 0, dpi);
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);

        update_id();
        update_pw_state();
        std::string host;
        uint16_t port;
        relay_address(host, port);
        SetWindowTextW(ctl(IDC_RELAY), widen(host + ":" + std::to_string(port)).c_str());
        SendMessageW(hwnd, WM_APP_SESSIONS, 0, 0);
        return 0;
    }
    case WM_DPICHANGED: {
        layout(HIWORD(wp));
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if (id == IDC_ID) SetTextColor(dc, RGB(0, 96, 192));
        else if (id == IDC_STATUS || id == IDC_PW_STATE || id == IDC_INFO) SetTextColor(dc, RGB(96, 96, 96));
        else SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)g_bg;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {   // Enter key: act on the field that has focus
            HWND f = GetFocus();
            if (f == ctl(IDC_PW)) on_set_password();
            else if (f == ctl(IDC_RELAY)) on_save_relay();
            else on_connect();
            return 0;
        }
        case IDC_CONNECT: on_connect(); return 0;
        case IDC_PW_SET: on_set_password(); return 0;
        case IDC_RELAY_SAVE: on_save_relay(); return 0;
        case IDC_COPY_ID: on_copy_id(); return 0;
        case IDC_DISCONNECT: disconnect_all_incoming(); return 0;
        }
        break;
    case WM_APP_STATUS:
    case WM_APP_INFO: {
        auto* text = (std::wstring*)lp;
        SetWindowTextW(ctl(msg == WM_APP_STATUS ? IDC_STATUS : IDC_INFO), text->c_str());
        delete text;
        return 0;
    }
    case WM_APP_ID:
        update_id();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_COPIED) {
            KillTimer(hwnd, TIMER_COPIED);
            SetWindowTextW(ctl(IDC_COPY_ID), L"Copy");
        }
        return 0;
    case WM_APP_SESSIONS: {
        size_t n = (size_t)wp;
        std::wstring t = n == 0 ? L"No one is connected to this desk."
                                : std::to_wstring(n) + (n == 1 ? L" viewer is" : L" viewers are") +
                                      L" connected and can control this computer.";
        SetWindowTextW(ctl(IDC_SESSIONS), t.c_str());
        EnableWindow(ctl(IDC_DISCONNECT), n > 0);
        if (n > 0) FlashWindow(hwnd, TRUE);
        return 0;
    }
    case WM_APP_SETCLIP: {
        auto* text = (std::wstring*)lp;
        clipboard_set(hwnd, *text);
        delete text;
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int show) {
    // One instance per user, so two copies don't fight over the same ID.
    CreateMutexW(nullptr, TRUE, L"Local\\RemoteDeskSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(MAIN_CLASS, nullptr)) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
        return 0;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    load_settings();

    if (!consent_given()) {   // nothing is shared or connected until the user agrees
        WelcomeChoice choice;
        choice.relay = g_relay;
        if (!run_welcome(hi, choice)) return 0;
        set_relay(choice.relay);
        save_consent();
        if (choice.startWithWindows) set_start_with_windows(true);
    } else if (launched_at_startup()) {
        show = SW_SHOWMINNOACTIVE;   // signed in to Windows: stay reachable, out of the way
    }

    g_bg = GetSysColorBrush(COLOR_WINDOW);

    WNDCLASSW wc{};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hi;
    wc.lpszClassName = MAIN_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hbrBackground = g_bg;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, MAIN_CLASS, L"RemoteDesk", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 680, 380, nullptr, nullptr, hi, nullptr);
    ShowWindow(hwnd, show);

    std::thread(control_thread).detach();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ExitProcess(0);   // detached session threads end with the process
}
