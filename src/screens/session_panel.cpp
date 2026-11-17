// Session window on the host: shown when you click someone who is viewing this PC under
// "Active sessions" (or Manage on the Access page). Change what they may do at any time (applies
// at once, the viewer is told), call them, send them files, or disconnect them.
#include "screens/main_window.h"
#include "remote/audio.h"

static const wchar_t* PANEL_CLASS = L"RemoteDeskSessionPanel";
enum { IDS_CONTROL = 10, IDS_CLIPBOARD, IDS_RECORD, IDS_FILES, IDS_SEND, IDS_DISCONNECT, IDS_AUDIO, IDS_CALL, IDS_MUTE };
constexpr int W = 460, H = 640, PAD = 28, ROW = 52, TOGGLES_Y = 158, CALL_Y = TOGGLES_Y + 5 * ROW + 18;

static const struct { int id; uint8_t bit; const wchar_t* title; const wchar_t* detail; } OPTIONS[] = {
    { IDS_CONTROL, PERM_CONTROL, L"Control mouse and keyboard", L"Off = they can only watch" },
    { IDS_CLIPBOARD, PERM_CLIPBOARD, L"Share the clipboard", L"Copy and paste between the computers" },
    { IDS_RECORD, PERM_RECORD, L"Screenshots and recording", L"Turning it off stops a recording in progress" },
    { IDS_FILES, PERM_FILES, L"Send and receive files", L"Turning it off cancels transfers in progress" },
    { IDS_AUDIO, PERM_AUDIO, L"Hear this computer's sound", L"What this PC plays: videos, calls, notifications" },
};
constexpr int N_OPTIONS = sizeof(OPTIONS) / sizeof(OPTIONS[0]);

static HWND g_panel = nullptr;
static int g_key = 0;

static bool find_session(SessionInfo& out) {
    for (const SessionInfo& s : sessions())
        if (s.key == g_key && s.incoming) { out = s; return true; }
    return false;
}

static void sync_controls() {
    SessionInfo s;
    if (!find_session(s)) return;
    for (const auto& o : OPTIONS) {
        HWND t = GetDlgItem(g_panel, o.id);
        if (ui::checked(t) != ((s.perms & o.bit) != 0)) ui::set_checked(t, (s.perms & o.bit) != 0);
    }
    EnableWindow(GetDlgItem(g_panel, IDS_SEND), (s.perms & PERM_FILES) != 0);
    // Call row: Call | Cancel call (ringing them) | Mute + End call (on a call). A call ringing
    // this PC is answered in the call prompt.
    HWND call = GetDlgItem(g_panel, IDS_CALL), mute = GetDlgItem(g_panel, IDS_MUTE);
    const bool active = s.call == CallLink::Active;
    SetWindowTextW(call, s.call == CallLink::Idle ? L"Call" : s.call == CallLink::Calling ? L"Cancel call" : L"End call");
    ui::set_kind(call, s.call == CallLink::Idle ? ui::Kind::Primary : ui::Kind::Danger);
    ui::set_busy(call, s.call == CallLink::Calling, true);
    EnableWindow(call, s.call != CallLink::Ringing);
    ShowWindow(mute, active ? SW_SHOWNA : SW_HIDE);
    SetWindowTextW(mute, s.muted ? L"Unmute" : L"Mute");
    InvalidateRect(g_panel, nullptr, FALSE);
}

