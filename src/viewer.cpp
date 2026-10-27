// Viewer side: one window + thread per outgoing connection.
#include "app.h"
#include "crypto.h"
#include "codec.h"
#include <windowsx.h>
#include <set>
#include <thread>

constexpr UINT WM_V_RESIZE  = WM_APP + 50;   // wParam = w, lParam = h (sent)
constexpr UINT WM_V_SETCLIP = WM_APP + 51;   // lParam = std::wstring*
constexpr UINT WM_V_LOST    = WM_APP + 52;
constexpr UINT WM_V_SCREENS = WM_APP + 53;   // lParam = const std::vector<uint8_t>* MSG_MONITORS payload (sent)
constexpr UINT ID_SCREEN_ALL = 1000, ID_SCREEN_BASE = 1001;   // menu bar commands
static const wchar_t* VIEWER_CLASS = L"RemoteDeskViewer";

struct Viewer {
    explicit Viewer(SOCKET s) : ch(s) {}
    SecureChannel ch;
    std::wstring remoteId;
    HWND hwnd = nullptr;

    std::mutex mx;                 // guards dib/bits/rw/rh
    HBITMAP dib = nullptr;
    uint8_t* bits = nullptr;       // top-down BGRA
    int rw = 0, rh = 0;
    bool sized = false;

    RECT dst{};                    // remote image area in client coords (UI thread)
    std::set<uint16_t> keysDown;   // released if we lose focus
    unsigned buttons = 0;
    std::mutex clipMx;
    std::wstring lastClip;
    bool closing = false;

    std::vector<MonitorDesc> screens;   // remote monitors (UI thread)
    int screen = 0;                     // shown monitor index, -1 = all screens
    std::vector<HBITMAP> menuIcons;
};

// ---- input forwarding ----
static void send_key(Viewer* v, uint16_t vk, uint16_t scan, bool down, bool ext) {
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
    if (dw <= 0 || dh <= 0 || v->rw <= 0) return false;
    m.x = std::clamp((int)((int64_t)(GET_X_LPARAM(lp) - v->dst.left) * v->rw / dw), 0, v->rw - 1);
    m.y = std::clamp((int)((int64_t)(GET_Y_LPARAM(lp) - v->dst.top) * v->rh / dh), 0, v->rh - 1);
    return true;
}

// Low-level hook so Win, Alt+Tab, Ctrl+Esc etc. go to the remote machine
// instead of the local shell while a viewer window is focused.
static thread_local Viewer* tl_viewer = nullptr;

static LRESULT CALLBACK kb_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && tl_viewer && GetForegroundWindow() == tl_viewer->hwnd) {
        auto* kb = (KBDLLHOOKSTRUCT*)lp;
        bool alt = (kb->flags & LLKHF_ALTDOWN) != 0;
        bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
        DWORD vk = kb->vkCode;
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

// ---- screen menu ----
// 16x16 glyphs: '#' frame, '.' screen (filled when selected), ' ' transparent.
static const char* ICON_SCREEN[16] = {
    "                ", "                ", " ############## ", " #............# ",
    " #............# ", " #............# ", " #............# ", " #............# ",
    " #............# ", " #............# ", " #............# ", " ############## ",
    "       ##       ", "       ##       ", "    ########    ", "                ",
};
static const char* ICON_ALL_SCREENS[16] = {
    "                ", "                ", "                ", "#######  #######",
    "#.....#  #.....#", "#.....#  #.....#", "#.....#  #.....#", "#.....#  #.....#",
    "#.....#  #.....#", "#.....#  #.....#", "#######  #######", "   #        #   ",
    "   #        #   ", "  ###      ###  ", "                ", "                ",
};

// Draws a glyph into a 32bpp alpha bitmap, which menus show next to the item text.
static HBITMAP make_icon(const char* const* glyph, int size, bool selected) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    uint32_t* px = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    if (!bmp) return nullptr;
    const uint32_t frame = selected ? 0xFF0067C0 : 0xFF5A5A5A;   // opaque, so no premultiply needed
    const uint32_t fill = 0xFF4BA0E6;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            char c = glyph[y * 16 / size][x * 16 / size];
            px[y * size + x] = c == '#' ? frame : (c == '.' && selected) ? fill : 0;
        }
    return bmp;
}

