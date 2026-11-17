// Main window frame: sidebar (brand, navigation, connection status, update button),
// page switching, theme changes and the messages worker threads post.
// The pages themselves live in home_page.cpp and settings_page.cpp.
#include "screens/main_window.h"
#include "app/service.h"
#include "app/license.h"
#include "app/updater.h"
#include "core/version.h"
#include <windowsx.h>

namespace mw {

State S;
HWND g_wnd = nullptr;

constexpr UINT_PTR TIMER_TICK = 2, TIMER_INFO = 3, TIMER_SESSIONS = 4, TIMER_UPDATE = 5;
constexpr UINT UPDATE_RECHECK_MS = 6 * 60 * 60 * 1000;   // apps that stay open still hear about releases
constexpr Box BOX_NAV_HOME{ 16, 104, SIDEBAR_W - 32, 46 };
constexpr Box BOX_NAV_ACCESS{ 16, 156, SIDEBAR_W - 32, 46 };
constexpr Box BOX_NAV_SETTINGS{ 16, 208, SIDEBAR_W - 32, 46 };
constexpr Box BOX_STATUS{ 16, CH - 132, SIDEBAR_W - 32, 112 };
constexpr Box BOX_RETRY{ 32, CH - 58, 100, 26 };
constexpr Box BOX_UPDATE{ 16, CH - 186, SIDEBAR_W - 32, 42 };

// ------------------------------------------------------------------ helpers
HWND ctl(int id) { return GetDlgItem(g_wnd, id); }

std::wstring get_text(int id) {
    HWND h = ctl(id);
    std::wstring s(GetWindowTextLengthW(h), 0);
    GetWindowTextW(h, &s[0], (int)s.size() + 1);
    return s;
}

void invalidate(const Box& b, int grow) {
    RECT r = R(b);
    InflateRect(&r, P(grow), P(grow));
    InvalidateRect(g_wnd, &r, FALSE);
}

void invalidate_content() {
    RECT r{ P(SIDEBAR_W), 0, P(CW), P(CH) };
    InvalidateRect(g_wnd, &r, FALSE);
}

bool in_rect(const RECT& r, LPARAM lp) {
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    return PtInRect(&r, pt) != FALSE;
}

HWND make_edit(int id, DWORD style, const wchar_t* ph, int limit) {
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style, 0, 0, 0, 0,
                             g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    ui::placeholder(e, ph);
    if (limit) SendMessageW(e, EM_SETLIMITTEXT, limit, 0);
    return e;
}

void place(int id, const Box& b) {
    RECT r = R(b);
    SetWindowPos(ctl(id), nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void place_field(const Field& f) {
    const ui::Fonts& F = ui::fonts(S.dpi);
    RECT b = R(f.box);
    int lh = (f.big ? F.fieldBigH : F.bodyH) + P(2);
    int right = b.right - P(14) - (f.dropId ? P(30) : 0);
    int top = b.top + (b.bottom - b.top - lh) / 2;
    SendMessageW(ctl(f.id), WM_SETFONT, (WPARAM)(f.big ? F.fieldBig : F.body), FALSE);
    SendMessageW(ctl(f.id), EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SetWindowPos(ctl(f.id), nullptr, b.left + P(14), top, right - b.left - P(14), lh, SWP_NOZORDER | SWP_NOACTIVATE);
    if (f.dropId) {
        int s = P(30);
        SetWindowPos(ctl(f.dropId), nullptr, b.right - P(6) - s, b.top + (b.bottom - b.top - s) / 2, s, s,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void paint_field(HDC dc, const ui::Palette& Pl, const Field& f, bool error) {
    const bool focus = GetFocus() == ctl(f.id);
    COLORREF border = error ? Pl.danger : focus ? Pl.focus : Pl.fieldBorder;
    ui::round_rect(dc, R(f.box), P(10), Pl.field, border, std::max(1, P(focus || error ? 2 : 1)));
}

int popup_menu(const Field& f, const std::vector<std::wstring>& items, const std::wstring& empty, const std::wstring& extra) {
    HMENU m = CreatePopupMenu();
    if (items.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, empty.c_str());
    for (size_t i = 0; i < items.size(); ++i) AppendMenuW(m, MF_STRING, i + 1, items[i].c_str());
    if (!extra.empty() && !items.empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 1000, extra.c_str());
    }
    RECT b = R(f.box);
    POINT pt{ b.left, b.bottom + P(4) };
    ClientToScreen(g_wnd, &pt);
    int r = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, g_wnd, nullptr);
    DestroyMenu(m);
    return r == 1000 ? 1000 : r - 1;
}

void card(HDC dc, const ui::Palette& Pl, const Box& b) {
    ui::round_rect(dc, R(b), P(16), Pl.card, Pl.cardBorder);
}

void card_title(HDC dc, const ui::Palette& Pl, const ui::Fonts& F, int x, int y, wchar_t icon, const std::wstring& t) {
    RECT ib = ui::rc(x, y, 34, 34, S.dpi);
    ui::round_rect(dc, ib, P(10), Pl.accentSoft);
    ui::glyph(dc, icon, ib, F.icon, Pl.accentText);
    ui::text(dc, t, ui::rc(x + 46, y, 300, 34, S.dpi), F.title, Pl.text);
}

void pill(HDC dc, RECT r, COLORREF bg, COLORREF fg, const std::wstring& t, HFONT f, bool dot) {
    ui::round_rect(dc, r, (r.bottom - r.top) / 2, bg);
    if (dot) {
        ui::circle(dc, r.left + P(14), (r.top + r.bottom) / 2.0f, (float)P(4), fg);
        r.left += P(24);
        r.right -= P(10);
        ui::text(dc, t, r, f, fg);
    } else {
        ui::text(dc, t, r, f, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

void set_info(InfoKind k, const std::wstring& t) {
    S.infoKind = k;
    S.info = t;
    // The Connect button spins while a connection is being set up.
    const bool busy = k == INFO_BUSY && viewer_connecting();
    ui::set_busy(ctl(IDC_CONNECT), busy, true);   // clickable: it reads "Cancel"
    SetWindowTextW(ctl(IDC_CONNECT), busy ? L"Cancel" : L"Connect");   // clicking it while busy stops connecting
    invalidate_content();
    KillTimer(g_wnd, TIMER_INFO);
    if (!t.empty() && (k == INFO_OK || k == INFO_HINT)) SetTimer(g_wnd, TIMER_INFO, 8000, nullptr);
}

void show_page(Page p) {
    S.page = p;
    ui::set_checked(ctl(IDC_NAV_HOME), p == PAGE_HOME);
    ui::set_checked(ctl(IDC_NAV_ACCESS), p == PAGE_ACCESS);
    ui::set_checked(ctl(IDC_NAV_SETTINGS), p == PAGE_SETTINGS);
    home::set_visible(p == PAGE_HOME);
    access::set_visible(p == PAGE_ACCESS);
    settings::set_visible(p == PAGE_SETTINGS);
    invalidate_content();
}

// ------------------------------------------------------------------ sidebar
static void update_sidebar_controls() {
    ShowWindow(ctl(IDC_RETRY), S.relay == RS_OFFLINE ? SW_SHOWNA : SW_HIDE);
    // The sidebar button appears once an update is found and stays (with its spinner and
    // progress fill) while it downloads and installs.
    const UpdateState us = update_status().state;
    const bool avail = us == UpdateState::Available;
    const bool show = avail || us == UpdateState::Downloading || us == UpdateState::Installing;
    if (show) settings::show_update_state(ctl(IDC_UPDATE_PILL), L"Update");
    ShowWindow(ctl(IDC_UPDATE_PILL), show ? SW_SHOWNA : SW_HIDE);
    ui::set_badge(ctl(IDC_NAV_SETTINGS), avail);
}

static void paint_sidebar(HDC dc, const ui::Palette& Pl, const ui::Fonts& F) {
    RECT sb{ 0, 0, P(SIDEBAR_W), P(CH) };
    ui::fill(dc, sb, Pl.sidebar);
    ui::line(dc, sb.right - 1, 0, sb.right - 1, sb.bottom, Pl.cardBorder);
    if (S.logo) DrawIconEx(dc, P(24), P(28), S.logo, P(36), P(36), 0, nullptr, DI_NORMAL);
    ui::text(dc, L"RemoteDesk", ui::rc(70, 26, 160, 22, S.dpi), F.brand, Pl.text);
    ui::text(dc, L"Version " RD_VERSION_STR, ui::rc(71, 48, 160, 18, S.dpi), F.small, Pl.faint);

    // Connection status block
    const RECT b = R(BOX_STATUS);
    ui::round_rect(dc, b, P(14), Pl.dark ? Pl.card : Pl.bg);
    const COLORREF c = S.relay == RS_ONLINE ? Pl.success : S.relay == RS_CONNECTING ? Pl.warning : Pl.danger;
    const wchar_t* state = S.relay == RS_ONLINE ? L"Online" : S.relay == RS_CONNECTING ? L"Connecting…" : L"Offline";
    ui::circle(dc, b.left + P(22), b.top + P(26), (float)P(5), c);
    ui::text(dc, state, { b.left + P(36), b.top + P(14), b.right - P(12), b.top + P(38) }, F.bodyBold, c);
    ui::text(dc, S.relay == RS_ONLINE ? L"Connected to RemoteDesk" : S.relay == RS_CONNECTING ? L"Reaching RemoteDesk…" : L"Can't reach RemoteDesk",
             { b.left + P(16), b.top + P(42), b.right - P(12), b.top + P(62) }, F.small, Pl.muted);
    if (S.relay == RS_ONLINE) {
        ui::text(dc, L"Ready for connections", { b.left + P(16), b.top + P(70), b.right - P(12), b.top + P(92) }, F.small, Pl.faint);
    } else if (S.relay == RS_OFFLINE) {
        ULONGLONG now = GetTickCount64();
        int secs = S.retryAt > now ? (int)((S.retryAt - now + 999) / 1000) : 0;
        std::wstring t = secs ? L"Retry in " + std::to_wstring(secs) + L"s" : L"Retrying…";
        ui::text(dc, t, { b.left + P(124), b.top + P(70), b.right - P(12), b.top + P(96) }, F.small, Pl.faint,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

// ------------------------------------------------------------------ frame
static void paint(HWND hwnd) {
    ui::Paint p(hwnd);
    const ui::Palette Pl = ui::pal();
    const ui::Fonts& F = ui::fonts(S.dpi);
    ui::fill(p.dc, p.client, Pl.bg);
    paint_sidebar(p.dc, Pl, F);
    if (S.page == PAGE_HOME) home::paint(p.dc, Pl, F);
    else if (S.page == PAGE_ACCESS) access::paint(p.dc, Pl, F);
    else settings::paint(p.dc, Pl, F);
}

static void layout() {
    place(IDC_NAV_HOME, BOX_NAV_HOME);
    place(IDC_NAV_ACCESS, BOX_NAV_ACCESS);
    place(IDC_NAV_SETTINGS, BOX_NAV_SETTINGS);
    place(IDC_RETRY, BOX_RETRY);
    place(IDC_UPDATE_PILL, BOX_UPDATE);
    home::layout();
    access::layout();
    settings::layout();
    if (S.logo) DestroyIcon(S.logo);
    S.logo = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, P(36), P(36), 0);
}

static void size_window(HWND hwnd, bool center) {
    RECT r{ 0, 0, P(CW), P(CH) };
    AdjustWindowRectExForDpi(&r, GetWindowLongW(hwnd, GWL_STYLE), FALSE, 0, S.dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (!center) { SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); return; }
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    SetWindowPos(hwnd, nullptr, wa.left + std::max(0L, (wa.right - wa.left - w) / 2),
                 wa.top + std::max(0L, (wa.bottom - wa.top - h) / 2), w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void apply_theme() {
    HWND hwnd = g_wnd;
    ui::style_window(hwnd);
    ui::set_surface(ctl(IDC_RETRY), ui::pal().dark ? ui::pal().card : ui::pal().bg);
    if (S.fieldBrush) DeleteObject(S.fieldBrush);
    S.fieldBrush = CreateSolidBrush(ui::pal().field);
    home::restyle();
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
    viewers_restyle();
}

static const Field* field_of(int id) {
    if (const Field* f = home::field(id)) return f;
    return settings::field(id);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = g_mainWnd = hwnd;
        S.dpi = GetDpiForWindow(hwnd);
        ui::make_control(hwnd, IDC_NAV_HOME, ui::Kind::Nav, L"Home", ui::ico::Home, ui::Surface::Sidebar);
        ui::make_control(hwnd, IDC_NAV_ACCESS, ui::Kind::Nav, L"Access", ui::ico::Shield, ui::Surface::Sidebar);
        ui::make_control(hwnd, IDC_NAV_SETTINGS, ui::Kind::Nav, L"Settings", ui::ico::Settings, ui::Surface::Sidebar);
        home::create(hwnd);
        access::create(hwnd);
        settings::create(hwnd);
        ui::make_control(hwnd, IDC_UPDATE_PILL, ui::Kind::Primary, L"Update", ui::ico::Download, ui::Surface::Sidebar);
        ui::make_control(hwnd, IDC_RETRY, ui::Kind::Link, L"Retry now", ui::ico::Refresh, ui::Surface::Custom);
        apply_theme();
        layout();
        show_page(PAGE_HOME);
        update_sidebar_controls();
        SetTimer(hwnd, TIMER_UPDATE, UPDATE_RECHECK_MS, nullptr);
        size_window(hwnd, true);
        return 0;
    }
    case WM_DPICHANGED: {
        S.dpi = HIWORD(wp);
        layout();
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        size_window(hwnd, false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        const ui::Palette Pl = ui::pal();
        HDC dc = (HDC)wp;
        SetTextColor(dc, Pl.text);
        SetBkColor(dc, Pl.field);
        return (LRESULT)S.fieldBrush;
    }
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpW((const wchar_t*)lp, L"ImmersiveColorScheme") && ui::refresh_system_theme()) apply_theme();
        return 0;
    case WM_MOUSEMOVE:
        if (S.page == PAGE_HOME) home::mouse_move(lp);
        return 0;
    case WM_MOUSELEAVE:
        if (S.page == PAGE_HOME) home::mouse_move(MAKELPARAM(-1, -1));
        return 0;
    case WM_LBUTTONDOWN:
        if (S.page == PAGE_HOME) home::click(lp);
        else if (S.page == PAGE_ACCESS) access::click(lp);
        else settings::click(lp);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            LPARAM l = MAKELPARAM(pt.x, pt.y);
            if (S.page == PAGE_HOME ? home::set_cursor(l) : S.page == PAGE_ACCESS ? access::set_cursor(l) : settings::set_cursor(l))
                return TRUE;
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (const Field* f = field_of(id))
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) invalidate(f->box);
        switch (id) {
        case IDOK: {   // Enter: act on the field that has focus
            HWND f = GetFocus();
            if (!home::on_enter(f)) settings::on_enter(f);
            return 0;
        }
        case IDC_NAV_HOME: show_page(PAGE_HOME); return 0;
        case IDC_NAV_ACCESS: show_page(PAGE_ACCESS); return 0;
        case IDC_NAV_SETTINGS: show_page(PAGE_SETTINGS); return 0;
        case IDC_RETRY: relay_wake(); return 0;
        case IDC_UPDATE_PILL: show_page(PAGE_SETTINGS); return 0;
        }
        if (home::command(id, code) || access::command(id, code) || settings::command(id, code)) return 0;
        break;
    }
    case WM_APP_RELAY: {
        RelayState st = (RelayState)(wp & 0xFF);
        RelayReason why = (RelayReason)((wp >> 8) & 0xFF);
        if (st == S.relay && why == S.reason && st != RS_OFFLINE) return 0;
        S.relay = st;
        S.reason = why;
        S.retryAt = st == RS_OFFLINE ? GetTickCount64() + (ULONGLONG)lp : 0;
        if (st == RS_OFFLINE) SetTimer(hwnd, TIMER_TICK, 1000, nullptr);
        else KillTimer(hwnd, TIMER_TICK);
        update_sidebar_controls();
        invalidate(BOX_STATUS);
        // Our server just became reachable (e.g. it was started after this app): look for an
        // update now if the start-up check couldn't reach it.
        if (st == RS_ONLINE) {
            license_refresh();   // drop a key that was revoked on the server (once per run)
            UpdateState u = update_status().state;
            if (u == UpdateState::Idle || u == UpdateState::Failed) updater_check(false);
        }
        return 0;
    }
    case WM_APP_INFO: {
        auto* text = (std::wstring*)lp;
        set_info((InfoKind)wp, *text);
        delete text;
        if (wp == INFO_ERROR) FlashWindow(hwnd, TRUE);
        return 0;
    }
    case WM_APP_ID:
        invalidate_content();
        return 0;
    case WM_APP_SESSIONS: {
        size_t before = 0, after = 0;
        for (auto& s : S.sessions) before += s.incoming;
        home::refresh();
        access::refresh();
        session_panel_refresh();
        call_prompts_refresh(hwnd);   // someone viewing this PC is calling
        for (auto& s : S.sessions) after += s.incoming;
        if (after > before) FlashWindow(hwnd, TRUE);
        if (S.sessions.empty()) KillTimer(hwnd, TIMER_SESSIONS);
        else SetTimer(hwnd, TIMER_SESSIONS, 30000, nullptr);
        return 0;
    }
    case WM_APP_NOTE: {
        auto* t = (std::wstring*)lp;
        MessageBoxW(hwnd, t->c_str(), L"RemoteDesk \u2014 Wake-on-LAN", MB_OK | MB_ICONINFORMATION);
        delete t;
        return 0;
    }
    case WM_APP_LICENSE:
        agent_push_license();
        settings::license_changed();
        return 0;
    case WM_APP_HISTORY:
        home::refresh();
        return 0;
    case WM_APP_UPDATE:
        update_sidebar_controls();
        settings::update_changed();
        if (update_status().state == UpdateState::Available && S.promptedVersion != update_status().latest) {
            S.promptedVersion = update_status().latest;   // ask once per version; "Later" leaves the sidebar button
            show_update_prompt(hwnd);
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TICK) invalidate(BOX_STATUS);
        else if (wp == TIMER_UPDATE) updater_check(false);
        else if (wp == TIMER_SESSIONS) { if (S.page != PAGE_SETTINGS) invalidate_content(); }   // session durations
        else if (wp == TIMER_INFO) { KillTimer(hwnd, TIMER_INFO); set_info(INFO_HINT, L""); }
        return 0;
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

}  // namespace mw

HWND create_main_window(HINSTANCE hi, int show) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = mw::main_proc;
    wc.hInstance = hi;
    wc.lpszClassName = L"RemoteDeskMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"RemoteDeskMain", L"RemoteDesk",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1160, 760, nullptr, nullptr, hi, nullptr);
    ShowWindow(hwnd, show);
    return hwnd;
}
