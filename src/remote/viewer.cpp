// Viewer side: one window + thread per outgoing connection.
//
// Window layout: a toolbar (who you're connected to, security code, what the host allowed,
// screen picker, screenshot, full screen, Disconnect) above the remote picture. In full
// screen (F11 or Alt+Enter) the picture fills the monitor and the toolbar slides in when
// the pointer touches the top edge. Full screen and screenshots are RemoteDesk Pro features;
// screenshots also need the host's "Screenshots and recording" permission.
// Release 2 adds recording (MP4 in VideosRemoteDesk; the host is told) and a Files panel
// (send by button or drag and drop; received files go to DownloadsRemoteDesk), both Pro and
// both subject to the host's permissions.
#include "core/app.h"
#include "core/crypto.h"
#include "core/codec.h"
#include "app/history.h"
#include "app/license.h"
#include "remote/files.h"
#include "remote/audio.h"
#include "remote/recorder.h"
#include "ui/ui.h"
#include <windowsx.h>
#include <commdlg.h>
#include <chrono>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <set>
#include <thread>

constexpr UINT WM_V_RESIZE  = WM_APP + 50;   // wParam = w, lParam = h (sent)
constexpr UINT WM_V_SETCLIP = WM_APP + 51;   // lParam = std::wstring*
constexpr UINT WM_V_LOST    = WM_APP + 52;
constexpr UINT WM_V_SCREENS = WM_APP + 53;   // lParam = const std::vector<uint8_t>* MSG_MONITORS payload (sent)
constexpr UINT WM_V_THEME   = WM_APP + 54;
constexpr UINT WM_V_TOAST   = WM_APP + 55;   // wParam = 1 if error, lParam = std::wstring* (receiver deletes)
constexpr UINT WM_V_FILES   = WM_APP + 56;   // the transfer list changed
constexpr UINT WM_V_FILEDONE = WM_APP + 57;  // wParam = 1 if failed, lParam = Transfer* (receiver deletes)
constexpr UINT WM_V_RECDONE = WM_APP + 58;   // wParam = 1 if failed, lParam = std::wstring* path or error
constexpr UINT WM_V_PERMS   = WM_APP + 59;   // wParam = new AccessPerm bits from the host
constexpr UINT WM_V_CALL    = WM_APP + 62;   // the call's state or mute changed
constexpr UINT WM_V_NOTE    = WM_APP + 63;   // lParam = std::wstring* (receiver deletes): a short message
constexpr wchar_t ICO_PAUSE = 0xE769, ICO_PLAY = 0xE768;
constexpr int IDV_SOUND = 2009, IDV_CALL = 2010, IDV_MUTE = 2011, IDV_SCREENSMENU = 2012;
constexpr wchar_t ICO_SPEAKER = 0xE767, ICO_SPEAKER_OFF = 0xE74F, ICO_PHONE = 0xE717, ICO_HANGUP = 0xE778, ICO_MIC = 0xE720;
constexpr UINT WM_V_LOCKED  = WM_APP + 61;   // wParam 1/0: the host's screen is locked and out of reach
constexpr UINT WM_V_PRO     = WM_APP + 60;   // the host's license arrived: relayout (PRO badge)
constexpr int IDV_SCREENS = 2001, IDV_DISCONNECT = 2002, IDV_SHOT = 2003, IDV_FULL = 2004,   // toolbar controls
              IDV_REC = 2005, IDV_FILES = 2006, IDV_SEND = 2007, IDV_OPEN = 2008;
constexpr int TOOLBAR_H = 56;                              // 96-DPI units
constexpr int PANEL_W = 340;                               // Files panel, 96-DPI units
constexpr UINT_PTR TIMER_TOAST = 1, TIMER_REC = 2, TIMER_PING = 3;
constexpr int REC_FPS = 15;
constexpr wchar_t ICO_CAMERA = 0xE722, ICO_FULL = 0xE740, ICO_WINDOW = 0xE73F, ICO_REC = 0xE7C8, ICO_FILES = 0xE8B7,
                  ICO_UP = 0xE74A, ICO_DOWN = 0xE74B, ICO_STOP = 0xE71A;
static const wchar_t* VIEWER_CLASS = L"RemoteDeskViewer";

struct Viewer {
    explicit Viewer(SOCKET s) : ch(s) {}
    SecureChannel ch;
    std::wstring remoteId;
    std::string peerId;            // digits, for file names
    HWND hwnd = nullptr;

    std::mutex mx;                 // guards dib/bits/rw/rh/dst/dirty
    HBITMAP dib = nullptr;
    uint8_t* bits = nullptr;       // top-down BGRA
    int rw = 0, rh = 0;
    bool sized = false;

    RECT dst{};                    // remote image area in client coords
    RECT dirty{};                  // changed area of the current frame, remote coords
    std::set<uint16_t> keysDown;   // released if we lose focus
    unsigned buttons = 0;
    std::mutex clipMx;
    std::wstring lastClip;
    bool closing = false;

    std::vector<MonitorDesc> screens;   // remote monitors (UI thread)
    int screen = 0;                     // shown monitor index, -1 = all screens
    std::wstring code;                  // security code, shown in the toolbar
    std::atomic<uint32_t> peerPro{ 0 };          // Pro features unlocked by the host's license
    std::atomic<ULONGLONG> lastRx{ GetTickCount64() };   // anything from the host (heartbeat)
    std::atomic<bool> pinged{ false };           // the host sends MSG_PING (1.4.1+): silence means it's gone
    std::atomic<uint8_t> perms{ PERM_LEGACY };   // what the host allows now (AccessPerm bits; it can change them)
    HWND picker = nullptr, disconnect = nullptr, shot = nullptr, full = nullptr;   // toolbar controls

    bool fullscreen = false, revealed = false;   // full screen; toolbar slid in
    WINDOWPLACEMENT placement{ sizeof(WINDOWPLACEMENT) };
    std::wstring toast, lastShot;       // message in the toolbar; file a click on it reveals
    bool toastError = false;
    RECT toastRc{};
    bool remoteLocked = false;          // the host is locked and has no service: say so

    // Files panel and transfers (release 2)
    std::unique_ptr<FileEngine> files;
    // Sound and calls (remote/audio.h)
    std::unique_ptr<SoundReceiver> sound;
    std::unique_ptr<CallLink> call;
    bool listening = false;             // asked the host for its sound
    HWND soundBtn = nullptr, callBtn = nullptr, muteBtn = nullptr, callPrompt = nullptr;
    bool panel = false;
    bool compactPicker = false;         // short screen labels when the toolbar is tight
    bool pickerMenu = false;            // too tight even for that: a Screens button with a menu instead
    bool shortCode = false;             // tight: the security code without its label
    std::wstring pickerSig;             // the picker's current options (rebuilt only when they change)
    HWND screensBtn = nullptr;
    HWND recBtn = nullptr, filesBtn = nullptr, sendBtn = nullptr, openBtn = nullptr;
    std::vector<std::pair<RECT, std::pair<uint32_t, bool>>> cancelHits;   // x buttons: id, outgoing
    struct PauseHit { RECT r; uint32_t id; bool outgoing, resume; };
    std::vector<PauseHit> pauseHits;    // pause / resume buttons

    // Recording (release 2): a thread copies the picture REC_FPS times a second into an MP4.
    std::atomic<bool> recording{ false };
    std::thread recThread;
    ULONGLONG recStart = 0;
};

static int toolbar_px(Viewer* v) { return ui::px(TOOLBAR_H, GetDpiForWindow(v->hwnd)); }
static bool toolbar_visible(Viewer* v) { return !v->fullscreen || v->revealed; }
// Height the picture leaves free at the top (none in full screen: the toolbar floats over it).
static int reserved_top(Viewer* v) { return v->fullscreen ? 0 : toolbar_px(v); }
// Height of the area where the pointer belongs to the toolbar, not the remote computer.
static int toolbar_hit(Viewer* v) { return toolbar_visible(v) ? toolbar_px(v) : 0; }
static bool can_control(Viewer* v) { return (v->perms & PERM_CONTROL) != 0; }

// ---- input forwarding ----
static void send_key(Viewer* v, uint16_t vk, uint16_t scan, bool down, bool ext) {
    if (!can_control(v)) return;   // view only (the host ignores it anyway)
    KeyEvent k{ vk, scan, (uint8_t)down, (uint8_t)ext };
    if (down) v->keysDown.insert(vk); else v->keysDown.erase(vk);
    v->ch.send(MSG_KEY, &k, sizeof(k));
}

static void release_all(Viewer* v) {
    auto keys = v->keysDown;
    for (uint16_t vk : keys)
        send_key(v, vk, (uint16_t)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), false, false);
    for (uint8_t b = 0; b < 3; ++b)
        if (v->buttons & (1u << b)) {
            MouseBtn mb{ b, 0 };
            v->ch.send(MSG_MOUSE_BTN, &mb, sizeof(mb));
        }
    v->buttons = 0;
}

static bool to_remote(Viewer* v, LPARAM lp, MouseMove& m) {
    int dw = v->dst.right - v->dst.left, dh = v->dst.bottom - v->dst.top;
    if (!can_control(v) || dw <= 0 || dh <= 0 || v->rw <= 0) return false;
    // Over the toolbar the pointer is ours, unless a button is held (dragging off the picture).
    if (!v->buttons && GET_Y_LPARAM(lp) < toolbar_hit(v)) return false;
    m.x = std::clamp((int)((int64_t)(GET_X_LPARAM(lp) - v->dst.left) * v->rw / dw), 0, v->rw - 1);
    m.y = std::clamp((int)((int64_t)(GET_Y_LPARAM(lp) - v->dst.top) * v->rh / dh), 0, v->rh - 1);
    return true;
}

// Low-level hook so Win, Alt+Tab, Ctrl+Esc etc. go to the remote machine
// instead of the local shell while a viewer window is focused.
static thread_local Viewer* tl_viewer = nullptr;