static void build_screen_menu(Viewer* v) {
    HMENU old = GetMenu(v->hwnd);
    for (HBITMAP b : v->menuIcons) DeleteObject(b);
    v->menuIcons.clear();
    HMENU menu = nullptr;
    if (v->screens.size() > 1) {
        menu = CreateMenu();
        const int size = MulDiv(16, GetDpiForWindow(v->hwnd), 96);
        auto add = [&](UINT id, const std::wstring& text, const char* const* glyph, bool selected) {
            HBITMAP icon = make_icon(glyph, size, selected);
            if (icon) v->menuIcons.push_back(icon);
            MENUITEMINFOW mi{ sizeof(mi) };
            mi.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP;
            mi.wID = id;
            mi.dwTypeData = (LPWSTR)text.c_str();
            mi.hbmpItem = icon;
            InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mi);
        };
        for (size_t i = 0; i < v->screens.size(); ++i)
            add(ID_SCREEN_BASE + (UINT)i,
                L"Screen " + std::to_wstring(i + 1) + (v->screens[i].primary ? L" (main)" : L""),
                ICON_SCREEN, v->screen == (int)i);
        add(ID_SCREEN_ALL, L"All screens", ICON_ALL_SCREENS, v->screen < 0);
    }
    SetMenu(v->hwnd, menu);
    if (old) DestroyMenu(old);
}

// ---- window ----
static void layout(Viewer* v, const RECT& rc) {
    int cw = rc.right, ch = rc.bottom;
    if (v->rw <= 0 || v->rh <= 0 || cw <= 0 || ch <= 0) { v->dst = rc; return; }
    double s = std::min((double)cw / v->rw, (double)ch / v->rh);
    int w = (int)(v->rw * s + 0.5), h = (int)(v->rh * s + 0.5);
    v->dst.left = (cw - w) / 2;
    v->dst.top = (ch - h) / 2;
    v->dst.right = v->dst.left + w;
    v->dst.bottom = v->dst.top + h;
}

