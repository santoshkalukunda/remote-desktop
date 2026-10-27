#include "common.h"
#include <vector>
#include <thread>
#include <cstdio>
#include <chrono>

static int gW, gH;

// Grab the whole screen (with cursor drawn in) into a top-down BGRA buffer.
static void capture(std::vector<uint8_t>& buf) {
    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateCompatibleBitmap(scr, gW, gH);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, gW, gH, scr, 0, 0, SRCCOPY);

    CURSORINFO ci{ sizeof(ci) };
    if (GetCursorInfo(&ci) && ci.flags == CURSOR_SHOWING) {
        ICONINFO ii{};
        if (GetIconInfo(ci.hCursor, &ii)) {
            DrawIconEx(mem, ci.ptScreenPos.x - ii.xHotspot,
                            ci.ptScreenPos.y - ii.yHotspot,
                       ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
            if (ii.hbmMask)  DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = gW; bi.bmiHeader.biHeight = -gH;   // top-down
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    buf.resize((size_t)gW * gH * 4);
    GetDIBits(mem, bmp, 0, gH, buf.data(), &bi, DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, scr);
}

// Send only tiles that changed since the previous frame (BGR, alpha dropped).
static bool send_frame(SOCKET s, const std::vector<uint8_t>& cur,
                       std::vector<uint8_t>& prev) {
    std::vector<uint8_t> tile;
    for (int ty = 0; ty < gH; ty += TILE) {
        for (int tx = 0; tx < gW; tx += TILE) {
            int tw = std::min(TILE, gW - tx);
            int th = std::min(TILE, gH - ty);
            bool changed = prev.empty();
            for (int r = 0; r < th && !changed; ++r) {
                const uint8_t* a = &cur [((size_t)(ty + r) * gW + tx) * 4];
                const uint8_t* b = &prev[((size_t)(ty + r) * gW + tx) * 4];
                if (memcmp(a, b, (size_t)tw * 4)) changed = true;
            }
            if (!changed) continue;

            tile.resize((size_t)tw * th * 3);
            uint8_t* o = tile.data();
            for (int r = 0; r < th; ++r) {
                const uint8_t* a = &cur[((size_t)(ty + r) * gW + tx) * 4];
                for (int c = 0; c < tw; ++c) { *o++ = a[0]; *o++ = a[1]; *o++ = a[2]; a += 4; }
            }
            TileHeader h{ tx, ty, tw, th };
            std::vector<uint8_t> pkt(sizeof(h) + tile.size());
            memcpy(pkt.data(), &h, sizeof(h));
            memcpy(pkt.data() + sizeof(h), tile.data(), tile.size());
            if (!send_msg(s, MSG_TILE, pkt.data(), (uint32_t)pkt.size())) return false;
        }
    }
    prev = cur;
    return send_msg(s, MSG_FRAME_END, nullptr, 0);
}

// Inject remote input locally.
static void inject(const MsgHeader& h, SOCKET s) {
    INPUT in{};
    if (h.type == MSG_MOUSE_MOVE) {
        MouseMove m; if (!recv_all(s, &m, sizeof(m))) return;
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        in.mi.dx = (LONG)((int64_t)m.x * 65535 / (gW > 1 ? gW - 1 : 1));
        in.mi.dy = (LONG)((int64_t)m.y * 65535 / (gH > 1 ? gH - 1 : 1));
        SendInput(1, &in, sizeof(in));
    } else if (h.type == MSG_MOUSE_BTN) {
        MouseBtn b; if (!recv_all(s, &b, sizeof(b))) return;
        in.type = INPUT_MOUSE;
        DWORD f = 0;
        if (b.button == 0) f = b.down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;
        if (b.button == 1) f = b.down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;
        if (b.button == 2) f = b.down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        in.mi.dwFlags = f;
        SendInput(1, &in, sizeof(in));
    } else if (h.type == MSG_MOUSE_WHEEL) {
        MouseWheel w; if (!recv_all(s, &w, sizeof(w))) return;
        in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = w.delta;
        SendInput(1, &in, sizeof(in));
    } else if (h.type == MSG_KEY) {
        KeyEvent k; if (!recv_all(s, &k, sizeof(k))) return;
        in.type = INPUT_KEYBOARD; in.ki.wVk = k.vk;
        in.ki.dwFlags = k.down ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(in));
    } else if (h.len) {
        std::vector<char> junk(h.len); recv_all(s, junk.data(), h.len); // skip unknown
    }
}

static void input_thread(SOCKET s) {
    MsgHeader h;
    while (recv_all(s, &h, sizeof(h))) inject(h, s);
}

int main() {
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SetProcessDPIAware();
    gW = GetSystemMetrics(SM_CXSCREEN);
    gH = GetSystemMetrics(SM_CYSCREEN);

    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    BOOL yes = TRUE; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(DEFAULT_PORT);
    bind(ls, (sockaddr*)&a, sizeof(a));
    listen(ls, 1);

    printf("=== Remote Host ===\nYour ID:  %s\nWaiting for a viewer on port %u...\n",
           encode_id(local_ipv4(), DEFAULT_PORT).c_str(), DEFAULT_PORT);

    for (;;) {
        SOCKET cs = accept(ls, nullptr, nullptr);
        if (cs == INVALID_SOCKET) continue;
        int one = 1; setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
        printf("Viewer connected.\n");

        ScreenInfo si{ gW, gH };
        send_msg(cs, MSG_SCREEN_INFO, &si, sizeof(si));

        std::thread it(input_thread, cs);
        std::vector<uint8_t> cur, prev;
        while (true) {
            capture(cur);
            if (!send_frame(cs, cur, prev)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(40)); // ~25 fps
        }
        closesocket(cs);
        it.join();
        prev.clear();
        printf("Viewer disconnected. Waiting again...\n");
    }
}