// Pro on either computer unlocks the session (the host's license arrives as MSG_PRO).
static bool pro_has(Viewer* v, uint32_t f) { return license_has(f) || (v->peerPro & f) == f; }
static bool pro_session(Viewer* v) { return license_info().active || v->peerPro != 0; }

static LRESULT CALLBACK kb_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && tl_viewer && can_control(tl_viewer) && GetForegroundWindow() == tl_viewer->hwnd) {
        auto* kb = (KBDLLHOOKSTRUCT*)lp;
        bool alt = (kb->flags & LLKHF_ALTDOWN) != 0;
        bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
        DWORD vk = kb->vkCode;
        if (vk == VK_END && alt && ctrl && !(kb->flags & LLKHF_INJECTED)) {   // Ctrl+Alt+End = Ctrl+Alt+Del over there
            if (!(kb->flags & LLKHF_UP)) tl_viewer->ch.send(MSG_SAS, nullptr, 0);
            return 1;
        }
        bool special = vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS ||
                       (vk == VK_TAB && alt) || (vk == VK_ESCAPE && (alt || ctrl));
        if (special && !(kb->flags & LLKHF_INJECTED)) {
            send_key(tl_viewer, (uint16_t)vk, (uint16_t)kb->scanCode, !(kb->flags & LLKHF_UP),
                     (kb->flags & LLKHF_EXTENDED) != 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ---- toolbar ----
// Toolbar plus the strip below it where messages float.
static void invalidate_top(Viewer* v) {
    RECT r{ 0, 0, 1 << 16, toolbar_px(v) + ui::px(64, GetDpiForWindow(v->hwnd)) };
    InvalidateRect(v->hwnd, &r, FALSE);
}

static void toast(Viewer* v, const std::wstring& text, bool error = false) {
    v->toast = text;
    v->toastError = error;
    KillTimer(v->hwnd, TIMER_TOAST);
    SetTimer(v->hwnd, TIMER_TOAST, error ? 6000 : 5000, nullptr);
    invalidate_top(v);
}

// The picker lists each remote monitor plus "All screens"; it only shows when there is
// more than one monitor to choose from.
static std::wstring screen_label(Viewer* v, size_t i) {
    return v->screens[i].primary ? std::wstring(L"Main") : L"Screen " + std::to_wstring(i + 1);
}

static void update_picker(Viewer* v) {
    if (!v->picker) return;
    const bool multi = v->screens.size() > 1;
    if (multi) {
        std::vector<std::pair<wchar_t, std::wstring>> opts;
        std::wstring sig = v->compactPicker ? L"c" : L"f";
        for (size_t i = 0; i < v->screens.size(); ++i) {
            opts.push_back({ ui::ico::Monitor, v->compactPicker ? std::to_wstring(i + 1) : screen_label(v, i) });
            sig += L"|" + opts.back().second;
        }
        opts.push_back({ ui::ico::System, v->compactPicker ? L"All" : L"All screens" });
        // Only when the options really change: rebuilding them repaints (and flickers) the picker.
        if (sig != v->pickerSig) { v->pickerSig = sig; ui::set_options(v->picker, opts); }
        const int sel = v->screen < 0 ? (int)v->screens.size() : v->screen;
        if (ui::selected(v->picker) != sel) ui::set_selected(v->picker, sel);
    }
    const bool show = multi && toolbar_visible(v);
    ShowWindow(v->picker, show && !v->pickerMenu ? SW_SHOWNA : SW_HIDE);
    ShowWindow(v->screensBtn, show && v->pickerMenu ? SW_SHOWNA : SW_HIDE);
}

// Too narrow for the picker: the Screens button opens the same choices as a menu.
static void screens_menu(Viewer* v) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < v->screens.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == v->screen ? MF_CHECKED : 0), 1 + i, screen_label(v, i).c_str());
    AppendMenuW(m, MF_STRING | (v->screen < 0 ? MF_CHECKED : 0), 1000, L"All screens");
    RECT r;
    GetWindowRect(v->screensBtn, &r);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, r.left, r.bottom + 4, 0, v->hwnd, nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    int32_t want = cmd == 1000 ? -1 : cmd - 1;
    if (want != v->screen) v->ch.send(MSG_SELECT_SCREEN, &want, sizeof(want));   // host answers with MSG_MONITORS
}

// Moves a control only if it actually moved: repositioning repaints it, and doing that on every
// update (toasts, call state, timers) is what made the toolbar flicker.
static void place_ctl(HWND h, int x, int y, int w, int hh) {
    if (!h) return;
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(nullptr, GetParent(h), (POINT*)&r, 2);
    if (r.left == x && r.top == y && r.right - r.left == w && r.bottom - r.top == hh) return;
    SetWindowPos(h, nullptr, x, y, w, hh, SWP_NOACTIVATE | SWP_NOZORDER);
}

// The security code pill: the full label when there's room, just the code when the toolbar is tight.
static std::wstring code_label(Viewer* v) { return v->shortCode ? v->code : L"Security code  " + v->code; }

static int panel_px(Viewer* v) { return v->panel ? ui::px(PANEL_W, GetDpiForWindow(v->hwnd)) : 0; }

static void show_toolbar_controls(Viewer* v) {
    const int show = toolbar_visible(v) ? SW_SHOWNA : SW_HIDE;
    for (HWND h : { v->shot, v->full, v->disconnect, v->recBtn, v->filesBtn, v->soundBtn, v->callBtn }) ShowWindow(h, show);
    const bool onCall = v->call && v->call->state() == CallLink::Active;
    ShowWindow(v->muteBtn, toolbar_visible(v) && onCall ? SW_SHOWNA : SW_HIDE);
    for (HWND h : { v->sendBtn, v->openBtn }) ShowWindow(h, v->panel ? SW_SHOWNA : SW_HIDE);
    update_picker(v);
}

static RECT pill_rect(HDC dc, int x, int tb, const std::wstring& t, HFONT f, UINT dpi);

// Right edge of the address, security code and view-only badge (where the picker may start).
static int left_content_end(Viewer* v) {
    const UINT dpi = GetDpiForWindow(v->hwnd);
    const ui::Fonts& F = ui::fonts(dpi);
    HDC dc = GetDC(v->hwnd);
    const int tb = toolbar_px(v);
    int x = ui::px(40, dpi) + ui::text_width(dc, v->remoteId, F.title) + ui::px(16, dpi);
    x = pill_rect(dc, x, tb, code_label(v), F.smallBold, dpi).right + ui::px(8, dpi);
    if (!can_control(v)) x = pill_rect(dc, x, tb, L"View only", F.smallBold, dpi).right + ui::px(8, dpi);
    // (the PRO badge is drawn only where it fits, so it takes no room from the picker)
    ReleaseDC(v->hwnd, dc);
    return x + ui::px(8, dpi);
}

static void layout_toolbar(Viewer* v) {
    if (!v->disconnect) return;
    const UINT dpi = GetDpiForWindow(v->hwnd);
    auto P = [dpi](int x) { return ui::px(x, dpi); };
    RECT rc;
    GetClientRect(v->hwnd, &rc);
    const int tb = toolbar_px(v), bh = P(36), y = (tb - bh) / 2;
    const CallLink::State cs = v->call ? v->call->state() : CallLink::Idle;
    const bool multi = v->screens.size() > 1;
    const int n = (int)v->screens.size() + 1;

    // Everything must fit in one row. When it's tight, give way step by step instead of squashing
    // anything: Disconnect becomes an icon, then the security code drops its label, then the
    // screen picker becomes a Screens button with a menu.
    auto buttons_width = [&](bool iconDisconnect) {
        int w = P(16) + (iconDisconnect ? P(44) : P(136)) + P(12) + P(44) + P(8) + P(44) + P(8) + (v->recording ? P(132) : P(44)) +
                P(8) + P(44) + P(16) + P(44) + P(8) + P(44);
        if (cs == CallLink::Active) w += P(8) + P(44);
        return w;
    };
    auto picker_need = [&](bool menu) { return !multi ? 0 : menu ? P(44) + P(16) : P(52) * n + P(16); };
    v->shortCode = false;
    v->pickerMenu = false;
    bool iconDisconnect = false;
    auto fits = [&] { return rc.right - buttons_width(iconDisconnect) - picker_need(v->pickerMenu) >= left_content_end(v); };
    if (!fits()) iconDisconnect = true;
    if (!fits()) v->shortCode = true;
    if (!fits()) v->pickerMenu = true;

    int x = rc.right - P(16) - (iconDisconnect ? P(44) : P(136));
    place_ctl(v->disconnect, x, y, iconDisconnect ? P(44) : P(136), bh);
    if ((GetWindowTextLengthW(v->disconnect) == 0) != iconDisconnect) SetWindowTextW(v->disconnect, iconDisconnect ? L"" : L"Disconnect");
    x -= P(12) + P(44);
    place_ctl(v->full, x, y, P(44), bh);
    x -= P(8) + P(44);
    place_ctl(v->shot, x, y, P(44), bh);
    const int rw = v->recording ? P(132) : P(44);   // "Stop  01:23" while recording
    x -= P(8) + rw;
    place_ctl(v->recBtn, x, y, rw, bh);
    x -= P(8) + P(44);
    place_ctl(v->filesBtn, x, y, P(44), bh);
    // Sound, Call (icon only: red phone while ringing them, red hang-up during a call), Mute in a call.
    x -= P(16) + P(44);
    place_ctl(v->soundBtn, x, y, P(44), bh);
    x -= P(8) + P(44);
    place_ctl(v->callBtn, x, y, P(44), bh);
    if (cs == CallLink::Active) {
        x -= P(8) + P(44);
        place_ctl(v->muteBtn, x, y, P(44), bh);
    }
    // Files panel: Send files at the top, Open received files at the bottom
    const int pw = P(PANEL_W), top = (v->fullscreen ? 0 : tb);
    place_ctl(v->sendBtn, rc.right - pw + P(20), top + P(64), pw - P(40), P(42));
    place_ctl(v->openBtn, rc.right - pw + P(20), rc.bottom - P(52), pw - P(40), P(32));
    // Screens: the picker (full labels if there's room, else numbers), or the Screens menu button.
    if (v->pickerMenu) {
        place_ctl(v->screensBtn, x - P(16) - P(44), y, P(44), bh);
    } else if (multi) {
        const int avail = std::max(0, x - P(16) - left_content_end(v));
        const bool compact = avail < P(124) * n;
        const int pkw = std::min(compact ? P(64) * n : P(140) * n, avail);
        if (compact != v->compactPicker) v->compactPicker = compact;
        place_ctl(v->picker, x - P(16) - pkw, y, pkw, bh);
    }
    update_picker(v);
    ui::set_icon(v->full, v->fullscreen ? ICO_WINDOW : ICO_FULL);
}

