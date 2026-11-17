// Host side: serves one viewer per call. Captures the monitor the viewer picked
// (or all of them), sends only changed regions as JPEG, and injects the viewer's input.
#include "core/app.h"
#include "core/crypto.h"
#include "core/codec.h"
#include "remote/files.h"
#include "remote/audio.h"
#include "app/license.h"
#include "app/service.h"
#include <functional>
#include "ui/ui.h"
#include <chrono>
#include <memory>
#include <map>
#include <thread>

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

    // Soft emerald ring around the remote pointer, so the viewer can tell it from its own
    // (ring-shaped) local pointer and spot it on any background.
    void highlight(int cx, int cy) {
        GdiFlush();
        ui::draw_pointer_halo(bits, w, h, cx, cy);
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

// ---- the input desktop ----
// The agent (SYSTEM) moves the capturing and input threads to whatever desktop has the input:
// the normal desktop, the lock or sign-in screen (Winlogon), or a UAC prompt. In the app the
// secure desktops can't be opened, so this does nothing there. `before` runs just ahead of a switch
// (the capture must let go of the old desktop's screen first). True = switched.
static bool follow_input_desktop(const std::function<void()>& before = {}) {
    HDESK in = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!in) return false;
    wchar_t now[128] = {}, here[128] = {};
    DWORD n = 0;
    GetUserObjectInformationW(in, UOI_NAME, now, sizeof(now), &n);
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, here, sizeof(here), &n);
    if (!lstrcmpiW(now, here)) { CloseDesktop(in); return false; }
    if (before) before();
    static thread_local HDESK opened = nullptr;
    if (!SetThreadDesktop(in)) { CloseDesktop(in); return false; }
    if (opened) CloseDesktop(opened);
    opened = in;
    return true;
}

// Without the service the lock screen, sign-in and UAC prompts are out of reach: the input
// desktop can't even be opened then. The viewer is told, instead of looking at a frozen picture.
static bool input_out_of_reach() {
    HDESK in = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!in) return true;
    wchar_t name[128] = {};
    DWORD n = 0;
    GetUserObjectInformationW(in, UOI_NAME, name, sizeof(name), &n);
    CloseDesktop(in);
    return lstrcmpiW(name, L"Default") != 0;
}

// Ctrl+Alt+Del can't be typed with SendInput; Windows lets a service (our agent's session is
// SYSTEM) ask for it through sas.dll when the SoftwareSASGeneration policy allows (setup sets it).
static void send_sas() {
    static HMODULE sas = LoadLibraryW(L"sas.dll");
    using Fn = VOID(WINAPI*)(BOOL);
    if (auto fn = sas ? (Fn)GetProcAddress(sas, "SendSAS") : nullptr) fn(FALSE);
}

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
    std::atomic<uint32_t> peerPro{ 0 };     // Pro features unlocked by the viewer's license
    std::atomic<uint8_t> perms{ PERM_ALL };   // what this viewer may do now (enforced below; can change mid-session)
    std::mutex heldMx;                   // keys / buttons the viewer is holding down on this PC
    std::map<uint16_t, std::pair<uint16_t, bool>> heldKeys;   // vk -> scan code, extended
    unsigned heldButtons = 0;
    int key = 0;                         // in the session registry
    std::atomic<ULONGLONG> lastRx{ GetTickCount64() };   // anything from the viewer (heartbeat)
    std::atomic<bool> pinged{ false };   // the viewer sends MSG_PING (1.4.1+): silence means it's gone
    std::unique_ptr<FileEngine> files;   // file transfer (needs PERM_FILES); destroyed before ch
    std::unique_ptr<SoundSender> sound;  // this PC's sound to the viewer (needs PERM_AUDIO)
    std::unique_ptr<CallLink> call;      // voice call with the viewer
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
        {
            std::lock_guard<std::mutex> lk(hs.heldMx);   // remembered, so revoking control can let go
            if (b.down && !(hs.perms & PERM_CONTROL)) return;   // revoked a moment ago
            if (b.down) hs.heldButtons |= 1u << b.button; else hs.heldButtons &= ~(1u << b.button);
            SendInput(1, &in, sizeof(in));
        }
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
        std::lock_guard<std::mutex> lk(hs.heldMx);
        if (k.down && !(hs.perms & PERM_CONTROL)) return;   // revoked a moment ago
        if (k.down) hs.heldKeys[k.vk] = { k.scan, k.extended != 0 }; else hs.heldKeys.erase(k.vk);
        SendInput(1, &in, sizeof(in));
    }
}

// Releases every key and mouse button the viewer is holding down on this PC.
static void release_held(HostSession& hs) {
    std::lock_guard<std::mutex> lk(hs.heldMx);
    static const DWORD up[] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP };
    for (int b = 0; b < 3; ++b)
        if (hs.heldButtons & (1u << b)) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = up[b];
            SendInput(1, &in, sizeof(in));
        }
    for (auto& [vk, k] : hs.heldKeys) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = k.first;
        in.ki.dwFlags = KEYEVENTF_KEYUP | (k.second ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof(in));
    }
    hs.heldButtons = 0;
    hs.heldKeys.clear();
}

