// Host side: serves one viewer per call. Captures the monitor the viewer picked
// (or all of them), sends only changed regions as JPEG, and injects the viewer's input.
#include "app.h"
#include "crypto.h"
#include "codec.h"
#include <chrono>
#include <set>
#include <thread>

static std::mutex g_sessMx;
static std::set<SOCKET> g_sessions;

static void post_session_count() {
    size_t n;
    {
        std::lock_guard<std::mutex> lk(g_sessMx);
        n = g_sessions.size();
    }
    PostMessageW(g_mainWnd, WM_APP_SESSIONS, (WPARAM)n, 0);
}

void disconnect_all_incoming() {
    std::lock_guard<std::mutex> lk(g_sessMx);
    for (SOCKET s : g_sessions) shutdown(s, SD_BOTH);
}

// ---- monitors ----
static BOOL CALLBACK add_monitor(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(m, &mi)) {
        const RECT& r = mi.rcMonitor;
        ((std::vector<MonitorDesc>*)lp)->push_back(
            { r.left, r.top, r.right - r.left, r.bottom - r.top, (uint8_t)((mi.dwFlags & MONITORINFOF_PRIMARY) != 0) });
    }
    return TRUE;
}

// Sorted left to right (then top to bottom), so "Screen 1" is the leftmost.
static std::vector<MonitorDesc> list_monitors() {
    std::vector<MonitorDesc> v;
    EnumDisplayMonitors(nullptr, nullptr, add_monitor, (LPARAM)&v);
    std::sort(v.begin(), v.end(), [](const MonitorDesc& a, const MonitorDesc& b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    if (v.size() > MAX_MONITORS) v.resize(MAX_MONITORS);
    return v;
}

static bool same_layout(const std::vector<MonitorDesc>& a, const std::vector<MonitorDesc>& b) {
    return a.size() == b.size() && (a.empty() || !memcmp(a.data(), b.data(), a.size() * sizeof(MonitorDesc)));
}

// ---- screen capture (GDI into a DIB section covering one monitor or all of them) ----
struct Capturer {
    int x = 0, y = 0, w = 0, h = 0;
    HDC screen = nullptr, mem = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint8_t* bits = nullptr;   // top-down BGRA, stride w*4

    ~Capturer() { release(); }

    void release() {
        if (mem && old) SelectObject(mem, old);
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        if (screen) ReleaseDC(nullptr, screen);
        screen = mem = nullptr; bmp = nullptr; old = nullptr; bits = nullptr;
    }

    bool init(const RECT& area) {
        release();
        x = area.left;
        y = area.top;
        w = area.right - area.left;
        h = area.bottom - area.top;
        if (w <= 0 || h <= 0) return false;
        screen = GetDC(nullptr);
        mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (!bmp) return false;
        old = SelectObject(mem, bmp);
        return true;
    }

    // Colored halo under the remote pointer so the viewer can tell it from its own cursor.
    void highlight(int cx, int cy) {
        const int R = 14;                            // radius in pixels
        const int cr = 255, cg = 140, cb = 0, a = 110; // orange, ~43% opacity
        GdiFlush();
        for (int dy = -R; dy <= R; dy++) {
            int py = cy + dy;
            if (py < 0 || py >= h) continue;
            for (int dx = -R; dx <= R; dx++) {
                int px = cx + dx;
                if (px < 0 || px >= w || dx * dx + dy * dy > R * R) continue;
                uint8_t* p = bits +((size_t)py * w + px) * 4;   // BGRA
                p[0] = (uint8_t)((p[0] * (255 - a) + cb * a) / 255);
                p[1] = (uint8_t)((p[1] * (255 - a) + cg * a) / 255);
                p[2] = (uint8_t)((p[2] * (255 - a) + cr * a) / 255);
            }
        }
    }

    void grab() {
        // Fails while the secure desktop (UAC / lock screen) is up; keep last frame.
        BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
        CURSORINFO ci{ sizeof(ci) };
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
            highlight(ci.ptScreenPos.x - x, ci.ptScreenPos.y - y);
            ICONINFO ii{};
            if (GetIconInfo(ci.hCursor, &ii)) {
                DrawIconEx(mem, ci.ptScreenPos.x - x - (int)ii.xHotspot,
                           ci.ptScreenPos.y - y - (int)ii.yHotspot, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
        }
        GdiFlush();
    }
};

struct HostSession {
    explicit HostSession(SOCKET s) : ch(s) {}
    SecureChannel ch;
    std::atomic<bool> alive{ true };
    std::atomic<int> inflight{ 0 };
    std::atomic<int> wantScreen{ 0 };    // monitor index the viewer asked for, -1 = all
    std::mutex geoMx;                    // guards cap/desk, for mouse mapping
    RECT cap{ 0, 0, 1, 1 };              // captured area, virtual-desktop coords
    RECT desk{ 0, 0, 1, 1 };             // whole virtual desktop
    std::mutex clipMx;
    std::wstring lastClip;
};

// ---- input injection ----
static void inject(HostSession& hs, uint8_t type, const std::vector<uint8_t>& p) {
    INPUT in{};
    if (type == MSG_MOUSE_MOVE && p.size() == sizeof(MouseMove)) {
        MouseMove m; memcpy(&m, p.data(), sizeof(m));
        RECT c, d;
        {
            std::lock_guard<std::mutex> lk(hs.geoMx);
            c = hs.cap; d = hs.desk;
        }
        // Viewer coords are relative to the captured area; SendInput wants 0..65535 over the virtual desktop.
        int vx = c.left + std::clamp(m.x, 0, (int)(c.right - c.left) - 1) - d.left;
        int vy = c.top + std::clamp(m.y, 0, (int)(c.bottom - c.top) - 1) - d.top;
        int dw = d.right - d.left, dh = d.bottom - d.top;
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dx = (LONG)((int64_t)vx * 65535 / std::max(dw - 1, 1));
        in.mi.dy = (LONG)((int64_t)vy * 65535 / std::max(dh - 1, 1));
        SendInput(1, &in, sizeof(in));
    } else if (type == MSG_MOUSE_BTN && p.size() == sizeof(MouseBtn)) {
        MouseBtn b; memcpy(&b, p.data(), sizeof(b));
        static const DWORD down[] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN };
        static const DWORD up[]   = { MOUSEEVENTF_LEFTUP,   MOUSEEVENTF_RIGHTUP,   MOUSEEVENTF_MIDDLEUP };
        if (b.button > 2) return;
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = b.down ? down[b.button] : up[b.button];
        SendInput(1, &in, sizeof(in));
    } else if (type == MSG_MOUSE_WHEEL && p.size() == sizeof(MouseWheel)) {
        MouseWheel mw; memcpy(&mw, p.data(), sizeof(mw));
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = mw.horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)mw.delta;
        SendInput(1, &in, sizeof(in));
    } else if (type == MSG_KEY && p.size() == sizeof(KeyEvent)) {
        KeyEvent k; memcpy(&k, p.data(), sizeof(k));
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = k.vk;
        in.ki.wScan = k.scan;
        in.ki.dwFlags = (k.down ? 0 : KEYEVENTF_KEYUP) | (k.extended ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof(in));
    }
}

static void rx_loop(HostSession* hs) {
    uint8_t type;
    std::vector<uint8_t> p;
    while (hs->alive && hs->ch.recv(type, p)) {
        if (type == MSG_FRAME_ACK) {
            if (hs->inflight > 0) --hs->inflight;
        } else if (type == MSG_CLIPBOARD) {
            std::wstring text((const wchar_t*)p.data(), p.size() / sizeof(wchar_t));
            {
                std::lock_guard<std::mutex> lk(hs->clipMx);
                if (text == hs->lastClip) continue;
                hs->lastClip = text;
            }
            PostMessageW(g_mainWnd, WM_APP_SETCLIP, 0, (LPARAM)new std::wstring(std::move(text)));
        } else if (type == MSG_SELECT_SCREEN && p.size() == sizeof(int32_t)) {
            int32_t i; memcpy(&i, p.data(), sizeof(i));
            hs->wantScreen = std::clamp(i, -1, MAX_MONITORS - 1);
        } else {
            inject(*hs, type, p);
        }
    }
    hs->alive = false;
}

// ---- frame encoding ----
static bool send_changes(HostSession& hs, const Capturer& cap, std::vector<uint8_t>& prev,
                         Jpeg& jpeg, std::vector<uint8_t>& jpg, std::vector<uint8_t>& pkt) {
    const size_t stride = (size_t)cap.w * 4;
    const bool full = prev.size() != stride * cap.h;
    const int cols = (cap.w + TILE - 1) / TILE;
    bool any = false;

    for (int ty = 0; ty < cap.h; ty += TILE) {
        const int th = std::min(TILE, cap.h - ty);
        auto dirty = [&](int c) {
            if (full) return true;
            const size_t off = (size_t)ty * stride + (size_t)c * TILE * 4;
            const size_t bytes = (size_t)std::min(TILE, cap.w - c * TILE) * 4;
            for (int r = 0; r < th; ++r)
                if (memcmp(cap.bits + off + r * stride, prev.data() + off + r * stride, bytes)) return true;
            return false;
        };
        // Merge horizontally adjacent dirty tiles into one rectangle.
        int run = -1;
        for (int c = 0; c <= cols; ++c) {
            const bool d = c < cols && dirty(c);
            if (d && run < 0) run = c;
            if (d || run < 0) continue;
            RectHeader rh{ run * TILE, ty, std::min(c * TILE, cap.w) - run * TILE, th };
            run = -1;
            if (!jpeg.encode(cap.bits + (size_t)rh.y * stride + (size_t)rh.x * 4, (int)stride,
                             rh.w, rh.h, JPEG_QUALITY, jpg)) return false;
            pkt.resize(sizeof(rh) + jpg.size());
            memcpy(pkt.data(), &rh, sizeof(rh));
            memcpy(pkt.data() + sizeof(rh), jpg.data(), jpg.size());
            if (!hs.ch.send(MSG_RECT, pkt.data(), (uint32_t)pkt.size())) return false;
            any = true;
        }
    }
    if (!any) return true;
    prev.assign(cap.bits, cap.bits + stride * cap.h);
    ++hs.inflight;
    return hs.ch.send(MSG_FRAME_END, nullptr, 0);
}

static void send_clipboard(HostSession& hs) {
    std::wstring text;
    if (!clipboard_get(nullptr, text) || text.size() > MAX_CLIPBOARD_CHARS) return;
    {
        std::lock_guard<std::mutex> lk(hs.clipMx);
        if (text == hs.lastClip) return;
        hs.lastClip = text;
    }
    hs.ch.send(MSG_CLIPBOARD, text.data(), (uint32_t)(text.size() * sizeof(wchar_t)));
}

static void stream(HostSession& hs) {
    Jpeg jpeg;
    Capturer cap;
    if (!jpeg.ok()) return;

    std::vector<MonitorDesc> mons;
    std::vector<uint8_t> prev, jpg, pkt;
    int screen = 0;   // captured monitor index, -1 = all screens

    // (Re)start capturing monitor `want` and tell the viewer the screen list and new size.
    auto select = [&](int want) {
        mons = list_monitors();
        if (mons.empty()) return false;
        if (want >= (int)mons.size() || (want < 0 && mons.size() == 1)) want = -2;
        if (want == -2)   // default: the primary monitor
            want = (int)(std::find_if(mons.begin(), mons.end(), [](const MonitorDesc& m) { return m.primary; }) -
                         mons.begin()) % (int)mons.size();
        RECT desk{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
        desk.right = desk.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        desk.bottom = desk.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        RECT area = desk;
        if (want >= 0) {
            const MonitorDesc& m = mons[want];
            area = { m.x, m.y, m.x + m.w, m.y + m.h };
        }
        if (!cap.init(area)) return false;
        {
            std::lock_guard<std::mutex> lk(hs.geoMx);
            hs.cap = area; hs.desk = desk;
        }
        screen = want;
        hs.wantScreen = want;
        prev.clear();
        hs.inflight = 0;

        std::vector<uint8_t> msg(sizeof(int32_t) + mons.size() * sizeof(MonitorDesc));
        int32_t sel = want;
        memcpy(msg.data(), &sel, sizeof(sel));
        memcpy(msg.data() + sizeof(sel), mons.data(), mons.size() * sizeof(MonitorDesc));
        ScreenInfo si{ cap.w, cap.h };
        return hs.ch.send(MSG_MONITORS, msg.data(), (uint32_t)msg.size()) &&
               hs.ch.send(MSG_SCREEN_INFO, &si, sizeof(si));
    };
    if (!select(-2)) return;

    std::thread rx(rx_loop, &hs);
    DWORD lastSeq = GetClipboardSequenceNumber();

    for (int frame = 1; hs.alive; ++frame) {
        auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(FRAME_INTERVAL_MS);
        if (hs.wantScreen != screen) {         // viewer switched screens
            if (!select(hs.wantScreen)) break;
        } else if (frame % 15 == 0 && !same_layout(list_monitors(), mons)) {   // resolution / layout changed
            if (!select(screen)) break;
        }
        if (hs.inflight < MAX_INFLIGHT_FRAMES) {
            cap.grab();
            if (!send_changes(hs, cap, prev, jpeg, jpg, pkt)) break;
        }
        DWORD seq = GetClipboardSequenceNumber();
        if (seq != lastSeq) {
            lastSeq = seq;
            if (seq != g_clipIgnoreSeq) send_clipboard(hs);
        }
        std::this_thread::sleep_until(next);
    }
    hs.alive = false;
    shutdown(hs.ch.socket(), SD_BOTH);
    rx.join();
}

// ---- access control ----
static bool authorize(HostSession& hs) {
    uint8_t type;
    std::vector<uint8_t> p;
    if (!hs.ch.recv(type, p) || type != MSG_HELLO) return false;
    std::string hello(p.begin(), p.end());
    size_t nl = hello.find('\n');
    std::string viewerId = digits_only(hello.substr(0, nl));
    std::string pw = nl == std::string::npos ? "" : hello.substr(nl + 1);

    auto reject = [&](const char* why) {
        hs.ch.send(MSG_REJECT, why, (uint32_t)strlen(why));
        return false;
    };

    if (!pw.empty()) {   // unattended access
        if (password_matches(pw)) return hs.ch.send(MSG_ACCEPT, nullptr, 0);
        Sleep(1500);     // slow down guessing
        return reject(password_is_set() ? "Wrong password." : "Unattended access is not enabled on the remote desk.");
    }

    std::wstring msg = L"Incoming connection request\n\n"
                       L"From:  " + widen(format_id(viewerId)) + L"\n"
                       L"Security code:  " + widen(hs.ch.code()) + L"\n\n"
                       L"Allow this person to see your screen and control your mouse and keyboard?\n\n"
                       L"Only accept if you know who is connecting. The security code must "
                       L"match the one shown on their side.";
    int r = MessageBoxW(nullptr, msg.c_str(), L"RemoteDesk - incoming connection",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
    if (r != IDYES) return reject("The remote user declined the connection.");
    return hs.ch.send(MSG_ACCEPT, nullptr, 0);
}

void host_session(std::string sid) {
    SOCKET s = relay_open("JOIN " + sid);
    if (s == INVALID_SOCKET) return;
    std::string acc, line;
    if (read_line(s, acc, line, 15000) != LineResult::Ok || line != "READY") { closesocket(s); return; }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        HostSession hs(s);
        set_recv_timeout(s, 30000);          // handshake + hello must arrive promptly
        if (hs.ch.handshake(false) && authorize(hs)) {
            set_recv_timeout(s, 0);
            {
                std::lock_guard<std::mutex> lk(g_sessMx);
                g_sessions.insert(s);
            }
            post_session_count();
            SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
            stream(hs);
            SetThreadExecutionState(ES_CONTINUOUS);
            {
                std::lock_guard<std::mutex> lk(g_sessMx);
                g_sessions.erase(s);
            }
            post_session_count();
        }
    }
    CoUninitialize();
    closesocket(s);
}
