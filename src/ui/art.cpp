// Pointer art: the viewer's local ring pointer, and the soft halo the host draws
// around its own pointer so the viewer can always spot the remote one.
#include "ui/ui.h"
#include <cmath>
#include <map>

namespace ui {

// Coverage of a disc of radius r at distance d from its centre, with a 1px soft edge.
static float disc(float d, float r) { return std::clamp(r - d + 0.5f, 0.0f, 1.0f); }

struct Rgba { float r, g, b, a; };
static Rgba over(Rgba top, Rgba under) {   // straight alpha
    float a = top.a + under.a * (1 - top.a);
    if (a <= 0) return { 0, 0, 0, 0 };
    return { (top.r * top.a + under.r * under.a * (1 - top.a)) / a, (top.g * top.a + under.g * under.a * (1 - top.a)) / a,
             (top.b * top.a + under.b * under.a * (1 - top.a)) / a, a };
}

HCURSOR viewer_cursor(UINT dpi) {
    static std::mutex mx;
    static std::map<UINT, HCURSOR> cache;
    std::lock_guard<std::mutex> lk(mx);
    if (HCURSOR c = cache[dpi]) return c;

    const int size = std::max(16, px(24, dpi));
    const float s = dpi / 96.0f, cx = size / 2.0f, cy = size / 2.0f;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    uint32_t* px32 = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)&px32, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return LoadCursorW(nullptr, IDC_CROSS);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            Rgba p{ 0, 0, 0, 0 };
            p = over({ 0, 0, 0, 0.45f * disc(d, 7.5f * s) }, p);              // soft dark outline
            p = over({ 1, 1, 1, disc(d, 6.5f * s) - disc(d, 4.5f * s) }, p);  // white ring
            p = over({ 0.06f, 0.73f, 0.51f, disc(d, 4.5f * s) }, p);           // emerald core
            p = over({ 1, 1, 1, disc(d, 1.3f * s) }, p);                       // hotspot dot
            const uint32_t a = (uint32_t)(p.a * 255 + 0.5f);
            px32[y * size + x] = (a << 24) | ((uint32_t)(p.r * 255 + 0.5f) << 16) |
                                 ((uint32_t)(p.g * 255 + 0.5f) << 8) | (uint32_t)(p.b * 255 + 0.5f);
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{ FALSE, (DWORD)(size / 2), (DWORD)(size / 2), mask, color };
    HCURSOR cur = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    if (!cur) cur = LoadCursorW(nullptr, IDC_CROSS);
    cache[dpi] = cur;
    return cur;
}

void draw_pointer_halo(uint8_t* bgra, int w, int h, int cx, int cy) {
    const float R = 17.0f;   // ring radius
    const int ext = (int)R + 3;
    for (int dy = -ext; dy <= ext; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= h) continue;
        for (int dx = -ext; dx <= ext; ++dx) {
            const int x = cx + dx;
            if (x < 0 || x >= w) continue;
            const float d = std::hypot((float)dx, (float)dy);
            // faint tinted fill + crisp 2.5px ring with a white outer edge, all anti-aliased
            const float fillA = 0.16f * disc(d, R - 1.5f);
            const float ringA = 0.9f * (disc(d, R + 1.25f) - disc(d, R - 1.25f));
            const float haloA = 0.35f * (disc(d, R + 2.5f) - disc(d, R + 1.25f));
            uint8_t* p = bgra + ((size_t)y * w + x) * 4;
            auto blend = [p](float a, int r, int g, int b) {
                if (a <= 0) return;
                p[0] = (uint8_t)(p[0] + (b - p[0]) * a);
                p[1] = (uint8_t)(p[1] + (g - p[1]) * a);
                p[2] = (uint8_t)(p[2] + (r - p[2]) * a);
            };
            blend(fillA, 0x10, 0xB9, 0x81);
            blend(haloA, 0xFF, 0xFF, 0xFF);
            blend(ringA, 0x05, 0x96, 0x69);
        }
    }
}

}  // namespace ui
