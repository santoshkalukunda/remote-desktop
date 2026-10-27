// First-run screen: the privacy policy, the permissions RemoteDesk needs, the
// relay server and (Microsoft Store version) Start with Windows. setup.exe
// asks the same questions during installation, so it is skipped there.
#include "app.h"

static const wchar_t* WELCOME_CLASS = L"RemoteDeskWelcome";

enum {
    IDW_POLICY = 300, IDW_RELAY, IDW_STARTUP, IDW_ACCEPT, IDW_DECLINE,
    IDW_PERM_FIRST = 320,   // one checkbox per PERMISSIONS entry
    IDW_STATIC_BASE = 400,
};

static const wchar_t* PERMISSIONS[] = {
    L"Screen sharing: show this computer's screen to a viewer you accept",
    L"Remote control: let that viewer use the mouse and keyboard on this computer",
    L"Clipboard sync: exchange copied text with the other computer during a session",
    L"Network: stay connected to the relay server so others can reach this computer",
    L"Keep awake: stop this computer from sleeping while a session is running",
};
constexpr int PERM_COUNT = sizeof(PERMISSIONS) / sizeof(PERMISSIONS[0]);

enum Font { F_NORMAL, F_TITLE, F_COUNT };

struct CtlDef {
    int id;
    const wchar_t* cls;
    const wchar_t* text;
    DWORD style;
    int x, y, w, h;
    Font font;
};

static const CtlDef CONTROLS[] = {
    { 0, L"STATIC", L"Welcome to RemoteDesk", SS_NOPREFIX, 16, 12, 528, 30, F_TITLE },
    { 0, L"STATIC", L"Please read the privacy policy. It explains what RemoteDesk shares, and with whom.",
      0, 16, 46, 528, 20, F_NORMAL },
    { IDW_POLICY, L"EDIT", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY, 16, 68, 528, 200, F_NORMAL },
    { 0, L"STATIC", L"Allow RemoteDesk to:", 0, 16, 280, 528, 20, F_NORMAL },
    { IDW_PERM_FIRST + 0, L"BUTTON", PERMISSIONS[0], WS_TABSTOP | BS_AUTOCHECKBOX, 16, 302, 528, 22, F_NORMAL },
    { IDW_PERM_FIRST + 1, L"BUTTON", PERMISSIONS[1], WS_TABSTOP | BS_AUTOCHECKBOX, 16, 326, 528, 22, F_NORMAL },
    { IDW_PERM_FIRST + 2, L"BUTTON", PERMISSIONS[2], WS_TABSTOP | BS_AUTOCHECKBOX, 16, 350, 528, 22, F_NORMAL },
    { IDW_PERM_FIRST + 3, L"BUTTON", PERMISSIONS[3], WS_TABSTOP | BS_AUTOCHECKBOX, 16, 374, 528, 22, F_NORMAL },
    { IDW_PERM_FIRST + 4, L"BUTTON", PERMISSIONS[4], WS_TABSTOP | BS_AUTOCHECKBOX, 16, 398, 528, 22, F_NORMAL },
    { 0, L"STATIC", L"Relay server (host:port, the same on every computer):", 0, 16, 432, 528, 20, F_NORMAL },
    { IDW_RELAY, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 16, 454, 300, 24, F_NORMAL },
    { IDW_STARTUP, L"BUTTON", L"Start RemoteDesk when I sign in to Windows (optional, keeps this computer reachable)",
      WS_TABSTOP | BS_AUTOCHECKBOX, 16, 490, 528, 22, F_NORMAL },
    { IDW_DECLINE, L"BUTTON", L"Decline", WS_TABSTOP, 300, 528, 100, 30, F_NORMAL },
    { IDW_ACCEPT, L"BUTTON", L"Accept and continue", WS_TABSTOP | BS_DEFPUSHBUTTON | WS_DISABLED, 408, 528, 136, 30, F_NORMAL },
};
constexpr int N_CONTROLS = sizeof(CONTROLS) / sizeof(CONTROLS[0]);
constexpr int CLIENT_W = 560, CLIENT_H = 572;

struct Welcome {
    WelcomeChoice* choice = nullptr;
    bool accepted = false;
    HWND hwnd = nullptr;
    HWND ctl[N_CONTROLS] = {};
    HFONT fonts[F_COUNT] = {};
    HBRUSH bg = nullptr;
};

static std::wstring privacy_text(HINSTANCE hi) {
    HRSRC r = FindResourceW(hi, MAKEINTRESOURCEW(IDR_PRIVACY), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(hi, r) : nullptr;
    const char* p = g ? (const char*)LockResource(g) : nullptr;
    if (!p) return L"The privacy policy could not be loaded. See PRIVACY.txt.";
    std::string text(p, SizeofResource(hi, r));
    std::string out;   // an EDIT control needs CRLF line breaks
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') out += '\r';
        out += c;
    }
    return widen(out);
}