static RECT pill_rect(HDC dc, int x, int tb, const std::wstring& t, HFONT f, UINT dpi) {
    const int w = ui::text_width(dc, t, f) + ui::px(24, dpi);
    return { x, (tb - ui::px(26, dpi)) / 2, x + w, (tb + ui::px(26, dpi)) / 2 };
}

// Message pill floating just below the toolbar (top of the screen in full screen).
static void paint_toast(HDC dc, Viewer* v, const RECT& rc) {
    v->toastRc = {};
    if (v->toast.empty()) return;
    const UINT dpi = GetDpiForWindow(v->hwnd);
    const ui::Palette Pl = ui::pal();
    const ui::Fonts& F = ui::fonts(dpi);
    const int w = ui::text_width(dc, v->toast, F.smallBold) + ui::px(56, dpi);
    const int top = (toolbar_visible(v) ? toolbar_px(v) : 0) + ui::px(12, dpi);
    const int mid = (rc.right - panel_px(v)) / 2;   // centred over the picture, not the Files panel
    RECT r{ mid - w / 2, top, mid + w / 2, top + ui::px(36, dpi) };
    ui::round_rect(dc, r, ui::px(18, dpi), Pl.card, v->toastError ? Pl.danger : Pl.accent, std::max(1, ui::px(1, dpi)));
    ui::circle(dc, r.left + ui::px(20, dpi), (r.top + r.bottom) / 2.0f, (float)ui::px(4, dpi), v->toastError ? Pl.danger : Pl.success);
    RECT t{ r.left + ui::px(34, dpi), r.top, r.right - ui::px(16, dpi), r.bottom };
    ui::text(dc, v->toast, t, F.smallBold, Pl.text);
    v->toastRc = r;
}

