// "Update available" prompt, shown once when RemoteDesk starts and our update server
// has a newer version: Update now (download, verify, install, restart) or Later
// (keep working; the sidebar keeps an Update button).
#include "core/app.h"
#include "app/updater.h"
#include "ui/ui.h"

static const wchar_t* PROMPT_CLASS = L"RemoteDeskUpdatePrompt";
enum { IDP_LATER = 10, IDP_UPDATE = 11 };
constexpr int W = 460, PAD = 28;
constexpr int NAV_SETTINGS = 101;   // mw::IDC_NAV_SETTINGS: show the download progress

static HWND g_prompt = nullptr;
static HWND g_owner = nullptr;

HWND update_prompt_window() { return g_prompt; }

static int height(bool notes) { return notes ? 300 : 210; }

static void paint(HWND h) {
    ui::Paint p(h);
    const UINT dpi = GetDpiForWindow(h);
    const ui::Palette P = ui::pal();
    const ui::Fonts& F = ui::fonts(dpi);
    const UpdateStatus u = update_status();
    auto R = [dpi](int x, int y, int w, int hh) { return ui::rc(x, y, w, hh, dpi); };
    ui::fill(p.dc, p.client, P.card);

    RECT ic = R(PAD, 26, 48, 48);
    ui::round_rect(p.dc, ic, ui::px(14, dpi), P.accentSoft);
    ui::glyph(p.dc, ui::ico::Download, ic, F.iconLarge, P.accentText);
    ui::text(p.dc, L"A new version is available", R(PAD + 64, 26, W - PAD * 2 - 64, 26), F.title, P.text);
    ui::text(p.dc, L"RemoteDesk " + widen(u.latest) + L" is ready.  You have " + widen(app_version()) + L".",
             R(PAD + 64, 52, W - PAD * 2 - 64, 22), F.small, P.muted);

    if (!u.notes.empty()) {
        RECT nb = R(PAD, 96, W - PAD * 2, 90);
        ui::round_rect(p.dc, nb, ui::px(12, dpi), P.field);
        ui::text(p.dc, L"What's new", R(PAD + 16, 108, W - PAD * 2 - 32, 18), F.smallBold, P.muted);
        ui::text(p.dc, u.notes, R(PAD + 16, 130, W - PAD * 2 - 32, 48), F.small, P.text,
                 DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

static void close_prompt(HWND h) {
    if (g_owner) EnableWindow(g_owner, TRUE);
    DestroyWindow(h);
}

static LRESULT CALLBACK prompt_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
        case IDP_UPDATE:
            close_prompt(h);
            if (g_owner) PostMessageW(g_owner, WM_COMMAND, NAV_SETTINGS, 0);
            updater_install();
            return 0;
        case IDCANCEL:
        case IDP_LATER:
            close_prompt(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        close_prompt(h);
        return 0;
    case WM_DESTROY:
        g_prompt = nullptr;
        if (g_owner) SetForegroundWindow(g_owner);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void show_update_prompt(HWND owner) {
    if (g_prompt) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = prompt_proc;
        wc.hInstance = hi;
        wc.lpszClassName = PROMPT_CLASS;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
        registered = true;
    }
    g_owner = owner;
    const bool notes = !update_status().notes.empty();
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    g_prompt = CreateWindowExW(WS_EX_DLGMODALFRAME, PROMPT_CLASS, L"RemoteDesk update", style, 0, 0, 0, 0, owner, nullptr, hi,
                               nullptr);
    if (!g_prompt) return;
    const UINT dpi = GetDpiForWindow(g_prompt);
    const int hgt = height(notes);
    ui::style_window(g_prompt);

    const int bw = 150, by = hgt - PAD - 44;
    HWND later = ui::make_control(g_prompt, IDP_LATER, ui::Kind::Secondary, L"Later", 0, ui::Surface::Card);
    HWND upd = ui::make_control(g_prompt, IDP_UPDATE, ui::Kind::Primary, L"Update now", ui::ico::Download, ui::Surface::Card);
    RECT lr = ui::rc(W - PAD - bw * 2 - 12, by, bw, 44, dpi), ur = ui::rc(W - PAD - bw, by, bw, 44, dpi);
    SetWindowPos(later, nullptr, lr.left, lr.top, lr.right - lr.left, lr.bottom - lr.top, SWP_NOZORDER);
    SetWindowPos(upd, nullptr, ur.left, ur.top, ur.right - ur.left, ur.bottom - ur.top, SWP_NOZORDER);

    // Centre on the main window (or the screen while it is minimized).
    RECT r{ 0, 0, ui::px(W, dpi), ui::px(hgt, dpi) };
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT o;
    if (IsIconic(owner) || !GetWindowRect(owner, &o)) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &mi);
        o = mi.rcWork;
    }
    SetWindowPos(g_prompt, HWND_TOP, o.left + (o.right - o.left - ww) / 2, o.top + (o.bottom - o.top - wh) / 2, ww, wh, 0);
    EnableWindow(owner, FALSE);   // modal while it is open
    ShowWindow(g_prompt, SW_SHOW);
    SetFocus(upd);
}