static void layout(Welcome* w, UINT dpi) {
    struct { int pt; int weight; } spec[F_COUNT] = { { 9, FW_NORMAL }, { 14, FW_SEMIBOLD } };
    for (int i = 0; i < F_COUNT; ++i) {
        if (w->fonts[i]) DeleteObject(w->fonts[i]);
        w->fonts[i] = CreateFontW(-MulDiv(spec[i].pt, dpi, 72), 0, 0, 0, spec[i].weight, 0, 0, 0, DEFAULT_CHARSET,
                                  0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
    for (int i = 0; i < N_CONTROLS; ++i) {
        const CtlDef& c = CONTROLS[i];
        SetWindowPos(w->ctl[i], nullptr, MulDiv(c.x, dpi, 96), MulDiv(c.y, dpi, 96), MulDiv(c.w, dpi, 96),
                     MulDiv(c.h, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(w->ctl[i], WM_SETFONT, (WPARAM)w->fonts[c.font], TRUE);
    }
}

static std::wstring relay_text(HWND hwnd) {
    HWND e = GetDlgItem(hwnd, IDW_RELAY);
    std::wstring s(GetWindowTextLengthW(e), 0);
    GetWindowTextW(e, &s[0], (int)s.size() + 1);
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// Accept is only possible once every permission is ticked and a relay is entered.
static void update_accept(HWND hwnd) {
    bool ok = !relay_text(hwnd).empty();
    for (int i = 0; i < PERM_COUNT; ++i)
        ok = ok && IsDlgButtonChecked(hwnd, IDW_PERM_FIRST + i) == BST_CHECKED;
    EnableWindow(GetDlgItem(hwnd, IDW_ACCEPT), ok);
}

static LRESULT CALLBACK welcome_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* w = (Welcome*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE: {
        w->hwnd = hwnd;
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        for (int i = 0; i < N_CONTROLS; ++i) {
            const CtlDef& c = CONTROLS[i];
            int id = c.id ? c.id : IDW_STATIC_BASE + i;
            w->ctl[i] = CreateWindowExW(0, c.cls, c.text, WS_CHILD | WS_VISIBLE | c.style, 0, 0, 0, 0, hwnd,
                                        (HMENU)(INT_PTR)id, hi, nullptr);
        }
        SetDlgItemTextW(hwnd, IDW_POLICY, privacy_text(hi).c_str());
        SetDlgItemTextW(hwnd, IDW_RELAY, widen(w->choice->relay).c_str());
        if (!is_packaged()) ShowWindow(GetDlgItem(hwnd, IDW_STARTUP), SW_HIDE);   // setup.exe offers its own

        UINT dpi = GetDpiForWindow(hwnd);
        layout(w, dpi);
        RECT r{ 0, 0, MulDiv(CLIENT_W, dpi, 96), MulDiv(CLIENT_H, dpi, 96) };
        AdjustWindowRectExForDpi(&r, GetWindowLongW(hwnd, GWL_STYLE), FALSE, 0, dpi);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        SetWindowPos(hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                     mi.rcWork.top + std::max(0L, (mi.rcWork.bottom - mi.rcWork.top - wh) / 2), ww, wh, SWP_NOZORDER);
        return 0;
    }
    case WM_DPICHANGED: {
        layout(w, HIWORD(wp));
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {   // labels, checkboxes and the read-only policy text
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)w->bg;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:   // Enter
        case IDW_ACCEPT: {
            if (!IsWindowEnabled(GetDlgItem(hwnd, IDW_ACCEPT))) return 0;
            std::wstring relay = relay_text(hwnd);
            if (relay.find(L' ') != std::wstring::npos) {
                MessageBoxW(hwnd, L"Enter the relay server as host:port, for example relay.example.com:21116.",
                            L"RemoteDesk", MB_ICONWARNING);
                return 0;
            }
            w->choice->relay = narrow(relay);
            w->choice->startWithWindows = IsDlgButtonChecked(hwnd, IDW_STARTUP) == BST_CHECKED;
            w->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        case IDCANCEL:   // Esc
        case IDW_DECLINE:
            DestroyWindow(hwnd);
            return 0;
        case IDW_RELAY:
            if (HIWORD(wp) == EN_CHANGE) update_accept(hwnd);
            return 0;
        default:
            if (LOWORD(wp) >= IDW_PERM_FIRST && LOWORD(wp) < IDW_PERM_FIRST + PERM_COUNT) update_accept(hwnd);
            return 0;
        }
    case WM_DESTROY:
        for (HFONT f : w->fonts) if (f) DeleteObject(f);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool run_welcome(HINSTANCE hi, WelcomeChoice& choice) {
    Welcome w;
    w.choice = &choice;
    w.bg = GetSysColorBrush(COLOR_WINDOW);

    WNDCLASSW wc{};
    wc.lpfnWndProc = welcome_proc;
    wc.hInstance = hi;
    wc.lpszClassName = WELCOME_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hbrBackground = w.bg;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, WELCOME_CLASS, L"RemoteDesk - Privacy and permissions",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 600, 640, nullptr, nullptr, hi, &w);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    update_accept(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return w.accepted;
}
