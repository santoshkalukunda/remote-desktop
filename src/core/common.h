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
#include <cstdlib>
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
    // File transfer (both ways; needs PERM_FILES). Every payload starts with the sender's u32 transfer id.
    MSG_FILE_OFFER   = 16,  // id, u64 size, utf8 file name
    MSG_FILE_ACCEPT  = 17,  // id                       (receiver -> sender)
    MSG_FILE_REJECT  = 18,  // id, utf8 reason          (receiver -> sender)
    MSG_FILE_DATA    = 19,  // id, bytes (up to FILE_CHUNK)
    MSG_FILE_END     = 20,  // id, 32-byte SHA-256 of the whole file
    MSG_FILE_CANCEL  = 21,  // id                       (either side; the id is the sender's)
    MSG_RECORDING    = 22,  // viewer->host: u8 1 = started recording, 0 = stopped (shown to the host)
    MSG_PERMS        = 23,  // host->viewer: u8 AccessPerm bits; the host changed what the viewer may do
    MSG_FILE_OFFER2  = 25,  // id, u64 size, 16-byte transfer token, utf8 name: resumable (peer sent CAP_FILE_RESUME)
                            // its MSG_FILE_ACCEPT carries id + u64 offset: bytes the receiver already has
    MSG_CAPS         = 26,  // either way, at session start: u32 capability bits (below)
    MSG_HOST_STATE   = 29,  // host->viewer: u8, 1 = its screen is locked (or a UAC prompt) and, without the
                            // service, can't be shown or used; 0 = back on the desktop
    MSG_SAS          = 28,  // viewer->host: Ctrl+Alt+Del (the viewer presses Ctrl+Alt+End; needs the service)
    MSG_PING         = 27,  // either way every 5 s. Once a side has seen one, 20 s of silence ends the session
                            // (a dropped network or a sleeping PC would otherwise leave a stale session behind)
    MSG_PRO          = 24,  // either way, at session start: this side's signed Pro license (base64 '\n' sig hex)
    // Sound and calls (remote/audio.h). Audio travels as IMA ADPCM, 20 ms per message.
    MSG_AUDIO        = 30,  // host->viewer: the host PC's sound (what its speakers play), 32 kHz stereo
    MSG_AUDIO_CTL    = 31,  // viewer->host: u8 1 = send me your sound, 0 = stop (needs PERM_AUDIO)
    MSG_CALL         = 32,  // either way: u8 CallAction (ring, accept, decline, hang up)
    MSG_VOICE        = 33,  // either way during a call: microphone, 16 kHz mono
    MSG_FILE_PAUSE   = 34,  // either way: u32 id, u8 whose (0 = the file you are receiving from me,
                            // 1 = the file you are sending me). Stops it; the receiver keeps what arrived.
    MSG_FILE_RESUME  = 35,  // receiver -> sender: u32 id: offer that paused file again (it continues)
};
constexpr int MAX_MONITORS = 16;
constexpr uint32_t FILE_CHUNK = 64 * 1024;
constexpr uint32_t CAP_FILE_RESUME = 1;   // understands MSG_FILE_OFFER2 (interrupted transfers continue)
constexpr uint32_t CAP_AUDIO = 2;         // understands MSG_AUDIO*, MSG_CALL, MSG_VOICE (1.5+)
constexpr uint32_t CAP_FILE_PAUSE = 4;    // understands MSG_FILE_PAUSE / MSG_FILE_RESUME (1.5.1+)
constexpr DWORD PING_EVERY_MS = 5000, PING_TIMEOUT_MS = 20000;
enum CallAction : uint8_t { CALL_RING = 1, CALL_ACCEPT = 2, CALL_DECLINE = 3, CALL_HANGUP = 4 };

// What the host allows, sent as the 1-byte payload of MSG_ACCEPT (viewing is always allowed).
// The host enforces CONTROL and CLIPBOARD itself; RECORD is honoured by the viewer app.
// An empty MSG_ACCEPT payload (hosts older than 1.3) means control + clipboard.
enum AccessPerm : uint8_t {
    PERM_CONTROL   = 1,   // mouse and keyboard
    PERM_CLIPBOARD = 2,   // clipboard sync
    PERM_FILES     = 4,   // file transfer (release 2)
    PERM_RECORD    = 8,   // screenshots and screen recording
    PERM_AUDIO     = 16,  // hear this computer's sound (calls are agreed separately, per call)
    PERM_ALL       = PERM_CONTROL | PERM_CLIPBOARD | PERM_FILES | PERM_RECORD | PERM_AUDIO,
    PERM_LEGACY    = PERM_CONTROL | PERM_CLIPBOARD,
};

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

// Connects with a bounded wait (the Windows default is ~21 s per address), so an
// unreachable relay never ties a thread up for long.
inline SOCKET tcp_connect(const std::string& host, uint16_t port, int timeoutMs = 6000) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
        return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        bool ok = connect(s, a->ai_addr, (int)a->ai_addrlen) == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wr, ex;
            FD_ZERO(&wr); FD_ZERO(&ex);
            FD_SET(s, &wr); FD_SET(s, &ex);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            ok = select(0, nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr) && !FD_ISSET(s, &ex);
        }
        if (ok) {
            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
            break;
        }
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
inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// ---- relay address: host (domain / IPv4 / IPv6) and TCP port, kept separately ----
inline bool parse_port(const std::string& s, uint16_t& port) {
    std::string t = trim(s);
    if (t.empty() || t.size() > 5 || digits_only(t) != t) return false;
    int v = atoi(t.c_str());
    if (v < 1 || v > 65535) return false;
    port = (uint16_t)v;
    return true;
}
// Accepts "host", "host:port", "[v6]:port" or a pasted URL; leaves `port` alone if none is given.
inline void split_relay(std::string s, std::string& host, uint16_t& port) {
    s = trim(s);
    size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    if (!s.empty() && s[0] == '[') {
        size_t e = s.find(']');
        if (e != std::string::npos) {
            host = s.substr(1, e - 1);
            if (e + 1 < s.size() && s[e + 1] == ':') parse_port(s.substr(e + 2), port);
            return;
        }
    }
    size_t c = s.find(':');
    if (c != std::string::npos && s.find(':', c + 1) == std::string::npos) {
        host = s.substr(0, c);
        parse_port(s.substr(c + 1), port);
    } else {
        host = s;
    }
}
inline bool valid_host(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    for (char c : h)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_' || c == ':' || c == '%'))
            return false;
    return true;
}
// "203.0.113.5:21116", "[::1]:21116"
inline std::string join_relay(const std::string& host, uint16_t port) {
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
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
