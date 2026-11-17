// "Someone wants to connect" window on the host: who is asking, the security code to
// compare, and what to allow (control, clipboard, screenshots & recording). Viewing the
// screen is the minimum; Decline (or no answer within 60 s) refuses the connection.
// Runs on the session thread with its own message loop.
#include "core/app.h"
#include "app/service.h"
#include "ui/ui.h"

static const wchar_t* PROMPT_CLASS = L"RemoteDeskAccessPrompt";
enum { IDA_CONTROL = 10, IDA_CLIPBOARD, IDA_RECORD, IDA_DECLINE, IDA_ALLOW, IDA_FILES, IDA_AUDIO };
constexpr int W = 480, H = 574, PAD = 28;
constexpr int N_OPTIONS = 5;
constexpr int TIMEOUT_S = 60;
constexpr UINT_PTR TIMER_COUNTDOWN = 1;

struct AccessPrompt {
    std::wstring who, code;
    int result = -1;   // permissions, or -1 = declined
    int secondsLeft = TIMEOUT_S;
    HWND hwnd = nullptr;
};

static const uint8_t OPTION_BITS[] = { PERM_CONTROL, PERM_CLIPBOARD, PERM_RECORD, PERM_FILES, PERM_AUDIO };
static const struct { int id; wchar_t icon; const wchar_t* title; const wchar_t* detail; bool on; } OPTIONS[] = {
    { IDA_CONTROL, 0xE962, L"Control mouse and keyboard", L"Off = they can only watch", true },
    { IDA_CLIPBOARD, 0xE77F, L"Share the clipboard", L"Copy and paste between the computers", true },
    { IDA_RECORD, 0xE722, L"Screenshots and recording", L"Let them save pictures or video of your screen", false },
    { IDA_FILES, 0xE8B7, L"Send and receive files", L"Files land in the Downloads folder", false },
    { IDA_AUDIO, 0xE767, L"Hear this computer's sound", L"What this PC plays: videos, calls, notifications", false },
};

