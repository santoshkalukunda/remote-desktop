// First-run screen: the privacy policy, the permissions RemoteDesk needs and
// (Microsoft Store version) Start with Windows. setup.exe asks the same questions
// during installation, so it is skipped there. The relay is ours (core/server_config.h).
#include "core/app.h"
#include "ui/ui.h"
#include <commctrl.h>
#include <windowsx.h>

static const wchar_t* WELCOME_CLASS = L"RemoteDeskWelcome";

enum {
    IDW_POLICY = 300, IDW_STARTUP, IDW_ACCEPT, IDW_DECLINE, IDW_ALL,
    IDW_PERM_FIRST = 320,   // one toggle per PERMISSIONS entry
};

static const wchar_t* PERMISSIONS[] = {
    L"Screen sharing — show this screen to a viewer you accept",
    L"Remote control — let that viewer use this mouse and keyboard",
    L"Clipboard sync — exchange copied text during a session",
    L"Network — stay connected to RemoteDesk so others can reach you",
    L"Keep awake — stop this computer sleeping during a session",
};
constexpr int PERM_COUNT = sizeof(PERMISSIONS) / sizeof(PERMISSIONS[0]);

// Layout in 96-DPI units.
constexpr int W = 600, M = 24, IW = W - 2 * M;
constexpr int POLICY_Y = 98, POLICY_H = 150;
constexpr int PERM_Y = 262, PERM_H = 206;
constexpr int STARTUP_Y = 486;