static void paint(HWND h) {
    ui::Paint p(h);
    const UINT dpi = GetDpiForWindow(h);
    const ui::Palette P = ui::pal();
    const ui::Fonts& F = ui::fonts(dpi);
    auto R = [dpi](int x, int y, int w, int hh) { return ui::rc(x, y, w, hh, dpi); };
    ui::fill(p.dc, p.client, P.card);
    SessionInfo s;
    if (!find_session(s)) return;

    RECT ic = R(PAD, 26, 52, 52);
    ui::round_rect(p.dc, ic, ui::px(16, dpi), s.recording ? P.dangerSoft : P.warningSoft);
    ui::glyph(p.dc, s.recording ? 0xE7C8 : ui::ico::Eye, ic, F.iconLarge, s.recording ? P.danger : P.warning);
    ui::text(p.dc, widen(format_id(s.peerId)), R(PAD + 68, 24, W - PAD * 2 - 68, 28), F.title, P.text);
    const ULONGLONG mins = (GetTickCount64() - s.since) / 60000;
    const std::wstring since = mins < 1 ? L"just connected" : L"connected for " + std::to_wstring(mins) + L" min";
    const std::wstring transfer = session_transfer_status(s.key);
    if (s.recording)
        ui::text(p.dc, L"● Recording your screen · " + since, R(PAD + 68, 52, W - PAD * 2 - 68, 22), F.smallBold, P.danger);
    else if (!transfer.empty())
        ui::text(p.dc, transfer, R(PAD + 68, 52, W - PAD * 2 - 68, 22), F.smallBold, P.accentText);
    else
        ui::text(p.dc, L"Is viewing this PC · " + since, R(PAD + 68, 52, W - PAD * 2 - 68, 22), F.small, P.muted);

    ui::text(p.dc, L"WHAT THEY CAN DO — CHANGES APPLY AT ONCE", R(PAD, 100, W - PAD * 2, 18), F.smallBold, P.muted);
    ui::text(p.dc, L"See your screen", R(PAD + 56, 124, 300, 24), F.body, P.text);
    ui::text(p.dc, L"Always", R(W - PAD - 100, 124, 100, 24), F.smallBold, P.faint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    for (int i = 0; i < N_OPTIONS; ++i)
        ui::text(p.dc, OPTIONS[i].detail, R(PAD + 56, TOGGLES_Y + 26 + i * ROW, W - PAD * 2 - 56, 18), F.small, P.faint);

    ui::text(p.dc, L"VOICE CALL", R(PAD, CALL_Y, 200, 18), F.smallBold, P.muted);
    const std::wstring st = call_state_text(s.call, s.muted);
    ui::text(p.dc, st.empty() ? std::wstring(L"Talk with them while they work") : st, R(PAD + 110, CALL_Y, W - PAD * 2 - 110, 18),
             s.call == CallLink::Active ? F.smallBold : F.small, s.call == CallLink::Active ? P.success : P.faint,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        bool isToggle = false;
        for (const auto& o : OPTIONS) isToggle |= o.id == id;
        if (isToggle) {   // a switch was flipped: apply them all at once
            uint8_t perms = 0;
            for (const auto& o : OPTIONS)
                if (ui::checked(GetDlgItem(h, o.id))) perms |= o.bit;
            if (!session_set_perms(g_key, perms)) DestroyWindow(h);   // the session has ended
            return 0;
        }
        SessionInfo s;
        if (id == IDS_CALL) {
            if (find_session(s)) {
                if (s.call == CallLink::Idle && !ensure_microphone(h)) return 0;
                session_call(g_key, s.call == CallLink::Idle ? CALL_DO_START : CALL_DO_HANGUP);
            }
            return 0;
        }
        if (id == IDS_MUTE) {
            if (find_session(s)) session_call(g_key, s.muted ? CALL_DO_UNMUTE : CALL_DO_MUTE);
            return 0;
        }
        if (id == IDS_SEND) {
            if (find_session(s)) mw::home::send_files_to(s.key, s.peerId);
            return 0;
        }
        if (id == IDS_DISCONNECT) {
            session_end(g_key);
            DestroyWindow(h);
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_panel = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void show_session_panel(HWND owner, int key) {
    if (g_panel) DestroyWindow(g_panel);   // one at a time
    g_key = key;
    SessionInfo s;
    if (!find_session(s)) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = panel_proc;
        wc.hInstance = hi;
        wc.lpszClassName = PANEL_CLASS;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
        registered = true;
    }
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    const std::wstring title = L"Session with " + widen(format_id(s.peerId));
    g_panel = CreateWindowExW(WS_EX_DLGMODALFRAME, PANEL_CLASS, title.c_str(), style, 0, 0, 0, 0, owner, nullptr, hi, nullptr);
    if (!g_panel) return;
    const UINT dpi = GetDpiForWindow(g_panel);
    ui::style_window(g_panel);
    auto place = [&](HWND c, int x, int y, int w, int hh) {
        RECT r = ui::rc(x, y, w, hh, dpi);
        SetWindowPos(c, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
    };
    for (int i = 0; i < N_OPTIONS; ++i)
        place(ui::make_control(g_panel, OPTIONS[i].id, ui::Kind::Toggle, OPTIONS[i].title, 0, ui::Surface::Card),
              PAD, TOGGLES_Y + i * ROW, W - PAD * 2, 28);
    const int bw = (W - PAD * 2 - 12) / 2;
    place(ui::make_control(g_panel, IDS_CALL, ui::Kind::Primary, L"Call", 0xE717, ui::Surface::Card), PAD, CALL_Y + 28, bw, 44);
    place(ui::make_control(g_panel, IDS_MUTE, ui::Kind::Secondary, L"Mute", 0xE720, ui::Surface::Card), PAD + bw + 12, CALL_Y + 28, bw, 44);
    place(ui::make_control(g_panel, IDS_SEND, ui::Kind::Secondary, L"Send files…", 0xE8B7, ui::Surface::Card),
          PAD, H - PAD - 44, bw, 44);
    place(ui::make_control(g_panel, IDS_DISCONNECT, ui::Kind::Danger, L"Disconnect", ui::ico::Close, ui::Surface::Card),
          PAD + bw + 12, H - PAD - 44, bw, 44);
    sync_controls();

    RECT r{ 0, 0, ui::px(W, dpi), ui::px(H, dpi) };
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT o;
    GetWindowRect(owner, &o);
    SetWindowPos(g_panel, HWND_TOP, o.left + (o.right - o.left - ww) / 2, std::max<int>(0, o.top + (o.bottom - o.top - wh) / 2), ww, wh, 0);
    ShowWindow(g_panel, SW_SHOW);
}

void session_panel_refresh() {
    if (!g_panel) return;
    SessionInfo s;
    if (!find_session(s)) { DestroyWindow(g_panel); return; }   // they disconnected
    sync_controls();
}