static void paint(AccessPrompt* a) {
    ui::Paint p(a->hwnd);
    const UINT dpi = GetDpiForWindow(a->hwnd);
    const ui::Palette P = ui::pal();
    const ui::Fonts& F = ui::fonts(dpi);
    auto R = [dpi](int x, int y, int w, int h) { return ui::rc(x, y, w, h, dpi); };
    ui::fill(p.dc, p.client, P.card);

    RECT ic = R(PAD, 26, 52, 52);
    ui::round_rect(p.dc, ic, ui::px(16, dpi), P.warningSoft);
    ui::glyph(p.dc, 0xE7BA /* Warning */, ic, F.iconLarge, P.warning);
    ui::text(p.dc, L"Someone wants to connect", R(PAD + 68, 24, W - PAD * 2 - 68, 28), F.title, P.text);
    ui::text(p.dc, L"From  " + a->who, R(PAD + 68, 52, W - PAD * 2 - 68, 26), F.bodyBold, P.muted);

    RECT code = R(PAD, 96, W - PAD * 2, 44);
    ui::round_rect(p.dc, code, ui::px(12, dpi), P.accentSoft);
    ui::text(p.dc, L"Security code", { code.left + ui::px(16, dpi), code.top, code.right, code.bottom }, F.small, P.accentText);
    ui::text(p.dc, a->code, { code.left, code.top, code.right - ui::px(16, dpi), code.bottom }, F.title, P.accentText,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ui::text(p.dc, L"Only allow people you know. The code must match the one on their screen.",
             R(PAD, 146, W - PAD * 2, 18), F.small, P.faint);

    ui::text(p.dc, L"ALLOW THEM TO", R(PAD, 178, 300, 18), F.smallBold, P.muted);
    ui::text(p.dc, L"See your screen", R(PAD + 56, 202, 300, 24), F.body, P.text);
    ui::text(p.dc, L"Always", R(W - PAD - 100, 202, 100, 24), F.smallBold, P.faint,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    for (int i = 0; i < N_OPTIONS; ++i)
        ui::text(p.dc, OPTIONS[i].detail, R(PAD + 56, 262 + i * 52, W - PAD * 2 - 56, 18), F.small, P.faint);

    ui::text(p.dc, L"Declines automatically in " + std::to_wstring(a->secondsLeft) + L" s",
             R(PAD, H - PAD - 44, 190, 44), F.small, P.faint);
}

static LRESULT CALLBACK prompt_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* a = (AccessPrompt*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!a) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(a);
        return 0;
    case WM_TIMER:
        if (--a->secondsLeft <= 0) { DestroyWindow(h); return 0; }
        { RECT r = ui::rc(PAD, H - PAD - 44, 190, 44, GetDpiForWindow(h)); InvalidateRect(h, &r, FALSE); }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDA_ALLOW: {   // (IDOK is ignored: Enter alone must never let someone in)
            int perms = 0;
            if (ui::checked(GetDlgItem(h, IDA_CONTROL))) perms |= PERM_CONTROL;
            if (ui::checked(GetDlgItem(h, IDA_CLIPBOARD))) perms |= PERM_CLIPBOARD;
            if (ui::checked(GetDlgItem(h, IDA_RECORD))) perms |= PERM_RECORD;
            if (ui::checked(GetDlgItem(h, IDA_FILES))) perms |= PERM_FILES;
            if (ui::checked(GetDlgItem(h, IDA_AUDIO))) perms |= PERM_AUDIO;
            a->result = perms;
            DestroyWindow(h);
            return 0;
        }
        case IDCANCEL:
        case IDA_DECLINE:
            DestroyWindow(h);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, TIMER_COUNTDOWN);
        a->hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int ask_access(const std::string& viewerId, const std::string& code) {
    if (g_agent) return agent_ask(viewerId, code);   // the person's own app shows the prompt
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static std::once_flag reg;
    std::call_once(reg, [hi] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = prompt_proc;
        wc.hInstance = hi;
        wc.lpszClassName = PROMPT_CLASS;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
    });
    AccessPrompt a;
    a.who = widen(format_id(viewerId));
    a.code = widen(code);
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, PROMPT_CLASS, L"RemoteDesk — connection request",
                             style, 0, 0, 0, 0, nullptr, nullptr, hi, &a);
    if (!h) return -1;
    a.hwnd = h;
    const UINT dpi = GetDpiForWindow(h);
    ui::style_window(h);
    for (int i = 0; i < N_OPTIONS; ++i) {
        HWND t = ui::make_control(h, OPTIONS[i].id, ui::Kind::Toggle, OPTIONS[i].title, 0, ui::Surface::Card);
        ui::set_checked(t, (default_perms(false) & OPTION_BITS[i]) != 0);   // Access page defaults
        RECT r = ui::rc(PAD, 236 + i * 52, W - PAD * 2, 28, dpi);
        SetWindowPos(t, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
    }
    HWND dec = ui::make_control(h, IDA_DECLINE, ui::Kind::Secondary, L"Decline", 0, ui::Surface::Card);
    HWND allow = ui::make_control(h, IDA_ALLOW, ui::Kind::Primary, L"Allow", ui::ico::Check, ui::Surface::Card);
    RECT dr = ui::rc(W - PAD - 130 - 12 - 120, H - PAD - 44, 120, 44, dpi), ar = ui::rc(W - PAD - 130, H - PAD - 44, 130, 44, dpi);
    SetWindowPos(dec, nullptr, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top, SWP_NOZORDER);
    SetWindowPos(allow, nullptr, ar.left, ar.top, ar.right - ar.left, ar.bottom - ar.top, SWP_NOZORDER);

    RECT r{ 0, 0, ui::px(W, dpi), ui::px(H, dpi) };
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, dpi);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    SetWindowPos(h, HWND_TOPMOST, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2, ww, wh, 0);
    SetTimer(h, TIMER_COUNTDOWN, 1000, nullptr);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    FlashWindow(h, TRUE);
    SetFocus(dec);   // a stray Enter must not let someone in

    MSG m;
    while (a.hwnd && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return a.result;
}