static void fit_window(Viewer* v) {
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(v->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int aw = (mi.rcWork.right - mi.rcWork.left) * 9 / 10;
    int ah = (mi.rcWork.bottom - mi.rcWork.top) * 9 / 10;
    double s = std::min(1.0, std::min((double)aw / v->rw, (double)ah / v->rh));
    RECT r{ 0, 0, (int)(v->rw * s), (int)(v->rh * s) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, GetMenu(v->hwnd) != nullptr);
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
        build_screen_menu(v);
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id != ID_SCREEN_ALL && (id < ID_SCREEN_BASE || id >= ID_SCREEN_BASE + v->screens.size())) break;
        int32_t want = id == ID_SCREEN_ALL ? -1 : (int32_t)(id - ID_SCREEN_BASE);
        if (want != v->screen) v->ch.send(MSG_SELECT_SCREEN, &want, sizeof(want));   // host answers with MSG_MONITORS
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        layout(v, rc);
        const RECT& d = v->dst;
        HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bars[4] = { { 0, 0, rc.right, d.top }, { 0, d.bottom, rc.right, rc.bottom },
                         { 0, d.top, d.left, d.bottom }, { d.right, d.top, rc.right, d.bottom } };
        for (RECT& b : bars) if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, bg);
        {
            std::lock_guard<std::mutex> lk(v->mx);
            if (v->dib) {
                HDC mem = CreateCompatibleDC(dc);
                HGDIOBJ old = SelectObject(mem, v->dib);
                int dw = d.right - d.left, dh = d.bottom - d.top;
                SetStretchBltMode(dc, (dw == v->rw && dh == v->rh) ? COLORONCOLOR : HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchBlt(dc, d.left, d.top, dw, dh, mem, 0, 0, v->rw, v->rh, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
            } else {
                FillRect(dc, &rc, bg);
                SetTextColor(dc, RGB(200, 200, 200));
                SetBkMode(dc, TRANSPARENT);
                DrawTextW(dc, L"Waiting for image...", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        MouseMove m;
        if (to_remote(v, lp, m)) v->ch.send(MSG_MOUSE_MOVE, &m, sizeof(m));
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        uint8_t b = (msg <= WM_LBUTTONUP) ? 0 : (msg <= WM_RBUTTONUP) ? 1 : 2;
        bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        MouseMove m;
        if (to_remote(v, lp, m)) v->ch.send(MSG_MOUSE_MOVE, &m, sizeof(m));
        MouseBtn mb{ b, (uint8_t)down };
        v->ch.send(MSG_MOUSE_BTN, &mb, sizeof(mb));
        if (down) { v->buttons |= 1u << b; SetCapture(hwnd); }
        else { v->buttons &= ~(1u << b); if (!v->buttons) ReleaseCapture(); }
        return 0;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        MouseWheel w{ GET_WHEEL_DELTA_WPARAM(wp), (uint8_t)(msg == WM_MOUSEHWHEEL) };
        v->ch.send(MSG_MOUSE_WHEEL, &w, sizeof(w));
        return 0;
    }

    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP: case WM_SYSKEYUP:
        send_key(v, (uint16_t)wp, (uint16_t)((lp >> 16) & 0xFF),
                 msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, ((lp >> 24) & 1) != 0);
        return 0;   // swallow, so Alt / F10 / Alt+F4 go to the remote side
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        return 0;

    case WM_KILLFOCUS:
        release_all(v);
        return 0;

    case WM_CLIPBOARDUPDATE: {
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

    case WM_V_LOST:
        if (!v->closing) {
            v->closing = true;
            MessageBoxW(hwnd, (L"The session with " + v->remoteId + L" has ended.").c_str(),
                        L"RemoteDesk", MB_ICONINFORMATION);
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        v->closing = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        for (HBITMAP b : v->menuIcons) DeleteObject(b);   // the menu itself goes with the window
        v->menuIcons.clear();
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
            } else if (type == MSG_FRAME_END) {
                InvalidateRect(v->hwnd, nullptr, FALSE);
                v->ch.send(MSG_FRAME_ACK, nullptr, 0);
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

static void fail(const std::wstring& text) {
    post_text(WM_APP_INFO, L"");
    MessageBoxW(nullptr, text.c_str(), L"RemoteDesk", MB_ICONERROR | MB_SETFOREGROUND);
}

void viewer_session(std::string target, std::string password) {
    const std::wstring fid = widen(format_id(target));
    post_text(WM_APP_INFO, L"Connecting to " + fid + L"...");

    SOCKET s = relay_open("CONN " + target);
    if (s == INVALID_SOCKET) { fail(L"Could not reach the relay server. Check the relay address."); return; }
    std::string acc, line;
    if (read_line(s, acc, line, 75000) != LineResult::Ok || line != "READY") {
        closesocket(s);
        if (line == "OFFLINE") fail(L"The remote desk " + fid + L" is not online.");
        else if (line == "TIMEOUT") fail(L"The remote desk " + fid + L" did not answer.");
        else fail(L"Connection to " + fid + L" failed.");
        return;
    }

    {
        Viewer v(s);
        v.remoteId = fid;
        if (!v.ch.handshake(true)) { closesocket(s); fail(L"Secure handshake failed."); return; }
        std::string hello = my_id() + "\n" + password;
        v.ch.send(MSG_HELLO, hello.data(), (uint32_t)hello.size());
        post_text(WM_APP_INFO, L"Waiting for " + fid + L" to accept...   Security code: " +
                               widen(v.ch.code()));

        set_recv_timeout(s, 180000);   // the remote user may take a while to click Accept
        uint8_t type = 0;
        std::vector<uint8_t> p;
        if (!v.ch.recv(type, p) || type != MSG_ACCEPT) {
            closesocket(s);
            fail(type == MSG_REJECT ? widen(std::string(p.begin(), p.end()))
                                    : L"The remote desk did not respond.");
            return;
        }
        set_recv_timeout(s, 0);
        post_text(WM_APP_INFO, L"");

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
        std::wstring title = L"RemoteDesk - " + fid + L"    (security code " + widen(v.ch.code()) + L")";
        CreateWindowExW(0, VIEWER_CLASS, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        1024, 640, nullptr, nullptr, hinst, &v);
        if (!v.hwnd) { closesocket(s); return; }
        // Title bar / Alt+Tab and taskbar icons, each loaded at its native size.
        SendMessageW(v.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        SendMessageW(v.hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                     GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
        ShowWindow(v.hwnd, SW_SHOW);
        SetForegroundWindow(v.hwnd);

        tl_viewer = &v;
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook, hinst, 0);
        std::thread rx(viewer_rx, &v);

        MSG m;   // no TranslateMessage: keys are forwarded raw, dead keys resolve remotely
        while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);

        if (hook) UnhookWindowsHookEx(hook);
        tl_viewer = nullptr;
        shutdown(s, SD_BOTH);
        rx.join();
        if (v.dib) DeleteObject(v.dib);
    }
    closesocket(s);
}