static void rx_loop(HostSession* hs) {
    uint8_t type;
    std::vector<uint8_t> p;
    while (hs->alive && hs->ch.recv(type, p)) {
        hs->lastRx = GetTickCount64();
        if (type == MSG_PING) {
            hs->pinged = true;
        } else if (type == MSG_FRAME_ACK) {
            if (hs->inflight > 0) --hs->inflight;
        } else if (type == MSG_CLIPBOARD) {
            if (!(hs->perms & PERM_CLIPBOARD)) continue;   // not allowed: ignore
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
        } else if (hs->files && hs->files->handle(type, p)) {
            // file transfer message: handled by the engine
        } else if (type == MSG_PRO) {
            hs->peerPro = license_peer_features(std::string(p.begin(), p.end()));
        } else if (type == MSG_AUDIO_CTL && p.size() == 1) {
            if (hs->sound) hs->sound->set(p[0] != 0 && (hs->perms & PERM_AUDIO));
        } else if (hs->call && hs->call->handle(type, p)) {
            // call signalling / voice: handled by the call
        } else if (type == MSG_SAS) {
            if (g_agent && (hs->perms & PERM_CONTROL)) send_sas();
        } else if (type == MSG_RECORDING && p.size() == 1) {
            session_set_recording(hs->key, p[0] != 0);   // shown on this session in the main window
        } else if (hs->perms & PERM_CONTROL) {   // view only: input is dropped here, whatever the viewer sends
            static thread_local ULONGLONG lastDeskCheck = 0;
            if (GetTickCount64() - lastDeskCheck > 200) { lastDeskCheck = GetTickCount64(); follow_input_desktop(); }
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
    ULONGLONG lastPing = 0, lastLockCheck = 0;
    bool locked = false;

    for (int frame = 1; hs.alive; ++frame) {
        auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(FRAME_INTERVAL_MS);
        if (hs.wantScreen != screen) {         // viewer switched screens
            if (!select(hs.wantScreen)) break;
        } else if (frame % 15 == 0 && !same_layout(list_monitors(), mons)) {   // resolution / layout changed
            if (!select(screen)) break;
        }
        // Locked, signed out, UAC: capture whichever desktop has the input (agent only).
        if (follow_input_desktop([&] { cap.release(); })) {
            if (!select(screen)) break;
        }
        if (hs.inflight < MAX_INFLIGHT_FRAMES) {
            cap.grab();
            if (!send_changes(hs, cap, prev, jpeg, jpg, pkt)) break;
        }
        DWORD seq = GetClipboardSequenceNumber();
        if (seq != lastSeq) {
            lastSeq = seq;
            if (seq != g_clipIgnoreSeq && (hs.perms & PERM_CLIPBOARD)) send_clipboard(hs);
        }
        const ULONGLONG now = GetTickCount64();
        if (!g_agent && now - lastLockCheck >= 1000) {   // the agent follows the lock screen; the app can't
            lastLockCheck = now;
            const bool l = input_out_of_reach();
            if (l != locked) {
                locked = l;
                const uint8_t b = l ? 1 : 0;
                hs.ch.send(MSG_HOST_STATE, &b, 1);
            }
        }
        if (now - lastPing >= PING_EVERY_MS) {
            lastPing = now;
            if (!hs.ch.send(MSG_PING, nullptr, 0)) break;
        }
        if (hs.pinged && now - hs.lastRx > PING_TIMEOUT_MS) break;   // the viewer vanished without closing
        std::this_thread::sleep_until(next);
    }
    hs.alive = false;
    shutdown(hs.ch.socket(), SD_BOTH);
    rx.join();
}

// ---- access control ----
static bool authorize(HostSession& hs, std::string& viewerId) {
    uint8_t type;
    std::vector<uint8_t> p;
    if (!hs.ch.recv(type, p) || type != MSG_HELLO) return false;
    std::string hello(p.begin(), p.end());
    size_t nl = hello.find('\n');
    viewerId = digits_only(hello.substr(0, nl));
    std::string pw = nl == std::string::npos ? "" : hello.substr(nl + 1);

    auto reject = [&](const char* why) {
        hs.ch.send(MSG_REJECT, why, (uint32_t)strlen(why));
        return false;
    };

    // The granted permissions travel as MSG_ACCEPT's 1-byte payload (older viewers ignore it).
    auto accept = [&](uint8_t perms) {
        hs.perms = perms;
        return hs.ch.send(MSG_ACCEPT, &perms, 1);
    };

    if (!pw.empty()) {   // unattended access: the owner set the password, so full access
        if (password_matches(pw)) return accept(default_perms(true));   // Access page
        Sleep(1500);     // slow down guessing
        return reject(password_is_set() ? "Wrong password." : "Unattended access is not enabled on the remote desk.");
    }

    // Ask the person at this computer what to allow (view only at minimum, or decline).
    int perms = ask_access(viewerId, hs.ch.code());
    if (perms == -2) return reject("Nobody on that computer can approve the request right now. Connect with its password.");
    if (perms < 0) return reject("The remote user declined the connection.");
    return accept((uint8_t)perms);
}

// The person at this PC changed what the viewer may do (Session window): enforce it here at
// once, tell the viewer, and stop file transfers that are no longer allowed.
void host_apply_perms(HostSession* hs, uint8_t perms) {
    const uint8_t before = hs->perms.exchange(perms);
    if ((before & PERM_CONTROL) && !(perms & PERM_CONTROL)) release_held(*hs);   // nothing stays pressed
    hs->ch.send(MSG_PERMS, &perms, 1);
    if ((before & PERM_FILES) && !(perms & PERM_FILES) && hs->files) hs->files->cancel_all();
    if (!(perms & PERM_AUDIO) && hs->sound) hs->sound->set(false);   // listening revoked: stop at once
}

// The Session window / Access page (or the app, through the agent) acts on the call.
void host_call(HostSession* hs, int action) {
    if (!hs->call) return;
    switch (action) {
    case CALL_DO_START: hs->call->call(); break;
    case CALL_DO_ACCEPT: hs->call->answer(true); break;
    case CALL_DO_DECLINE: hs->call->answer(false); break;
    case CALL_DO_HANGUP: hs->call->hang_up(); break;
    case CALL_DO_MUTE: hs->call->set_muted(true); break;
    case CALL_DO_UNMUTE: hs->call->set_muted(false); break;
    }
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
        std::string viewerId;
        if (hs.ch.handshake(false) && authorize(hs, viewerId)) {
            set_recv_timeout(s, 0);
            if (const std::string t = license_token(); !t.empty()) hs.ch.send(MSG_PRO, t.data(), t.size());
            const std::wstring who = widen(format_id(viewerId));
            hs.files = std::make_unique<FileEngine>(
                hs.ch, viewerId,
                [&hs]() -> std::wstring {
                    if (!(hs.perms & PERM_FILES)) return L"File transfer isn't allowed for this session";
                    if (g_agent) {   // files belong in the signed-in person's Downloads
                        HANDLE t = console_user_token();
                        if (!t) return L"Nobody is signed in on the other computer to receive files";
                        CloseHandle(t);
                    }
                    if (!license_has(PRO_FILES) && !(hs.peerPro & PRO_FILES))
                        return L"File transfer needs RemoteDesk Pro on one of the two computers \u2014 add a key in Settings";
                    return L"";
                },
                [] {   // progress: the Active sessions list shows "Sending x · 45%" (5 updates a second at most)
                    static std::atomic<ULONGLONG> last{ 0 };
                    const ULONGLONG now = GetTickCount64();
                    if (now - last >= 200) { last = now; PostMessageW(g_mainWnd, WM_APP_SESSIONS, 0, 0); }
                },
                [who](const Transfer& t) {
                    PostMessageW(g_mainWnd, WM_APP_SESSIONS, 0, 0);   // final state of the progress line
                    if (t.state == Transfer::Done)
                        post_info(INFO_OK, t.outgoing ? L"Sent " + t.name + L" to " + who
                                                      : L"Received " + t.name + L" from " + who + L" \u00B7 in Downloads \u203A RemoteDesk");
                    else if (t.state == Transfer::Failed)
                        post_info(INFO_ERROR, t.name + L": " + t.error);
                });
            auto note = [who](const std::wstring& text) { post_info(INFO_HINT, who + L": " + text); };
            hs.sound = std::make_unique<SoundSender>(hs.ch, note);
            hs.call = std::make_unique<CallLink>(hs.ch, [&hs] {
                if (hs.key) session_set_call(hs.key, hs.call->state(), hs.call->muted());   // list + Session window
            }, note);
            // The same viewer again (e.g. after its network dropped): the old session is stale, end it.
            if (const int old = session_find(viewerId, true)) session_end(old);
            SessionInfo info{ 0, viewerId, true, nullptr, s, GetTickCount64() };
            info.files = hs.files.get();
            info.perms = hs.perms;
            info.host = &hs;
            const int key = session_add(info);
            hs.key = key;
            SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
            stream(hs);
            SetThreadExecutionState(ES_CONTINUOUS);
            session_remove(key);   // first: nobody can reach the engine through the registry any more
            hs.call->end();
            hs.sound->set(false);
            hs.files->stop();
            release_held(hs);   // the session may end mid-drag or with a key down
        }
    }
    CoUninitialize();
    closesocket(s);
}
