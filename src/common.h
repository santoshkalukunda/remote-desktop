#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

constexpr uint16_t DEFAULT_RELAY_PORT = 21116;
constexpr int TILE = 64;                  // change-detection tile size
constexpr int MAX_INFLIGHT_FRAMES = 2;    // host waits for viewer acks beyond this
constexpr int FRAME_INTERVAL_MS = 33;     // ~30 fps cap
constexpr float JPEG_QUALITY = 0.75f;

// ---- messages carried inside the encrypted channel ----
enum MsgType : uint8_t {
    MSG_HELLO        = 1,   // viewer->host: "<viewer id>\n<password>"
    MSG_ACCEPT       = 2,   // host->viewer
    MSG_REJECT       = 3,   // host->viewer: utf8 reason
    MSG_SCREEN_INFO  = 4,   // host->viewer: ScreenInfo
    MSG_RECT         = 5,   // host->viewer: RectHeader + JPEG bytes
    MSG_FRAME_END    = 6,   // host->viewer
    MSG_FRAME_ACK    = 7,   // viewer->host
    MSG_MOUSE_MOVE   = 8,   // viewer->host: MouseMove
    MSG_MOUSE_BTN    = 9,   // viewer->host: MouseBtn
    MSG_MOUSE_WHEEL  = 10,  // viewer->host: MouseWheel
    MSG_KEY          = 11,  // viewer->host: KeyEvent
    MSG_CLIPBOARD    = 12,  // both ways: UTF-16 text
    MSG_MONITORS     = 13,  // host->viewer: int32 selected (-1 = all) + MonitorDesc[]
    MSG_SELECT_SCREEN = 14, // viewer->host: int32 monitor index (-1 = all screens)
};
constexpr int MAX_MONITORS = 16;

#pragma pack(push, 1)
struct ScreenInfo { int32_t w, h; };
struct RectHeader { int32_t x, y, w, h; };
struct MouseMove  { int32_t x, y; };
struct MouseBtn   { uint8_t button; uint8_t down; };      // 0=L 1=R 2=M
struct MouseWheel { int32_t delta; uint8_t horizontal; };
struct KeyEvent   { uint16_t vk; uint16_t scan; uint8_t down; uint8_t extended; };
struct MonitorDesc { int32_t x, y, w, h; uint8_t primary; };   // virtual-desktop coords
#pragma pack(pop)

// ---- socket helpers (handle partial send/recv) ----
inline bool send_all(SOCKET s, const void* b, size_t len) {
    const char* p = (const char*)b;
    while (len > 0) {
        int n = send(s, p, (int)std::min<size_t>(len, 1 << 20), 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}
inline bool recv_all(SOCKET s, void* b, size_t len) {
    char* p = (char*)b;
    while (len > 0) {
        int n = recv(s, p, (int)std::min<size_t>(len, 1 << 20), 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}
inline bool send_line(SOCKET s, const std::string& line) {
    std::string l = line + "\n";
    return send_all(s, l.data(), l.size());
}

enum class LineResult { Ok, Timeout, Error };

// Reads one '\n'-terminated line. `acc` keeps partial data across timeouts.
// timeoutMs < 0 waits forever; otherwise it bounds each wait for more data.
inline LineResult read_line(SOCKET s, std::string& acc, std::string& line, int timeoutMs = -1) {
    for (;;) {
        size_t nl = acc.find('\n');
        if (nl != std::string::npos) {
            line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return LineResult::Ok;
        }
        if (acc.size() > 512) return LineResult::Error;
        if (timeoutMs >= 0) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            int r = select(0, &rd, nullptr, nullptr, &tv);
            if (r == 0) return LineResult::Timeout;
            if (r < 0) return LineResult::Error;
        }
        char c;
        if (recv(s, &c, 1, 0) != 1) return LineResult::Error;   // byte-wise: never over-read binary data
        acc += c;
    }
}

inline SOCKET tcp_connect(const std::string& host, uint16_t port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
        return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) {
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));
    }
    return s;
}

inline void set_recv_timeout(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
}

// ---- string helpers ----
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
inline std::string digits_only(const std::string& s) {
    std::string d;
    for (char c : s) if (c >= '0' && c <= '9') d += c;
    return d;
}
// "123456789" -> "123 456 789"
inline std::string format_id(const std::string& id) {
    std::string out;
    for (size_t i = 0; i < id.size(); ++i) { if (i && i % 3 == 0) out += ' '; out += id[i]; }
    return out;
}

// ---- clipboard (text only) ----
// Sequence number of the last change we made ourselves, so it isn't echoed back.
extern std::atomic<DWORD> g_clipIgnoreSeq;

inline bool clipboard_get(HWND owner, std::wstring& out) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            out.assign(p, wcsnlen(p, GlobalSize(h) / sizeof(wchar_t)));
            GlobalUnlock(h);
            ok = true;
        }
    }
    CloseClipboard();
    return ok;
}
inline void clipboard_set(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), text.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
    g_clipIgnoreSeq = GetClipboardSequenceNumber();
}
constexpr size_t MAX_CLIPBOARD_CHARS = 1 << 20;
