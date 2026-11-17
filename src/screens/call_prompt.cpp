// "… is calling you" — Accept / Decline. Used by the viewer window (the host calls it) and by the
// main window (a viewer calls this PC; the call itself may run in the service's agent).
#include "screens/main_window.h"
#include "remote/audio.h"
#include <map>
#include <mmdeviceapi.h>
#include <shellapi.h>

static const wchar_t* CALL_CLASS = L"RemoteDeskCallPrompt";
enum { IDC_ACCEPT = 10, IDC_DECLINE };
constexpr int W = 400, H = 210, PAD = 24;

struct CallPrompt {
    std::wstring who;
    std::function<void(bool)> answer;
    bool answered = false;
};

static void paint(HWND h, CallPrompt* c) {
    ui::Paint p(h);
    const UINT dpi = GetDpiForWindow(h);
    const ui::Palette P = ui::pal();
    const ui::Fonts& F = ui::fonts(dpi);
    ui::fill(p.dc, p.client, P.card);
    RECT ic = ui::rc(PAD, 24, 52, 52, dpi);
    ui::round_rect(p.dc, ic, ui::px(26, dpi), P.accentSoft);
    ui::glyph(p.dc, 0xE717 /* phone */, ic, F.iconLarge, P.accentText);
    ui::text(p.dc, c->who, ui::rc(PAD + 68, 26, W - PAD * 2 - 68, 28, dpi), F.title, P.text);
    ui::text(p.dc, L"is calling you on RemoteDesk", ui::rc(PAD + 68, 56, W - PAD * 2 - 68, 22, dpi), F.small, P.muted);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* c = (CallPrompt*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (c) paint(h, c);
        return 0;
    case WM_COMMAND:
        if (c && (LOWORD(wp) == IDC_ACCEPT || LOWORD(wp) == IDC_DECLINE)) {
            c->answered = true;
            if (c->answer) c->answer(LOWORD(wp) == IDC_ACCEPT);
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        if (c && !c->answered) { c->answered = true; if (c->answer) c->answer(false); }
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        delete c;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND show_call_prompt(HWND owner, const std::wstring& who, std::function<void(bool)> answer) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = hi;
        wc.lpszClassName = CALL_CLASS;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
        registered = true;
    }
    auto* c = new CallPrompt{ who, std::move(answer) };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, CALL_CLASS, L"Incoming call — RemoteDesk", style, 0, 0, 0, 0,
                             owner, nullptr, hi, c);
    if (!h) { delete c; return nullptr; }
    const UINT dpi = GetDpiForWindow(h);
    ui::style_window(h);
    const int bw = (W - PAD * 2 - 12) / 2;
    auto place = [&](HWND ctl, int x, int y, int w, int hh) {
        RECT r = ui::rc(x, y, w, hh, dpi);
        SetWindowPos(ctl, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
    };
    place(ui::make_control(h, IDC_DECLINE, ui::Kind::Danger, L"Decline", ui::ico::Close, ui::Surface::Card), PAD, H - PAD - 44, bw, 44);
    place(ui::make_control(h, IDC_ACCEPT, ui::Kind::Primary, L"Accept", 0xE717, ui::Surface::Card), PAD + bw + 12, H - PAD - 44, bw, 44);
    RECT r{ 0, 0, ui::px(W, dpi), ui::px(H, dpi) };
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    SetWindowPos(h, HWND_TOPMOST, wa.right - ww - 24, wa.bottom - wh - 24, ww, wh, SWP_SHOWWINDOW);   // bottom right, like a call app
    FlashWindow(h, TRUE);
    MessageBeep(MB_ICONASTERISK);
    return h;
}

// Windows' microphone privacy switches ("Deny" = off): the whole device, all apps, desktop apps.
static bool mic_blocked() {
    const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
    auto denied = [](HKEY root, const std::wstring& key) {
        wchar_t v[32] = {};
        DWORD sz = sizeof(v);
        return RegGetValueW(root, key.c_str(), L"Value", RRF_RT_REG_SZ, nullptr, v, &sz) == ERROR_SUCCESS && !lstrcmpiW(v, L"Deny");
    };
    return denied(HKEY_LOCAL_MACHINE, base) || denied(HKEY_CURRENT_USER, base) ||
           denied(HKEY_CURRENT_USER, std::wstring(base) + L"\\NonPackaged");
}

static bool mic_present() {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    bool found = false;
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eCommunications, &dev))) { found = true; dev->Release(); }
        en->Release();
    }
    if (com) CoUninitialize();
    return found;
}

bool ensure_microphone(HWND owner) {
    if (mic_blocked()) {
        const int r = MessageBoxW(owner,
            L"Windows is blocking the microphone, so the other person wouldn't hear you.\n\n"
            L"Open Settings › Privacy & security › Microphone and turn on “Microphone access” and "
            L"“Let desktop apps access your microphone”. Then start the call again.\n\n"
            L"Open the microphone settings now?",
            L"Microphone needed — RemoteDesk", MB_YESNO | MB_ICONWARNING);
        if (r == IDYES) ShellExecuteW(owner, L"open", L"ms-settings:privacy-microphone", nullptr, nullptr, SW_SHOWNORMAL);
        return false;
    }
    if (!mic_present()) {
        return MessageBoxW(owner,
            L"No microphone was found on this computer, so the other person won't hear you. You will still hear them.\n\n"
            L"Go ahead with the call?",
            L"No microphone — RemoteDesk", MB_YESNO | MB_ICONQUESTION) == IDYES;
    }
    return true;
}

void close_call_prompt(HWND h) {   // the caller gave up: close without answering
    if (!h || !IsWindow(h)) return;
    if (auto* c = (CallPrompt*)GetWindowLongPtrW(h, GWLP_USERDATA)) c->answered = true;
    DestroyWindow(h);
}

// Main window: one prompt per session that is ringing this PC; closed when it stops ringing.
void call_prompts_refresh(HWND owner) {
    static std::map<int, HWND> open;
    std::map<int, bool> ringing;
    for (const SessionInfo& s : sessions())
        if (s.incoming && s.call == CallLink::Ringing) ringing[s.key] = true;
    for (auto it = open.begin(); it != open.end();) {
        if (!IsWindow(it->second)) { it = open.erase(it); continue; }
        if (!ringing.count(it->first)) { close_call_prompt(it->second); it = open.erase(it); continue; }
        ++it;
    }
    for (const SessionInfo& s : sessions()) {
        if (!s.incoming || s.call != CallLink::Ringing || open.count(s.key)) continue;
        const int key = s.key;
        if (HWND h = show_call_prompt(owner, widen(format_id(s.peerId)), [key, owner](bool yes) {
                if (yes && !ensure_microphone(owner)) yes = false;   // can't talk: decline rather than a silent call
                session_call(key, yes ? CALL_DO_ACCEPT : CALL_DO_DECLINE);
            }))
            open[key] = h;
    }
}