struct Welcome {
    WelcomeChoice* choice = nullptr;
    bool accepted = false;
    bool packaged = false;
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HBRUSH fieldBrush = nullptr;
    HICON logo = nullptr;
    int buttonsY() const { return packaged ? 532 : 490; }
    int height() const { return buttonsY() + 40 + 24; }
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

static RECT policy_box(UINT dpi) { return ui::rc(M, POLICY_Y, IW, POLICY_H, dpi); }

// Accept is only possible once every permission is on.
static void update_accept(Welcome* w) {
    bool ok = true;
    for (int i = 0; i < PERM_COUNT; ++i) ok = ok && ui::checked(GetDlgItem(w->hwnd, IDW_PERM_FIRST + i));
    EnableWindow(GetDlgItem(w->hwnd, IDW_ACCEPT), ok);
}

static void layout(Welcome* w) {
    const UINT dpi = w->dpi;
    const ui::Fonts& F = ui::fonts(dpi);
    auto place = [&](int id, RECT r) {
        SetWindowPos(GetDlgItem(w->hwnd, id), nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };
    RECT pb = policy_box(dpi);
    InflateRect(&pb, -ui::px(12, dpi), -ui::px(8, dpi));
    pb.right += ui::px(6, dpi);   // scrollbar hugs the border
    place(IDW_POLICY, pb);
    SendMessageW(GetDlgItem(w->hwnd, IDW_POLICY), WM_SETFONT, (WPARAM)F.small, TRUE);
    place(IDW_ALL, ui::rc(M + IW - 16 - 84, PERM_Y + 14, 84, 26, dpi));
    for (int i = 0; i < PERM_COUNT; ++i) place(IDW_PERM_FIRST + i, ui::rc(M + 16, PERM_Y + 50 + i * 30, IW - 32, 28, dpi));
    place(IDW_STARTUP, ui::rc(M, STARTUP_Y, IW, 28, dpi));
    place(IDW_DECLINE, ui::rc(M + IW - 200 - 12 - 110, w->buttonsY(), 110, 40, dpi));
    place(IDW_ACCEPT, ui::rc(M + IW - 200, w->buttonsY(), 200, 40, dpi));

    if (w->logo) DestroyIcon(w->logo);
    w->logo = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, ui::px(36, dpi),
                                ui::px(36, dpi), 0);
}

static void paint(Welcome* w) {
    ui::Paint p(w->hwnd);
    const UINT dpi = w->dpi;
    const ui::Palette P = ui::pal();
    const ui::Fonts& F = ui::fonts(dpi);
    auto R = [dpi](int x, int y, int ww, int hh) { return ui::rc(x, y, ww, hh, dpi); };
    ui::fill(p.dc, p.client, P.bg);

    if (w->logo) DrawIconEx(p.dc, ui::px(M, dpi), ui::px(22, dpi), w->logo, ui::px(36, dpi), ui::px(36, dpi), 0, nullptr, DI_NORMAL);
    ui::text(p.dc, L"Welcome to RemoteDesk", R(M + 50, 18, 480, 28), F.heading, P.text);
    ui::text(p.dc, L"Review the privacy policy and choose what RemoteDesk may do on this computer.",
             R(M + 51, 46, 500, 18), F.small, P.muted);

    ui::text(p.dc, L"Privacy policy", R(M, 76, 300, 18), F.smallBold, P.muted);
    const bool focusPolicy = GetFocus() == GetDlgItem(w->hwnd, IDW_POLICY);
    ui::round_rect(p.dc, policy_box(dpi), ui::px(8, dpi), P.field, focusPolicy ? P.focus : P.fieldBorder,
                   std::max(1, ui::px(focusPolicy ? 2 : 1, dpi)));

    ui::round_rect(p.dc, R(M, PERM_Y, IW, PERM_H), ui::px(12, dpi), P.card, P.cardBorder);
    ui::text(p.dc, L"Allow RemoteDesk to", R(M + 16, PERM_Y + 14, 300, 26), F.title, P.text);
}

static void restyle(Welcome* w) {
    ui::style_window(w->hwnd);
    ui::style_scroll(GetDlgItem(w->hwnd, IDW_POLICY));
    if (w->fieldBrush) DeleteObject(w->fieldBrush);
    w->fieldBrush = CreateSolidBrush(ui::pal().field);
    RedrawWindow(w->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

static void accept(Welcome* w) {
    if (!IsWindowEnabled(GetDlgItem(w->hwnd, IDW_ACCEPT))) return;
    w->choice->startWithWindows = ui::checked(GetDlgItem(w->hwnd, IDW_STARTUP));
    w->accepted = true;
    DestroyWindow(w->hwnd);
}

static LRESULT CALLBACK welcome_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* w = (Welcome*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE: {
        w->hwnd = hwnd;
        w->dpi = GetDpiForWindow(hwnd);
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                        ES_AUTOVSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDW_POLICY, hi, nullptr);
        ui::make_control(hwnd, IDW_ALL, ui::Kind::Link, L"Allow all", ui::ico::Check);
        for (int i = 0; i < PERM_COUNT; ++i)
            ui::make_control(hwnd, IDW_PERM_FIRST + i, ui::Kind::Toggle, PERMISSIONS[i], 0, ui::Surface::Card);
        ui::make_control(hwnd, IDW_STARTUP, ui::Kind::Toggle,
                         L"Start RemoteDesk when I sign in to Windows (keeps this computer reachable)", 0,
                         ui::Surface::Window);
        ui::make_control(hwnd, IDW_DECLINE, ui::Kind::Secondary, L"Decline", 0, ui::Surface::Window);
        ui::make_control(hwnd, IDW_ACCEPT, ui::Kind::Primary, L"Accept and continue", ui::ico::Arrow, ui::Surface::Window);

        SetDlgItemTextW(hwnd, IDW_POLICY, privacy_text(hi).c_str());
        if (!w->packaged) ShowWindow(GetDlgItem(hwnd, IDW_STARTUP), SW_HIDE);   // setup.exe offers its own

        restyle(w);
        layout(w);
        RECT r{ 0, 0, ui::px(W, w->dpi), ui::px(w->height(), w->dpi) };
        AdjustWindowRectExForDpi(&r, GetWindowLongW(hwnd, GWL_STYLE), FALSE, 0, w->dpi);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        SetWindowPos(hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                     mi.rcWork.top + std::max(0L, (mi.rcWork.bottom - mi.rcWork.top - wh) / 2), ww, wh, SWP_NOZORDER);
        return 0;
    }
    case WM_DPICHANGED: {
        w->dpi = HIWORD(wp);
        layout(w);
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(w);
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpW((const wchar_t*)lp, L"ImmersiveColorScheme") && ui::refresh_system_theme()) restyle(w);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {   // the read-only policy text
        const ui::Palette P = ui::pal();
        HDC dc = (HDC)wp;
        SetTextColor(dc, P.muted);
        SetBkColor(dc, P.field);
        return (LRESULT)w->fieldBrush;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDOK:   // Enter
        case IDW_ACCEPT:
            accept(w);
            return 0;
        case IDCANCEL:   // Esc
        case IDW_DECLINE:
            DestroyWindow(hwnd);
            return 0;
        case IDW_ALL:
            for (int i = 0; i < PERM_COUNT; ++i) ui::set_checked(GetDlgItem(hwnd, IDW_PERM_FIRST + i), true);
            update_accept(w);
            return 0;
        case IDW_POLICY:
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        default:
            if (id >= IDW_PERM_FIRST && id < IDW_PERM_FIRST + PERM_COUNT) update_accept(w);
            return 0;
        }
    }
    case WM_DESTROY:
        if (w->fieldBrush) DeleteObject(w->fieldBrush);
        if (w->logo) DestroyIcon(w->logo);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool run_welcome(HINSTANCE hi, WelcomeChoice& choice) {
    Welcome w;
    w.choice = &choice;
    w.packaged = is_packaged();

    WNDCLASSW wc{};
    wc.lpfnWndProc = welcome_proc;
    wc.hInstance = hi;
    wc.lpszClassName = WELCOME_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, WELCOME_CLASS, L"RemoteDesk — Privacy and permissions",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 720, nullptr, nullptr, hi, &w);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    update_accept(&w);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return w.accepted;
}
