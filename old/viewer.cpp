#include "common.h"
#include <vector>
#include <thread>
#include <mutex>
#include <cstdio>

static int rW = 1, rH = 1;              // remote screen size
static HWND g_hwnd;
static HBITMAP g_dib;
static uint8_t* g_bits = nullptr;       // top-down BGRA backbuffer
static std::mutex g_mx;
static SOCKET g_sock;

static void make_backbuffer(HWND hwnd) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; g_bits = nullptr; }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = rW; bi.bmiHeader.biHeight = -rH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(hwnd);
    g_dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&g_bits, NULL, 0);
    ReleaseDC(hwnd, dc);
}

static void send_mouse_move(HWND hwnd, int mx, int my) {
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right, ch = rc.bottom; if (cw <= 0 || ch <= 0) return;
    MouseMove m{ (int32_t)((int64_t)mx * rW / cw), (int32_t)((int64_t)my * rH / ch) };
    send_msg(g_sock, MSG_MOUSE_MOVE, &m, sizeof(m));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_USER + 1:                    // resize request from recv thread
        make_backbuffer(hwnd);
        SetWindowPos(hwnd, 0, 0, 0, rW, rH, SWP_NOMOVE | SWP_NOZORDER);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_dib) {
            HDC md = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(md, g_dib);
            RECT rc; GetClientRect(hwnd, &rc);
            SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, 0, 0, rc.right, rc.bottom, md, 0, 0, rW, rH, SRCCOPY);
            SelectObject(md, old); DeleteDC(md);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: send_mouse_move(hwnd, LOWORD(lp), HIWORD(lp)); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: {
        MouseBtn b{ 0, (uint8_t)(msg == WM_LBUTTONDOWN) };
        send_msg(g_sock, MSG_MOUSE_BTN, &b, sizeof(b)); return 0; }
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        MouseBtn b{ 1, (uint8_t)(msg == WM_RBUTTONDOWN) };
        send_msg(g_sock, MSG_MOUSE_BTN, &b, sizeof(b)); return 0; }
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        MouseBtn b{ 2, (uint8_t)(msg == WM_MBUTTONDOWN) };
        send_msg(g_sock, MSG_MOUSE_BTN, &b, sizeof(b)); return 0; }
    case WM_MOUSEWHEEL: {
        MouseWheel w{ GET_WHEEL_DELTA_WPARAM(wp) };
        send_msg(g_sock, MSG_MOUSE_WHEEL, &w, sizeof(w)); return 0; }
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        KeyEvent k{ (uint16_t)wp, (uint8_t)(msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) };
        send_msg(g_sock, MSG_KEY, &k, sizeof(k)); return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void recv_thread() {
    MsgHeader h;
    std::vector<uint8_t> payload;
    while (recv_all(g_sock, &h, sizeof(h))) {
        if (h.type == MSG_SCREEN_INFO) {
            ScreenInfo si; recv_all(g_sock, &si, sizeof(si));
            rW = si.w; rH = si.h;
            PostMessage(g_hwnd, WM_USER + 1, 0, 0);
        } else if (h.type == MSG_TILE) {
            TileHeader t; recv_all(g_sock, &t, sizeof(t));
            payload.resize((size_t)t.w * t.h * 3);
            recv_all(g_sock, payload.data(), (int)payload.size());
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_bits) {
                const uint8_t* in = payload.data();
                for (int r = 0; r < t.h; ++r) {
                    uint8_t* o = g_bits + ((size_t)(t.y + r) * rW + t.x) * 4;
                    for (int c = 0; c < t.w; ++c) { o[0]=in[0]; o[1]=in[1]; o[2]=in[2]; o[3]=255; o+=4; in+=3; }
                }
            }
        } else if (h.type == MSG_FRAME_END) {
            InvalidateRect(g_hwnd, nullptr, FALSE);
        } else if (h.len) {
            payload.resize(h.len); recv_all(g_sock, payload.data(), h.len);
        }
    }
    PostMessage(g_hwnd, WM_CLOSE, 0, 0);
}

int main(int argc, char** argv) {
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SetProcessDPIAware();

    std::string id;
    if (argc > 1) id = argv[1];
    else { printf("Enter host ID: "); char b[64]; scanf("%63s", b); id = b; }

    uint32_t ip_be; uint16_t port;
    if (!decode_id(id, ip_be, port)) { printf("Bad ID\n"); return 1; }

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = ip_be; a.sin_port = htons(port);
    if (connect(g_sock, (sockaddr*)&a, sizeof(a)) != 0) { printf("Connect failed\n"); return 1; }
    int one = 1; setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));

    WNDCLASS wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "RDViewer"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
    g_hwnd = CreateWindow("RDViewer", "Remote Viewer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        1024, 640, NULL, NULL, wc.hInstance, NULL);

    std::thread rt(recv_thread);
    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    closesocket(g_sock);
    rt.join();
    return 0;
}