// While the other computer is locked (and has no service), a banner over the picture explains
// why it doesn't change or respond.
static void paint_locked(HDC dc, Viewer* v, const RECT& rc) {
    if (!v->remoteLocked) return;
    const UINT dpi = GetDpiForWindow(v->hwnd);
    const ui::Palette Pl = ui::pal();
    const ui::Fonts& F = ui::fonts(dpi);
    const std::wstring a = L"The other computer is locked", b = L"Someone there has to unlock it. To unlock it from here, "
        L"install RemoteDesk on it for all users with \u201CAllow access to the lock and sign-in screens\u201D.";
    const int mid = (rc.right - panel_px(v)) / 2;
    const int w = std::min<int>(ui::px(640, dpi), rc.right - panel_px(v) - ui::px(48, dpi));
    RECT r{ mid - w / 2, rc.bottom - ui::px(120, dpi), mid + w / 2, rc.bottom - ui::px(32, dpi) };
    ui::round_rect(dc, r, ui::px(16, dpi), Pl.card, Pl.danger, std::max(1, ui::px(1, dpi)));
    RECT ic{ r.left + ui::px(16, dpi), r.top, r.left + ui::px(48, dpi), r.bottom };
    ui::glyph(dc, ui::ico::Lock, ic, F.icon, Pl.danger);
    ui::text(dc, a, { r.left + ui::px(58, dpi), r.top + ui::px(14, dpi), r.right - ui::px(16, dpi), r.top + ui::px(38, dpi) }, F.bodyBold, Pl.text);
    ui::text(dc, b, { r.left + ui::px(58, dpi), r.top + ui::px(40, dpi), r.right - ui::px(16, dpi), r.bottom - ui::px(8, dpi) }, F.small,
             Pl.muted, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void paint_toolbar(HDC dc, Viewer* v, const RECT& rc) {
    const UINT dpi = GetDpiForWindow(v->hwnd);
    auto P = [dpi](int x) { return ui::px(x, dpi); };
    const ui::Palette Pl = ui::pal();
    const ui::Fonts& F = ui::fonts(dpi);
    const int tb = toolbar_px(v);
    RECT bar{ 0, 0, rc.right, tb };
    ui::fill(dc, bar, Pl.sidebar);
    ui::line(dc, 0, tb - 1, rc.right, tb - 1, Pl.cardBorder);
    // live dot, address, security code, view-only badge
    ui::circle(dc, (float)P(24), tb / 2.0f, (float)P(5), Pl.success);
    const int nameW = ui::text_width(dc, v->remoteId, F.title);
    ui::text(dc, v->remoteId, { P(40), 0, P(40) + nameW + P(4), tb }, F.title, Pl.text);
    int x = P(40) + nameW + P(16);
    const std::wstring code = code_label(v);
    RECT pill = pill_rect(dc, x, tb, code, F.smallBold, dpi);
    ui::round_rect(dc, pill, P(13), Pl.accentSoft);
    ui::text(dc, code, pill, F.smallBold, Pl.accentText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    x = pill.right + P(8);
    if (!can_control(v)) {
        RECT vo = pill_rect(dc, x, tb, L"View only", F.smallBold, dpi);
        ui::round_rect(dc, vo, P(13), Pl.warningSoft);
        ui::text(dc, L"View only", vo, F.smallBold, Pl.warning, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        x = vo.right + P(8);
    }
    // "PRO" next to the Pro buttons while unlicensed
    RECT sr;
    GetWindowRect(IsWindowVisible(v->muteBtn) ? v->muteBtn : v->callBtn, &sr);
    MapWindowPoints(nullptr, v->hwnd, (POINT*)&sr, 2);
    const bool pro = pro_session(v);
    int right = sr.left - P(8);
    if (IsWindowVisible(v->picker)) {
        RECT pr;
        GetWindowRect(v->picker, &pr);
        MapWindowPoints(nullptr, v->hwnd, (POINT*)&pr, 2);
        right = pr.left - P(8);
    }
    if (!pro) {
        RECT badge = pill_rect(dc, 0, tb, L"PRO", F.smallBold, dpi);
        OffsetRect(&badge, right - badge.right, 0);
        if (badge.left >= x) {   // only where it fits: never over the address or security code
            ui::round_rect(dc, badge, P(13), Pl.accent);
            ui::text(dc, L"PRO", badge, F.smallBold, Pl.onAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            right = badge.left - P(8);
        }
    }
}

// ---- full screen (F11 / Alt+Enter) ----
static void set_fullscreen(Viewer* v, bool on) {
    if (on == v->fullscreen) return;
    if (on && !pro_has(v, PRO_FULLSCREEN)) {
        toast(v, L"Full screen is a RemoteDesk Pro feature — add your key in Settings", true);
        return;
    }
    HWND h = v->hwnd;
    const LONG style = GetWindowLongW(h, GWL_STYLE);
    if (on) {
        MONITORINFO mi{ sizeof(mi) };
        GetWindowPlacement(h, &v->placement);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        v->fullscreen = true;
        v->revealed = false;
        SetWindowLongW(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        toast(v, L"Full screen — move the pointer to the top for the toolbar, F11 to leave");
    } else {
        v->fullscreen = false;
        v->revealed = false;
        SetWindowLongW(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &v->placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    show_toolbar_controls(v);
    layout_toolbar(v);
    InvalidateRect(h, nullptr, FALSE);
    SetFocus(h);
}

static void reveal(Viewer* v, bool on) {
    if (!v->fullscreen || v->revealed == on) return;
    v->revealed = on;
    show_toolbar_controls(v);
    RECT r{ 0, 0, 1 << 16, toolbar_px(v) };
    InvalidateRect(v->hwnd, &r, FALSE);   // shows the toolbar, or the picture under it again
}

// ---- screenshot ----
static std::wstring screenshot_path(const std::string& peer) {
    PWSTR pics = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) dir = pics;
    CoTaskMemFree(pics);
    if (dir.empty()) return L"";
    dir += L"\\RemoteDesk";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf(name, 96, L"\\RemoteDesk %hs %04u-%02u-%02u %02u.%02u.%02u.png", peer.c_str(), t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
    return dir + name;
}

static void take_screenshot(Viewer* v) {
    if (!pro_has(v, PRO_SCREENSHOT)) { toast(v, L"Screenshots are a RemoteDesk Pro feature — add your key in Settings", true); return; }
    if (!(v->perms & PERM_RECORD)) { toast(v, L"The other person didn't allow screenshots", true); return; }
    std::vector<uint8_t> px;
    int w, h;
    {
        std::lock_guard<std::mutex> lk(v->mx);
        if (!v->bits) return;
        w = v->rw; h = v->rh;
        px.assign(v->bits, v->bits + (size_t)w * h * 4);
    }
    const std::wstring path = screenshot_path(v->peerId);
    HWND hwnd = v->hwnd;
    std::thread([px = std::move(px), w, h, path, hwnd] {   // encoding a big PNG takes a moment
        bool ok = !path.empty() && save_png(px.data(), w, h, path);
        auto* msg = new std::wstring(ok ? path : L"");
        if (!PostMessageW(hwnd, WM_V_TOAST, ok ? 0 : 1, (LPARAM)msg)) delete msg;
    }).detach();
}

// ---- sound and calls ----
static bool peer_has_audio(Viewer* v) { return v->files && (v->files->peer_caps() & CAP_AUDIO); }

static void update_sound_button(Viewer* v) {
    ui::set_kind(v->soundBtn, v->listening ? ui::Kind::Primary : ui::Kind::Secondary);
    ui::set_icon(v->soundBtn, v->listening ? ICO_SPEAKER : ICO_SPEAKER_OFF);
}

static void toggle_sound(Viewer* v) {
    if (!v->listening) {
        if (!peer_has_audio(v)) { toast(v, L"The other computer needs RemoteDesk 1.5 or later for sound", true); return; }
        if (!(v->perms & PERM_AUDIO)) { toast(v, L"The other person hasn't allowed you to hear their computer's sound", true); return; }
    }
    v->listening = !v->listening;
    const uint8_t b = v->listening ? 1 : 0;
    v->ch.send(MSG_AUDIO_CTL, &b, 1);
    if (!v->listening && v->sound) v->sound->stop();
    update_sound_button(v);
    toast(v, v->listening ? L"Playing the other computer's sound" : L"Their sound is off");
}

static void press_call(Viewer* v) {
    if (!v->call) return;
    switch (v->call->state()) {
    case CallLink::Idle:
        if (!peer_has_audio(v)) { toast(v, L"The other computer needs RemoteDesk 1.5 or later for calls", true); return; }
        if (!ensure_microphone(v->hwnd)) return;   // Windows blocks it: explained, settings offered
        v->call->call();
        break;
    case CallLink::Ringing: break;   // answered in the call window
    default: v->call->hang_up(); break;
    }
}

static void update_call_ui(Viewer* v) {
    const CallLink::State s = v->call ? v->call->state() : CallLink::Idle;
    const bool muted = v->call && v->call->muted();
    SetWindowTextW(v->callBtn, L"");   // the icon and colour say it: phone, spinning phone (ringing), red hang-up
    ui::set_kind(v->callBtn, s == CallLink::Calling || s == CallLink::Active ? ui::Kind::Danger : ui::Kind::Secondary);
    ui::set_icon(v->callBtn, s == CallLink::Active ? ICO_HANGUP : ICO_PHONE);
    ui::set_busy(v->callBtn, s == CallLink::Calling, true);
    ui::set_kind(v->muteBtn, muted ? ui::Kind::Danger : ui::Kind::Secondary);
    // Their call: answer in a small window (closed again if they give up first).
    if (s == CallLink::Ringing && !v->callPrompt) {
        Viewer* vp = v;
        v->callPrompt = show_call_prompt(v->hwnd, v->remoteId, [vp](bool yes) {
            vp->callPrompt = nullptr;
            if (yes && !ensure_microphone(vp->hwnd)) yes = false;   // can't talk: decline rather than a silent call
            if (vp->call) vp->call->answer(yes);
        });
    } else if (s != CallLink::Ringing && v->callPrompt) {
        close_call_prompt(v->callPrompt);
        v->callPrompt = nullptr;
    }
    if (s == CallLink::Active) toast(v, muted ? L"On a call · your microphone is muted" : L"On a call");
    else if (s == CallLink::Calling) toast(v, L"Calling " + v->remoteId + L"…");
    show_toolbar_controls(v);
    layout_toolbar(v);
    invalidate_top(v);   // only the toolbar changed: don't repaint the picture
}

// ---- files (release 2) ----
// Why files can't be exchanged right now, or "" if they can.
static std::wstring files_refusal(Viewer* v) {
    if (!pro_has(v, PRO_FILES)) return L"File transfer needs RemoteDesk Pro on one of the two computers — add a key in Settings";
    if (!(v->perms & PERM_FILES)) return L"The other person didn't allow file transfer";
    return L"";
}

static void set_panel(Viewer* v, bool on) {
    if (v->panel == on) return;
    v->panel = on;
    show_toolbar_controls(v);
    layout_toolbar(v);
    InvalidateRect(v->hwnd, nullptr, FALSE);
}

static void send_paths(Viewer* v, const std::vector<std::wstring>& paths) {
    const std::wstring why = files_refusal(v);
    if (!why.empty()) { toast(v, why, true); return; }
    if (paths.empty() || !v->files) return;
    v->files->send_files(paths);
    set_panel(v, true);
}

static void choose_and_send(Viewer* v) {
    if (const std::wstring why = files_refusal(v); !why.empty()) { toast(v, why, true); return; }
    std::vector<wchar_t> buf(32768);
    OPENFILENAMEW of{ sizeof(of) };
    of.hwndOwner = v->hwnd;
    of.lpstrFile = buf.data();
    of.nMaxFile = (DWORD)buf.size();
    of.lpstrTitle = L"Send files to the other computer";
    of.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    // One file: the full path. Several: the folder, then each name, double-NUL terminated.
    std::vector<std::wstring> paths;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) paths.push_back(first);
    for (; *p; p += wcslen(p) + 1) paths.push_back(first + L"\\" + p);
    send_paths(v, paths);
}

static std::wstring human_size(uint64_t b) {
    wchar_t s[32];
    if (b < 1024) swprintf(s, 32, L"%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024) swprintf(s, 32, L"%.0f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) swprintf(s, 32, L"%.1f MB", b / 1048576.0);
    else swprintf(s, 32, L"%.2f GB", b / 1073741824.0);
    return s;
}

static void paint_panel(HDC dc, Viewer* v, const RECT& rc) {
    if (!v->panel) return;
    const UINT dpi = GetDpiForWindow(v->hwnd);
    auto P = [dpi](int x) { return ui::px(x, dpi); };
    const ui::Palette Pl = ui::pal();
    const ui::Fonts& F = ui::fonts(dpi);
    const int top = v->fullscreen ? 0 : toolbar_px(v);
    RECT pn{ rc.right - P(PANEL_W), top, rc.right, rc.bottom };
    ui::fill(dc, pn, Pl.sidebar);
    ui::line(dc, pn.left, pn.top, pn.left, pn.bottom, Pl.cardBorder);
    ui::text(dc, L"Files", { pn.left + P(20), top + P(16), pn.right - P(20), top + P(48) }, F.title, Pl.text);
    v->cancelHits.clear();
    v->pauseHits.clear();
    const bool canPause = v->files && v->files->can_pause();   // both sides 1.5.1+

    const std::wstring why = files_refusal(v);
    int y = top + P(122);
    if (!why.empty()) {
        RECT r{ pn.left + P(20), y, pn.right - P(20), y + P(60) };
        ui::text(dc, why, r, F.small, Pl.danger, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        y += P(70);
    }
    std::vector<Transfer> list = v->files ? v->files->list() : std::vector<Transfer>{};
    if (list.empty() && why.empty()) {
        RECT r{ pn.left + P(20), y + P(24), pn.right - P(20), y + P(110) };
        ui::round_rect(dc, r, P(14), CLR_INVALID, Pl.cardBorder);
        ui::glyph(dc, ICO_FILES, { r.left, r.top + P(10), r.right, r.top + P(46) }, F.iconLarge, Pl.faint);
        ui::text(dc, L"Drop files here to send them", { r.left, r.top + P(48), r.right, r.top + P(70) }, F.small, Pl.muted,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    // newest first
    for (auto it = list.rbegin(); it != list.rend() && y < rc.bottom - P(140); ++it) {
        const Transfer& t = *it;
        RECT row{ pn.left + P(14), y, pn.right - P(14), y + P(70) };
        ui::round_rect(dc, row, P(12), Pl.card, Pl.cardBorder);
        const bool live = t.state == Transfer::Queued || t.state == Transfer::Waiting || t.state == Transfer::Active ||
                          t.state == Transfer::Paused;
        // Pause while it's moving (resumable files only), Resume while paused.
        const bool pauseBtn = canPause && ((t.state == Transfer::Active && t.resumable) || (t.state == Transfer::Queued && t.outgoing) ||
                                           t.state == Transfer::Paused);
        COLORREF ic = t.state == Transfer::Failed ? Pl.danger : t.state == Transfer::Done ? Pl.success : Pl.accentText;
        RECT icr{ row.left + P(12), row.top + P(14), row.left + P(44), row.top + P(46) };
        ui::round_rect(dc, icr, P(10), t.state == Transfer::Failed ? Pl.dangerSoft : Pl.accentSoft);
        ui::glyph(dc, t.outgoing ? ICO_UP : ICO_DOWN, icr, F.icon, ic);
        const bool xBtn = live || (t.outgoing && t.resumable && t.state == Transfer::Failed);
        const int tx = icr.right + P(12), tr = row.right - (xBtn ? P(40) : P(12)) - (pauseBtn ? P(32) : 0);
        ui::text(dc, t.name, { tx, row.top + P(10), tr, row.top + P(32) }, F.bodyBold, Pl.text);
        std::wstring sub;
        switch (t.state) {
        case Transfer::Queued: sub = L"Waiting · " + human_size(t.size); break;
        case Transfer::Waiting: sub = L"Asking the other computer…"; break;
        case Transfer::Active:
            sub = std::to_wstring(t.percent()) + L"%  \u00B7  " + human_size(t.done) + L" of " + human_size(t.size) +
                  (t.resumed ? L"  \u00B7  resumed" : L"");
            break;
        case Transfer::Done: sub = (t.outgoing ? L"Sent · " : L"Received · ") + human_size(t.size); break;
        case Transfer::Failed: sub = t.error.empty() ? L"Failed" : t.error; break;
        case Transfer::Cancelled: sub = L"Cancelled"; break;
        case Transfer::Paused: sub = L"Paused at " + std::to_wstring(t.percent()) + L"%  ·  " + human_size(t.done) + L" of " + human_size(t.size); break;
        }
        if (t.state == Transfer::Waiting && t.done) sub = L"Resuming…";
        ui::text(dc, sub, { tx, row.top + P(32), tr, row.top + P(50) }, F.small,
                 t.state == Transfer::Failed ? Pl.danger : Pl.muted);
        if (t.state == Transfer::Active || t.state == Transfer::Waiting || t.state == Transfer::Paused) {   // progress bar
            RECT bar{ tx, row.bottom - P(14), row.right - P(12), row.bottom - P(10) };
            ui::round_rect(dc, bar, P(2), Pl.divider);
            if (t.size) {
                RECT done = bar;
                done.right = bar.left + (LONG)((bar.right - bar.left) * (double)t.done / (double)t.size);
                if (done.right > done.left) ui::round_rect(dc, done, P(2), t.state == Transfer::Paused ? Pl.faint : Pl.accent);
            }
        }
        // x cancels a running transfer, or discards an interrupted one that would otherwise resume
        if (live || (t.outgoing && t.resumable && t.state == Transfer::Failed)) {
            RECT x{ row.right - P(36), row.top + P(14), row.right - P(10), row.top + P(40) };
            ui::glyph(dc, ui::ico::Close, x, F.iconSmall, Pl.faint);
            v->cancelHits.push_back({ x, { t.id, t.outgoing } });
        }
        if (pauseBtn) {
            const bool paused = t.state == Transfer::Paused;
            RECT pb{ row.right - (xBtn ? P(68) : P(36)), row.top + P(14), row.right - (xBtn ? P(42) : P(10)), row.top + P(40) };
            ui::glyph(dc, paused ? ICO_PLAY : ICO_PAUSE, pb, F.iconSmall, paused ? Pl.accentText : Pl.muted);
            v->pauseHits.push_back({ pb, t.id, t.outgoing, paused });
        }
        y += P(78);
    }
}

// ---- recording (release 2) ----
static std::wstring video_path(const std::string& peer) {
    PWSTR vids = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vids))) dir = vids;
    CoTaskMemFree(vids);
    if (dir.empty()) return L"";
    dir += L"\\RemoteDesk";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf(name, 96, L"\\RemoteDesk %hs %04u-%02u-%02u %02u.%02u.%02u.mp4", peer.c_str(), t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
    return dir + name;
}

static void rec_thread(Viewer* v, std::wstring path, int w, int h) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring err;
    {
        Recorder rec;
        if (!rec.start(path, w, h, REC_FPS, err)) {
            auto* m = new std::wstring(err);
            if (!PostMessageW(v->hwnd, WM_V_RECDONE, 1, (LPARAM)m)) delete m;
            v->recording = false;
            CoUninitialize();
            return;
        }
        std::vector<uint8_t> px;
        const auto t0 = std::chrono::steady_clock::now();
        for (int64_t frame = 0; v->recording; ++frame) {
            std::this_thread::sleep_until(t0 + std::chrono::microseconds(frame * 1000000 / REC_FPS));
            {
                std::lock_guard<std::mutex> lk(v->mx);
                if (!v->bits || v->rw != w || v->rh != h) { err = L"Recording stopped: the screen changed size"; break; }
                px.assign(v->bits, v->bits + (size_t)w * h * 4);
            }
            if (!rec.add_frame(px.data(), w, h, frame * 10000000 / REC_FPS)) { err = L"Recording stopped: the encoder failed"; break; }
        }
        rec.stop();   // finishes the MP4
    }
    CoUninitialize();
    auto* m = new std::wstring(err.empty() ? path : err);
    if (!PostMessageW(v->hwnd, WM_V_RECDONE, err.empty() ? 0 : 1, (LPARAM)m)) delete m;
}

static void update_rec_button(Viewer* v) {
    if (v->recording) {
        const ULONGLONG s = (GetTickCount64() - v->recStart) / 1000;
        wchar_t t[32];
        swprintf(t, 32, L"Stop  %02llu:%02llu", s / 60, s % 60);
        SetWindowTextW(v->recBtn, t);
        ui::set_kind(v->recBtn, ui::Kind::Danger);
        ui::set_icon(v->recBtn, ICO_STOP);
    } else {
        SetWindowTextW(v->recBtn, L"");
        ui::set_kind(v->recBtn, ui::Kind::Secondary);
        ui::set_icon(v->recBtn, ICO_REC);
    }
}

static void notify_host_recording(Viewer* v, bool on) {
    uint8_t b = on ? 1 : 0;
    v->ch.send(MSG_RECORDING, &b, 1);   // the host shows "Recording your screen"
}

static void stop_recording(Viewer* v) {
    if (!v->recording && !v->recThread.joinable()) return;
    v->recording = false;
    if (v->recThread.joinable()) v->recThread.join();
    KillTimer(v->hwnd, TIMER_REC);
    notify_host_recording(v, false);
    update_rec_button(v);
    layout_toolbar(v);
}

static void start_recording(Viewer* v) {
    if (!pro_has(v, PRO_RECORDING)) { toast(v, L"Recording is a RemoteDesk Pro feature — add your key in Settings", true); return; }
    if (!(v->perms & PERM_RECORD)) { toast(v, L"The other person didn't allow recording", true); return; }
    int w, h;
    {
        std::lock_guard<std::mutex> lk(v->mx);
        if (!v->bits) return;
        w = v->rw; h = v->rh;
    }
    const std::wstring path = video_path(v->peerId);
    if (path.empty()) { toast(v, L"No Videos folder to record into", true); return; }
    if (v->recThread.joinable()) v->recThread.join();
    v->recording = true;
    v->recStart = GetTickCount64();
    v->recThread = std::thread(rec_thread, v, path, w, h);
    notify_host_recording(v, true);
    SetTimer(v->hwnd, TIMER_REC, 1000, nullptr);
    update_rec_button(v);
    layout_toolbar(v);
    toast(v, L"Recording — the other person can see that you are recording");
}

// ---- window ----
// Fits the picture into `area`, keeping its aspect ratio.
static void layout(Viewer* v, const RECT& area) {
    int cw = area.right - area.left, ch = area.bottom - area.top;
    if (v->rw <= 0 || v->rh <= 0 || cw <= 0 || ch <= 0) { v->dst = area; return; }
    double s = std::min((double)cw / v->rw, (double)ch / v->rh);
    int w = (int)(v->rw * s + 0.5), h = (int)(v->rh * s + 0.5);
    v->dst.left = area.left + (cw - w) / 2;
    v->dst.top = area.top + (ch - h) / 2;
    v->dst.right = v->dst.left + w;
    v->dst.bottom = v->dst.top + h;
}

static void fit_window(Viewer* v) {
    if (v->fullscreen) return;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(v->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const int tb = toolbar_px(v);
    int aw = (mi.rcWork.right - mi.rcWork.left) * 9 / 10;
    int ah = (mi.rcWork.bottom - mi.rcWork.top) * 9 / 10 - tb;
    double s = std::min(1.0, std::min((double)aw / v->rw, (double)ah / v->rh));
    RECT r{ 0, 0, std::max((int)(v->rw * s), ui::px(860, GetDpiForWindow(v->hwnd))), (int)(v->rh * s) + tb };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(v->hwnd));
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(v->hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, SWP_NOZORDER);
}

static LRESULT CALLBACK viewer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* v = (Viewer*)((CREATESTRUCTW*)lp)->lpCreateParams;
        v->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)v);
    }
    auto* v = (Viewer*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!v) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE:
        AddClipboardFormatListener(hwnd);
        v->picker = ui::make_control(hwnd, IDV_SCREENS, ui::Kind::Segmented, L"", 0, ui::Surface::Sidebar);
        v->shot = ui::make_control(hwnd, IDV_SHOT, ui::Kind::Secondary, L"", ICO_CAMERA, ui::Surface::Sidebar);
        v->full = ui::make_control(hwnd, IDV_FULL, ui::Kind::Secondary, L"", ICO_FULL, ui::Surface::Sidebar);
        v->disconnect = ui::make_control(hwnd, IDV_DISCONNECT, ui::Kind::Danger, L"Disconnect", ui::ico::Close,
                                         ui::Surface::Sidebar);
        v->recBtn = ui::make_control(hwnd, IDV_REC, ui::Kind::Secondary, L"", ICO_REC, ui::Surface::Sidebar);
        v->filesBtn = ui::make_control(hwnd, IDV_FILES, ui::Kind::Secondary, L"", ICO_FILES, ui::Surface::Sidebar);
        v->soundBtn = ui::make_control(hwnd, IDV_SOUND, ui::Kind::Secondary, L"", ICO_SPEAKER_OFF, ui::Surface::Sidebar);
        v->callBtn = ui::make_control(hwnd, IDV_CALL, ui::Kind::Secondary, L"", ICO_PHONE, ui::Surface::Sidebar);
        v->muteBtn = ui::make_control(hwnd, IDV_MUTE, ui::Kind::Secondary, L"", ICO_MIC, ui::Surface::Sidebar);
        v->screensBtn = ui::make_control(hwnd, IDV_SCREENSMENU, ui::Kind::Secondary, L"", ui::ico::Monitor, ui::Surface::Sidebar);
        v->sendBtn = ui::make_control(hwnd, IDV_SEND, ui::Kind::Primary, L"Send files\u2026", ICO_UP, ui::Surface::Sidebar);
        v->openBtn = ui::make_control(hwnd, IDV_OPEN, ui::Kind::Link, L"Open received files", ICO_FILES, ui::Surface::Sidebar);
        DragAcceptFiles(hwnd, TRUE);   // drop files on the window to send them
        // If RemoteDesk runs as administrator (e.g. started by setup), Windows blocks drops from
        // Explorer (a normal-rights program) unless these messages are let through.
        ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);
        show_toolbar_controls(v);
        layout_toolbar(v);
        if (!can_control(v)) toast(v, L"View only — you can watch, but not control this computer");
        return 0;

    case WM_V_RESIZE: {
        std::lock_guard<std::mutex> lk(v->mx);
        if (v->dib) DeleteObject(v->dib);
        v->dib = nullptr; v->bits = nullptr;
        v->rw = (int)wp; v->rh = (int)lp;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = v->rw;
        bi.bmiHeader.biHeight = -v->rh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        v->dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&v->bits, nullptr, 0);
        if (v->bits) memset(v->bits, 0, (size_t)v->rw * v->rh * 4);
        if (!v->sized) { v->sized = true; if (!IsZoomed(hwnd)) fit_window(v); }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_V_SCREENS: {
        const auto& p = *(const std::vector<uint8_t>*)lp;
        int32_t sel;
        memcpy(&sel, p.data(), sizeof(sel));
        if (sel != v->screen) v->sized = false;   // refit the window to the new screen's shape
        v->screen = sel;
        v->screens.resize((p.size() - sizeof(sel)) / sizeof(MonitorDesc));
        memcpy(v->screens.data(), p.data() + sizeof(sel), v->screens.size() * sizeof(MonitorDesc));
        update_picker(v);
        layout_toolbar(v);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDV_DISCONNECT) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (id == IDV_FULL) { set_fullscreen(v, !v->fullscreen); SetFocus(hwnd); return 0; }
        if (id == IDV_SHOT) { take_screenshot(v); SetFocus(hwnd); return 0; }
        if (id == IDV_REC) { if (v->recording) stop_recording(v); else start_recording(v); SetFocus(hwnd); return 0; }
        if (id == IDV_FILES) { set_panel(v, !v->panel); SetFocus(hwnd); return 0; }
        if (id == IDV_SOUND) { toggle_sound(v); SetFocus(hwnd); return 0; }
        if (id == IDV_SCREENSMENU) { screens_menu(v); SetFocus(hwnd); return 0; }
        if (id == IDV_CALL) { press_call(v); SetFocus(hwnd); return 0; }
        if (id == IDV_MUTE) { if (v->call) v->call->set_muted(!v->call->muted()); SetFocus(hwnd); return 0; }
        if (id == IDV_SEND) { choose_and_send(v); SetFocus(hwnd); return 0; }
        if (id == IDV_OPEN) {
            ShellExecuteW(nullptr, L"open", FileEngine::downloads_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            SetFocus(hwnd);
            return 0;
        }
        if (id == IDV_SCREENS) {
            const int i = ui::selected(v->picker);
            int32_t want = i >= (int)v->screens.size() ? -1 : i;   // last option = all screens
            if (want != v->screen) v->ch.send(MSG_SELECT_SCREEN, &want, sizeof(want));   // host answers with MSG_MONITORS
            SetFocus(hwnd);   // keys go back to the remote computer
            return 0;
        }
        break;
    }

    case WM_V_CALL:
        update_call_ui(v);
        return 0;
    case WM_V_NOTE: {
        auto* t = (std::wstring*)lp;
        toast(v, *t);
        delete t;
        return 0;
    }
    case WM_V_LOCKED:
        v->remoteLocked = wp != 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_V_PRO:   // the host's license unlocked Pro for this session
        layout_toolbar(v);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_V_PERMS: {   // the other person granted or revoked something
        const uint8_t before = v->perms, now = (uint8_t)wp;
        if (before == now) return 0;
        // Let go of anything held down while we still may (release_all sends key-ups / button-ups).
        if ((before & PERM_CONTROL) && !(now & PERM_CONTROL)) { release_all(v); ReleaseCapture(); }
        v->perms = now;
        std::wstring what;
        auto change = [&](uint8_t bit, const wchar_t* on, const wchar_t* off) {
            if ((before & bit) == (now & bit)) return;
            if (!what.empty()) what += L"  \u00B7  ";
            what += (now & bit) ? on : off;
        };
        change(PERM_CONTROL, L"You can now control this computer", L"Control was turned off \u2014 you can only watch");
        change(PERM_CLIPBOARD, L"Clipboard sharing is on", L"Clipboard sharing is off");
        change(PERM_RECORD, L"Screenshots and recording allowed", L"Screenshots and recording are no longer allowed");
        change(PERM_FILES, L"File transfer allowed", L"File transfer is no longer allowed");
        change(PERM_AUDIO, L"You may hear their computer's sound", L"Their computer's sound is no longer shared");
        if (!(now & PERM_RECORD) && v->recording) stop_recording(v);   // the MP4 so far is kept
        if (!(now & PERM_AUDIO) && v->listening) { v->listening = false; if (v->sound) v->sound->stop(); update_sound_button(v); }
        toast(v, L"The other person changed your access: " + what, !(now & ~before));
        layout_toolbar(v);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_V_FILES: {   // transfer progress: repaint the panel only
        if (v->panel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            rc.left = rc.right - panel_px(v);
            InvalidateRect(hwnd, &rc, FALSE);
        }
        return 0;
    }
    case WM_V_FILEDONE: {
        auto* t = (Transfer*)lp;
        if (t->state == Transfer::Done && !t->outgoing) {
            v->lastShot = t->path;
            toast(v, L"Received " + t->name + L" \u00B7 click to show");
        } else if (t->state == Transfer::Done) {
            toast(v, L"Sent " + t->name);
        } else if (t->state == Transfer::Failed) {
            toast(v, t->name + L": " + (t->error.empty() ? std::wstring(L"failed") : t->error), true);
        }
        delete t;
        return 0;
    }
    case WM_V_RECDONE: {
        auto* m = (std::wstring*)lp;
        stop_recording(v);   // the thread has ended; tidy up the button and tell the host
        if (wp) toast(v, *m, true);
        else { v->lastShot = *m; toast(v, L"Recording saved to Videos \u203A RemoteDesk \u00B7 click to show"); }
        delete m;
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        std::vector<std::wstring> paths;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t p[MAX_PATH * 4];
            if (DragQueryFileW(drop, i, p, (UINT)std::size(p))) paths.push_back(p);
        }
        DragFinish(drop);
        send_paths(v, paths);
        return 0;
    }

    case WM_V_TOAST: {
        auto* path = (std::wstring*)lp;
        if (wp) toast(v, L"Couldn't save the screenshot", true);
        else { v->lastShot = *path; toast(v, L"Screenshot saved to Pictures › RemoteDesk  ·  click to show"); }
        delete path;
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REC) { update_rec_button(v); return 0; }
        if (wp == TIMER_PING) {
            v->ch.send(MSG_PING, nullptr, 0);
            if (v->pinged && GetTickCount64() - v->lastRx > PING_TIMEOUT_MS)
                shutdown(v->ch.socket(), SD_BOTH);   // the host vanished: the rx thread ends and reports it
            return 0;
        }
        if (wp == TIMER_TOAST) {
            KillTimer(hwnd, TIMER_TOAST);
            v->toast.clear();
            invalidate_top(v);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:   // the scale changed: the whole picture must be redrawn
        layout_toolbar(v);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {   // ring pointer over the picture, arrow over the margins and toolbar
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            const bool onPicture = v->dib && PtInRect(&v->dst, pt) && pt.y >= toolbar_hit(v);
            const bool onToast = !IsRectEmpty(&v->toastRc) && PtInRect(&v->toastRc, pt) && !v->lastShot.empty();
            SetCursor(onToast ? LoadCursorW(nullptr, IDC_HAND)
                      : onPicture && can_control(v) ? ui::viewer_cursor(GetDpiForWindow(hwnd))
                                                    : LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_V_THEME:
    case WM_SETTINGCHANGE:
        ui::style_window(hwnd);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        SetDCBrushColor(dc, ui::pal().viewerBg);
        HBRUSH bg = (HBRUSH)GetStockObject(DC_BRUSH);
        RECT area = rc;
        area.top = std::min(rc.bottom, (LONG)reserved_top(v));
        area.right = std::max(area.left, area.right - panel_px(v));   // the Files panel takes the right side
        {
            std::lock_guard<std::mutex> lk(v->mx);
            layout(v, area);
            const RECT d = v->dst;
            RECT bars[4] = { { 0, area.top, rc.right, d.top }, { 0, d.bottom, rc.right, rc.bottom },
                             { 0, d.top, d.left, d.bottom }, { d.right, d.top, rc.right, d.bottom } };
            if (v->dib) {
                for (RECT& b : bars) if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, bg);
                // Only the invalidated part of the picture is scaled and copied.
                RECT p;
                const int dw = d.right - d.left, dh = d.bottom - d.top;
                if (dw > 0 && dh > 0 && IntersectRect(&p, &ps.rcPaint, &d)) {
                    auto src = [](int64_t v0, int64_t num, int64_t den, bool up) {
                        return (int)(up ? (v0 * num + den - 1) / den : v0 * num / den);
                    };
                    int sx0 = std::clamp(src(p.left - d.left, v->rw, dw, false), 0, v->rw);
                    int sx1 = std::clamp(src(p.right - d.left, v->rw, dw, true), 0, v->rw);
                    int sy0 = std::clamp(src(p.top - d.top, v->rh, dh, false), 0, v->rh);
                    int sy1 = std::clamp(src(p.bottom - d.top, v->rh, dh, true), 0, v->rh);
                    int dx0 = d.left + (int)((int64_t)sx0 * dw / v->rw), dx1 = d.left + (int)((int64_t)sx1 * dw / v->rw);
                    int dy0 = d.top + (int)((int64_t)sy0 * dh / v->rh), dy1 = d.top + (int)((int64_t)sy1 * dh / v->rh);
                    HDC mem = CreateCompatibleDC(dc);
                    HGDIOBJ old = SelectObject(mem, v->dib);
                    SetStretchBltMode(dc, (dw == v->rw && dh == v->rh) ? COLORONCOLOR : HALFTONE);
                    SetBrushOrgEx(dc, 0, 0, nullptr);
                    StretchBlt(dc, dx0, dy0, dx1 - dx0, dy1 - dy0, mem, sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                }
            } else {
                FillRect(dc, &area, bg);
                const ui::Fonts& F = ui::fonts(GetDpiForWindow(hwnd));
                RECT t = area;
                t.bottom = (area.top + area.bottom) / 2 + ui::px(4, GetDpiForWindow(hwnd));
                ui::text(dc, L"Connected to " + v->remoteId, t, F.heading, RGB(0xE6, 0xE9, 0xEF),
                         DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
                t.top = t.bottom + ui::px(6, GetDpiForWindow(hwnd));
                t.bottom = rc.bottom;
                ui::text(dc, L"Waiting for the first picture…", t, F.body, RGB(0xA0, 0xA8, 0xB7),
                         DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        paint_panel(dc, v, rc);
        if (toolbar_visible(v)) paint_toolbar(dc, v, rc);   // on top: in full screen it floats over the picture
        paint_toast(dc, v, rc);
        paint_locked(dc, v, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (v->fullscreen) {   // slide the toolbar in at the top edge, away once the pointer leaves it
            const int y = GET_Y_LPARAM(lp);
            if (!v->revealed && y <= ui::px(3, GetDpiForWindow(hwnd)) && !v->buttons) reveal(v, true);
            else if (v->revealed && y > toolbar_px(v) + ui::px(24, GetDpiForWindow(hwnd))) reveal(v, false);
        }
        MouseMove m;
        if (to_remote(v, lp, m)) v->ch.send(MSG_MOUSE_MOVE, &m, sizeof(m));
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        uint8_t b = (msg <= WM_LBUTTONUP) ? 0 : (msg <= WM_RBUTTONUP) ? 1 : 2;
        bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (msg == WM_LBUTTONDOWN && !v->buttons && !v->lastShot.empty() && PtInRect(&v->toastRc, pt)) {
                ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + v->lastShot + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                return 0;   // the "screenshot saved" message opens its folder
            }
        }
        if (down && !v->buttons && v->panel) {   // clicks in the Files panel are ours (x = cancel a transfer)
            RECT rc;
            GetClientRect(hwnd, &rc);
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x >= rc.right - panel_px(v)) {
                for (auto& [r, h] : v->cancelHits)
                    if (msg == WM_LBUTTONDOWN && PtInRect(&r, pt) && v->files) v->files->cancel(h.first, h.second);
                for (const auto& ph : v->pauseHits)
                    if (msg == WM_LBUTTONDOWN && PtInRect(&ph.r, pt) && v->files) {
                        if (ph.resume) v->files->resume(ph.id, ph.outgoing);
                        else v->files->pause(ph.id, ph.outgoing);
                    }
                return 0;
            }
        }
        if (down && !v->buttons && GET_Y_LPARAM(lp) < toolbar_hit(v)) return 0;   // a click on the toolbar is ours
        if (!down && !(v->buttons & (1u << b))) return 0;   // release of a click we kept
        if (!can_control(v)) return 0;
        MouseMove m;
        if (to_remote(v, lp, m)) v->ch.send(MSG_MOUSE_MOVE, &m, sizeof(m));
        MouseBtn mb{ b, (uint8_t)down };
        v->ch.send(MSG_MOUSE_BTN, &mb, sizeof(mb));
        if (down) { v->buttons |= 1u << b; SetCapture(hwnd); }
        else { v->buttons &= ~(1u << b); if (!v->buttons) ReleaseCapture(); }
        return 0;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        if (!can_control(v)) return 0;
        MouseWheel w{ GET_WHEEL_DELTA_WPARAM(wp), (uint8_t)(msg == WM_MOUSEHWHEEL) };
        v->ch.send(MSG_MOUSE_WHEEL, &w, sizeof(w));
        return 0;
    }

    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP: case WM_SYSKEYUP: {
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        const bool alt = (lp >> 29) & 1;
        // F11 and Alt+Enter toggle full screen here instead of going to the remote computer.
        if (wp == VK_F11 || (wp == VK_RETURN && alt)) {
            if (down && !(lp & (1 << 30))) set_fullscreen(v, !v->fullscreen);   // ignore auto-repeat
            return 0;
        }
        send_key(v, (uint16_t)wp, (uint16_t)((lp >> 16) & 0xFF), down, ((lp >> 24) & 1) != 0);
        return 0;   // swallow, so Alt / F10 / Alt+F4 go to the remote side
    }
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        return 0;

    case WM_KILLFOCUS:
        release_all(v);
        return 0;

    case WM_CLIPBOARDUPDATE: {
        if (!(v->perms & PERM_CLIPBOARD)) return 0;
        if (GetClipboardSequenceNumber() == g_clipIgnoreSeq) return 0;
        std::wstring text;
        if (!clipboard_get(hwnd, text) || text.size() > MAX_CLIPBOARD_CHARS) return 0;
        {
            std::lock_guard<std::mutex> lk(v->clipMx);
            if (text == v->lastClip) return 0;
            v->lastClip = text;
        }
        v->ch.send(MSG_CLIPBOARD, text.data(), (uint32_t)(text.size() * sizeof(wchar_t)));
        return 0;
    }
    case WM_V_SETCLIP: {
        auto* text = (std::wstring*)lp;
        clipboard_set(hwnd, *text);
        delete text;
        return 0;
    }

    case WM_V_LOST:   // remote side ended: no modal box, the main window says so
        if (!v->closing) {
            v->closing = true;
            post_info(INFO_HINT, L"Session with " + v->remoteId + L" ended.");
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        if (!v->closing) post_info(INFO_HINT, L"Disconnected from " + v->remoteId + L".");
        v->closing = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        stop_recording(v);   // finishes the MP4 before the window goes
        RemoveClipboardFormatListener(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void viewer_rx(Viewer* v) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Jpeg jpeg;
        uint8_t type;
        std::vector<uint8_t> p, px;
        while (v->ch.recv(type, p)) {
            v->lastRx = GetTickCount64();
            if (type == MSG_SCREEN_INFO && p.size() == sizeof(ScreenInfo)) {
                ScreenInfo si; memcpy(&si, p.data(), sizeof(si));
                if (si.w <= 0 || si.h <= 0 || si.w > 32768 || si.h > 32768) break;
                DWORD_PTR r;
                SendMessageTimeoutW(v->hwnd, WM_V_RESIZE, (WPARAM)si.w, (LPARAM)si.h,
                                    SMTO_NORMAL, 5000, &r);
            } else if (type == MSG_MONITORS && p.size() >= sizeof(int32_t) &&
                       (p.size() - sizeof(int32_t)) % sizeof(MonitorDesc) == 0 &&
                       (p.size() - sizeof(int32_t)) / sizeof(MonitorDesc) <= MAX_MONITORS) {
                DWORD_PTR r;   // sent, not posted, so it is handled before the MSG_SCREEN_INFO that follows
                SendMessageTimeoutW(v->hwnd, WM_V_SCREENS, 0, (LPARAM)&p, SMTO_NORMAL, 5000, &r);
            } else if (type == MSG_RECT && p.size() > sizeof(RectHeader)) {
                RectHeader rh; memcpy(&rh, p.data(), sizeof(rh));
                int w, h;
                if (!jpeg.decode(p.data() + sizeof(rh), p.size() - sizeof(rh), w, h, px)) continue;
                std::lock_guard<std::mutex> lk(v->mx);
                if (!v->bits || rh.x < 0 || rh.y < 0 || w != rh.w || h != rh.h ||
                    rh.x + w > v->rw || rh.y + h > v->rh) continue;
                for (int r = 0; r < h; ++r)
                    memcpy(v->bits + ((size_t)(rh.y + r) * v->rw + rh.x) * 4, px.data() + (size_t)r * w * 4,
                           (size_t)w * 4);
                RECT changed{ rh.x, rh.y, rh.x + w, rh.y + h };
                UnionRect(&v->dirty, &v->dirty, &changed);
            } else if (type == MSG_FRAME_END) {
                // Repaint only the changed part, mapped to window coordinates (plus a pixel of
                // slack for the scaling filter), instead of the whole window every frame.
                RECT inv{};
                bool all = false;
                {
                    std::lock_guard<std::mutex> lk(v->mx);
                    const RECT d = v->dst, c = v->dirty;
                    const int dw = d.right - d.left, dh = d.bottom - d.top;
                    if (dw <= 0 || dh <= 0 || v->rw <= 0 || v->rh <= 0) all = true;
                    else if (!IsRectEmpty(&c)) {
                        inv = { d.left + (int)((int64_t)c.left * dw / v->rw) - 2, d.top + (int)((int64_t)c.top * dh / v->rh) - 2,
                                d.left + (int)(((int64_t)c.right * dw + v->rw - 1) / v->rw) + 2,
                                d.top + (int)(((int64_t)c.bottom * dh + v->rh - 1) / v->rh) + 2 };
                    }
                    v->dirty = {};
                }
                if (all) InvalidateRect(v->hwnd, nullptr, FALSE);
                else if (!IsRectEmpty(&inv)) InvalidateRect(v->hwnd, &inv, FALSE);
                v->ch.send(MSG_FRAME_ACK, nullptr, 0);
            } else if (type == MSG_HOST_STATE && p.size() == 1) {
                PostMessageW(v->hwnd, WM_V_LOCKED, p[0] & 1, 0);
            } else if (type == MSG_PING) {
                v->pinged = true;
            } else if (type == MSG_PRO) {
                v->peerPro = license_peer_features(std::string(p.begin(), p.end()));
                PostMessageW(v->hwnd, WM_V_PRO, 0, 0);
            } else if (type == MSG_AUDIO) {
                if (v->sound && v->listening) v->sound->handle(p);
            } else if (v->call && v->call->handle(type, p)) {
                // call signalling / voice
            } else if (type == MSG_PERMS && p.size() == 1) {
                PostMessageW(v->hwnd, WM_V_PERMS, p[0], 0);   // applied on the window's thread
            } else if (v->files && v->files->handle(type, p)) {
                // file transfer message: handled by the engine
            } else if (type == MSG_CLIPBOARD) {
                std::wstring text((const wchar_t*)p.data(), p.size() / sizeof(wchar_t));
                {
                    std::lock_guard<std::mutex> lk(v->clipMx);
                    if (text == v->lastClip) continue;
                    v->lastClip = text;
                }
                PostMessageW(v->hwnd, WM_V_SETCLIP, 0, (LPARAM)new std::wstring(std::move(text)));
            }
        }
    }
    CoUninitialize();
    PostMessageW(v->hwnd, WM_V_LOST, 0, 0);
}

static BOOL CALLBACK restyle_one(HWND h, LPARAM) {
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64) && !lstrcmpW(cls, VIEWER_CLASS)) PostMessageW(h, WM_V_THEME, 0, 0);
    return TRUE;
}
void viewers_restyle() {   // viewer windows live on their own threads: post, don't touch
    EnumWindows(restyle_one, 0);
}

// Errors show in red under the Connect button instead of a modal box.
static void fail(const std::wstring& text) {
    post_info(INFO_ERROR, text);
}

// ---- cancelling a connection that's still being set up (the Connect button turns into Cancel) ----
static std::atomic<int> g_cancelGen{ 0 };     // bumps on Cancel: every attempt started before stops
static std::atomic<int> g_connecting{ 0 };    // attempts still dialing / waking / waiting for approval
static std::mutex g_dialMx;
static std::vector<SOCKET> g_dialing;         // their sockets, shut down on Cancel to end blocking reads

bool viewer_connecting() { return g_connecting > 0; }

void viewer_cancel_connect() {
    ++g_cancelGen;
    std::lock_guard<std::mutex> lk(g_dialMx);
    for (SOCKET s : g_dialing) shutdown(s, SD_BOTH);
}

namespace {
struct Attempt {   // one connection attempt, until it has a viewer window (or gave up)
    int gen = g_cancelGen;
    bool done = false;
    Attempt() { ++g_connecting; }
    ~Attempt() { finish(); }
    void finish() { if (!done) { done = true; --g_connecting; } }
    bool cancelled() const { return g_cancelGen != gen; }
    void watch(SOCKET s) { std::lock_guard<std::mutex> lk(g_dialMx); g_dialing.push_back(s); }
    void unwatch(SOCKET s) {
        std::lock_guard<std::mutex> lk(g_dialMx);
        g_dialing.erase(std::remove(g_dialing.begin(), g_dialing.end(), s), g_dialing.end());
    }
    bool wait(DWORD ms) {   // sleeps in small steps; false if cancelled meanwhile
        for (DWORD t = 0; t < ms && !cancelled(); t += 250) Sleep(250);
        return !cancelled();
    }
};
}

void viewer_session(std::string target, std::string password, bool savedPassword) {
    const std::wstring fid = widen(format_id(target));
    Attempt attempt;
    post_info(INFO_BUSY, L"Connecting to " + fid + L"\u2026");
    auto stopped = [&] { post_info(INFO_HINT, L"Stopped connecting to " + fid + L"."); };

    std::string acc, line;
    auto dial = [&](SOCKET& s) {   // "READY", "OFFLINE", "TIMEOUT", ... or "" when the relay is unreachable
        acc.clear();
        line.clear();
        s = relay_open("CONN " + target);
        if (s == INVALID_SOCKET) return;
        attempt.watch(s);
        if (read_line(s, acc, line, 75000) != LineResult::Ok) line = "?";
        if (line != "READY") { attempt.unwatch(s); closesocket(s); s = INVALID_SOCKET; }
    };
    SOCKET s;
    dial(s);
    if (attempt.cancelled()) { if (s != INVALID_SOCKET) { attempt.unwatch(s); closesocket(s); } stopped(); return; }
    if (line == "OFFLINE") {
        // Switched off or asleep: try to wake it through a RemoteDesk computer on its network,
        // then keep trying for a few minutes while it starts.
        const int helpers = wake_request(target);
        if (helpers == 0 || helpers == WAKE_UNKNOWN || helpers == WAKE_FAILED) {
            fail(fid + L" is offline." +
                 (helpers == 0 ? L" It can't be woken up now: no other RemoteDesk computer on its network is on."
                  : helpers == WAKE_UNKNOWN ? L" It can't be woken up remotely yet (it needs RemoteDesk 1.4.1 or later)." : L""));
            return;
        }
        if (helpers == WAKE_INTERNET)
            post_info(INFO_BUSY, fid + L" is offline and no RemoteDesk computer is on in its network, so the wake-up went over the "
                                       L"internet (works if its router forwards Wake-on-LAN, UDP port 9). Waiting\u2026");
        if (helpers > 0)
            post_info(INFO_BUSY, L"" + fid + L" is offline \u2014 waking it up through " + std::to_wstring(helpers) +
                                     (helpers == 1 ? L" computer" : L" computers") + L" on its network. This can take a minute or two\u2026");
        const ULONGLONG until = GetTickCount64() + 180000;
        do {
            if (helpers != WAKE_ONLINE && !attempt.wait(10000)) { stopped(); return; }
            dial(s);
            if (attempt.cancelled()) { if (s != INVALID_SOCKET) { attempt.unwatch(s); closesocket(s); } stopped(); return; }
        } while (line == "OFFLINE" && GetTickCount64() < until);
        if (line == "OFFLINE") {
            fail(fid + L" didn't wake up. Most laptops can only be woken from Sleep (not Shut down), plugged in. "
                       L"On that PC: Settings › Set up Wake-on-LAN, and see the Wake-on-LAN guide.");
            return;
        }
    }
    if (line != "READY") {
        if (line.empty()) fail(L"Couldn't reach the RemoteDesk server.");
        else if (line == "TIMEOUT") fail(fid + L" didn't answer in time.");
        else fail(L"Connection to " + fid + L" failed.");
        return;
    }

    {
        Viewer v(s);
        v.remoteId = fid;
        if (!v.ch.handshake(true)) {
            attempt.unwatch(s);
            closesocket(s);
            if (attempt.cancelled()) stopped(); else fail(L"Secure handshake failed.");
            return;
        }
        std::string hello = my_id() + "\n" + password;
        v.ch.send(MSG_HELLO, hello.data(), (uint32_t)hello.size());
        post_info(INFO_BUSY, password.empty() ? L"Waiting for " + fid + L" to accept\u2026  Security code " + widen(v.ch.code())
                                              : L"Signing in to " + fid + L"\u2026");

        set_recv_timeout(s, 180000);   // the remote user may take a while to click Accept
        uint8_t type = 0;
        std::vector<uint8_t> p;
        const bool answered = v.ch.recv(type, p);
        attempt.unwatch(s);
        if (attempt.cancelled()) { closesocket(s); stopped(); return; }
        if (!answered || type != MSG_ACCEPT) {
            closesocket(s);
            std::string why(p.begin(), p.end());
            if (type == MSG_REJECT && savedPassword && why == "Wrong password.") {
                history_forget_password(target);   // changed on the other side: ask again next time
                fail(L"The saved password for " + fid + L" no longer works. It has been forgotten.");
            } else {
                fail(type == MSG_REJECT ? widen(why) : fid + L" didn't respond.");
            }
            return;
        }
        set_recv_timeout(s, 0);
        attempt.finish();   // connected: Cancel no longer applies (Disconnect does)
        v.peerId = target;
        v.perms = p.empty() ? (uint8_t)PERM_LEGACY : p[0];   // hosts before 1.3 send no permissions
        if (const std::string t = license_token(); !t.empty()) v.ch.send(MSG_PRO, t.data(), t.size());
        v.ch.send(MSG_PING, nullptr, 0);   // at once: from here on the host notices if we vanish
        post_info(INFO_OK, (v.perms & PERM_CONTROL) ? L"Connected to " + fid : L"Connected to " + fid + L" (view only)");
        history_record(target, password);   // a password that worked is saved (DPAPI) for one-click reconnects

        HINSTANCE hinst = GetModuleHandleW(nullptr);
        static std::once_flag reg;
        std::call_once(reg, [hinst] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = viewer_proc;
            wc.hInstance = hinst;
            wc.lpszClassName = VIEWER_CLASS;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
            RegisterClassW(&wc);
        });
        v.code = widen(v.ch.code());   // shown in the toolbar
        std::wstring title = fid + L"  \u2014  RemoteDesk";
        // Roomy by default (85% of the screen, centred), so the whole toolbar fits with labels.
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int ww = std::max<int>(1024, (wa.right - wa.left) * 85 / 100), wh = std::max<int>(640, (wa.bottom - wa.top) * 85 / 100);
        CreateWindowExW(0, VIEWER_CLASS, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, wa.left + ((wa.right - wa.left) - ww) / 2,
                        wa.top + ((wa.bottom - wa.top) - wh) / 2, ww, wh, nullptr, nullptr, hinst, &v);
        if (!v.hwnd) { closesocket(s); return; }
        ui::style_window(v.hwnd);
        // Title bar / Alt+Tab and taskbar icons, each loaded at its native size.
        SendMessageW(v.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        SendMessageW(v.hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                     GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
        // File transfer engine: asks the license and the host's permission for every file.
        HWND vh = v.hwnd;
        Viewer* vp = &v;
        v.files = std::make_unique<FileEngine>(
            v.ch, target, [vp] { return files_refusal(vp); }, [vh] { PostMessageW(vh, WM_V_FILES, 0, 0); },
            [vh](const Transfer& t) {
                auto* copy = new Transfer(t);
                if (!PostMessageW(vh, WM_V_FILEDONE, 0, (LPARAM)copy)) delete copy;
            });
        // Sound and calls: state changes and short messages go to the window's thread.
        auto note = [vh](const std::wstring& t) {
            auto* m = new std::wstring(t);
            if (!PostMessageW(vh, WM_V_NOTE, 0, (LPARAM)m)) delete m;
        };
        v.sound = std::make_unique<SoundReceiver>();
        v.call = std::make_unique<CallLink>(v.ch, [vh] { PostMessageW(vh, WM_V_CALL, 0, 0); }, note);
        ShowWindow(v.hwnd, SW_SHOW);
        SetTimer(v.hwnd, TIMER_PING, PING_EVERY_MS, nullptr);
        // Files left unfinished with this computer last time continue now: show them.
        if (const int n = FileEngine::pending_for(target)) {
            set_panel(&v, true);
            toast(&v, L"Continuing " + std::to_wstring(n) + (n == 1 ? L" unfinished file" : L" unfinished files"));
        }
        SetForegroundWindow(v.hwnd);
        const int key = session_add({ 0, target, false, v.hwnd, INVALID_SOCKET, GetTickCount64() });

        tl_viewer = &v;
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook, hinst, 0);
        std::thread rx(viewer_rx, &v);

        MSG m;   // no TranslateMessage: keys are forwarded raw, dead keys resolve remotely
        while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);

        if (hook) UnhookWindowsHookEx(hook);
        tl_viewer = nullptr;
        shutdown(s, SD_BOTH);
        rx.join();
        if (v.call) v.call->end();
        if (v.sound) v.sound->stop();
        if (v.files) v.files->stop();
        session_remove(key);
        if (v.dib) DeleteObject(v.dib);
    }
    closesocket(s);
}
