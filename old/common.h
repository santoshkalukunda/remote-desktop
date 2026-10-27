#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

constexpr uint16_t DEFAULT_PORT = 57550;
constexpr int TILE = 64;                 // tile size for change detection

enum MsgType : uint8_t {
    MSG_SCREEN_INFO = 1,
    MSG_TILE        = 2,
    MSG_FRAME_END   = 3,
    MSG_MOUSE_MOVE  = 4,
    MSG_MOUSE_BTN   = 5,
    MSG_MOUSE_WHEEL = 6,
    MSG_KEY         = 7,
};

#pragma pack(push, 1)
struct MsgHeader  { uint8_t type; uint32_t len; };
struct ScreenInfo { int32_t w; int32_t h; };
struct TileHeader { int32_t x, y, w, h; };      // followed by w*h*3 bytes BGR
struct MouseMove  { int32_t x, y; };
struct MouseBtn   { uint8_t button; uint8_t down; };   // 0=L 1=R 2=M
struct MouseWheel { int32_t delta; };
struct KeyEvent   { uint16_t vk; uint8_t down; };
#pragma pack(pop)

// ---- socket helpers (handle partial send/recv) ----
inline bool send_all(SOCKET s, const void* b, int len) {
    const char* p = (const char*)b;
    while (len > 0) { int n = send(s, p, len, 0); if (n <= 0) return false; p += n; len -= n; }
    return true;
}
inline bool recv_all(SOCKET s, void* b, int len) {
    char* p = (char*)b;
    while (len > 0) { int n = recv(s, p, len, 0); if (n <= 0) return false; p += n; len -= n; }
    return true;
}
inline bool send_msg(SOCKET s, uint8_t type, const void* payload, uint32_t len) {
    MsgHeader h{ type, len };
    if (!send_all(s, &h, sizeof(h))) return false;
    return len == 0 ? true : send_all(s, payload, (int)len);
}

// ---- auto ID: pack IPv4(32) + port(16) into base36 ----
inline std::string encode_id(uint32_t ip_be, uint16_t port) {
    uint64_t v = ((uint64_t)ntohl(ip_be) << 16) | port;
    const char* A = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string s;
    if (!v) s = "0";
    while (v) { s += A[v % 36]; v /= 36; }
    std::reverse(s.begin(), s.end());
    while (s.size() < 9) s = "0" + s;         // pad for a stable look
    std::string out;                           // group as XXX-XXX-XXX
    for (size_t i = 0; i < s.size(); ++i) { if (i && i % 3 == 0) out += '-'; out += s[i]; }
    return out;
}
inline bool decode_id(const std::string& in, uint32_t& ip_be, uint16_t& port) {
    uint64_t v = 0;
    for (char c : in) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else continue;                          // skip dashes/spaces
        v = v * 36 + d;
    }
    port = (uint16_t)(v & 0xFFFF);
    ip_be = htonl((uint32_t)(v >> 16));
    return port != 0;
}

// ---- find primary outbound IPv4 without sending anything ----
inline uint32_t local_ipv4() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    connect(s, (sockaddr*)&a, sizeof(a));       // UDP connect: no packet leaves
    sockaddr_in loc{}; int l = sizeof(loc);
    getsockname(s, (sockaddr*)&loc, &l);
    closesocket(s);
    return loc.sin_addr.s_addr;
}