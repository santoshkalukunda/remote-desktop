// Drawing primitives: anti-aliased shapes through GDI+, text through GDI, and a
// reusable back buffer so painting never flickers or allocates per frame.
#include "ui/ui.h"
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#endif

namespace ui {

extern std::wstring g_textFace, g_displayFace, g_iconFace;   // theme.cpp
void register_controls(HINSTANCE hi);                        // controls.cpp
static ULONG_PTR g_gdipToken = 0;

COLORREF mix(COLORREF a, COLORREF b, int t) {
    auto m = [t](int x, int y) { return (x * (255 - t) + y * t) / 255; };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

static Gdiplus::Color gp(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

static void round_path(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = std::min(r * 2, std::min(w, h));
    if (d <= 0.5f) { p.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static void setup(Gdiplus::Graphics& g) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

void fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void round_rect(HDC dc, const RECT& r, int radius, COLORREF fillc, COLORREF border, int bw) {
    if (!RectVisible(dc, &r)) return;   // outside the area being repainted
    Gdiplus::Graphics g(dc);
    setup(g);
    const float x = (float)r.left, y = (float)r.top, w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    if (fillc != CLR_INVALID) {
        Gdiplus::GraphicsPath p;
        round_path(p, x, y, w, h, (float)radius);
        Gdiplus::SolidBrush b(gp(fillc));
        g.FillPath(&b, &p);
    }
    if (border != CLR_INVALID && bw > 0) {
        const float i = bw / 2.0f;
        Gdiplus::GraphicsPath p;
        round_path(p, x + i, y + i, w - bw, h - bw, std::max(0.0f, radius - i));
        Gdiplus::Pen pen(gp(border), (float)bw);
        g.DrawPath(&pen, &p);
    }
}

void gradient_rect(HDC dc, const RECT& r, int radius, COLORREF from, COLORREF to) {
    if (!RectVisible(dc, &r)) return;
    Gdiplus::Graphics g(dc);
    setup(g);
    Gdiplus::GraphicsPath p;
    round_path(p, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (float)radius);
    Gdiplus::LinearGradientBrush b(Gdiplus::PointF((float)r.left, (float)r.top), Gdiplus::PointF((float)r.right, (float)r.bottom),
                                   gp(from), gp(to));
    g.FillPath(&b, &p);
}

void circle(HDC dc, float cx, float cy, float r, COLORREF c) {
    RECT bounds{ (int)(cx - r) - 1, (int)(cy - r) - 1, (int)(cx + r) + 2, (int)(cy + r) + 2 };
    if (!RectVisible(dc, &bounds)) return;
    Gdiplus::Graphics g(dc);
    setup(g);
    Gdiplus::SolidBrush b(gp(c));
    g.FillEllipse(&b, cx - r, cy - r, r * 2, r * 2);
}

void arc(HDC dc, float cx, float cy, float r, float startDeg, float sweepDeg, float width, COLORREF c) {
    RECT bounds{ (int)(cx - r - width) - 1, (int)(cy - r - width) - 1, (int)(cx + r + width) + 2, (int)(cy + r + width) + 2 };
    if (!RectVisible(dc, &bounds)) return;
    Gdiplus::Graphics g(dc);
    setup(g);
    Gdiplus::Pen pen(gp(c), width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawArc(&pen, cx - r, cy - r, r * 2, r * 2, startDeg, sweepDeg);
}

void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c) {
    if (y1 == y2) fill(dc, { x1, y1, x2, y1 + 1 }, c);
    else if (x1 == x2) fill(dc, { x1, y1, x1 + 1, y2 }, c);
}

void text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ o = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt);
    SelectObject(dc, o);
}

int text_width(HDC dc, const std::wstring& s, HFONT f) {
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

void glyph(HDC dc, wchar_t g, RECT r, HFONT f, COLORREF c) {
    text(dc, std::wstring(1, g), r, f, c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// One back buffer per UI thread, grown as needed and reused by every paint.
struct BackBuffer {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    ~BackBuffer() {
        if (dc) { SelectObject(dc, old); DeleteDC(dc); }
        if (bmp) DeleteObject(bmp);
    }
};
static thread_local BackBuffer t_back;

Paint::Paint(HWND h) : hwnd(h) {
    wdc = BeginPaint(h, &ps);
    GetClientRect(h, &client);
    BackBuffer& b = t_back;
    const int w = std::max(1L, client.right), hh = std::max(1L, client.bottom);
    if (!b.dc || w > b.w || hh > b.h) {
        if (b.dc) { SelectObject(b.dc, b.old); DeleteDC(b.dc); }
        if (b.bmp) DeleteObject(b.bmp);
        b.w = std::max(w, b.w);
        b.h = std::max(hh, b.h);
        b.dc = CreateCompatibleDC(wdc);
        b.bmp = CreateCompatibleBitmap(wdc, b.w, b.h);
        b.old = SelectObject(b.dc, b.bmp);
    }
    dc = b.dc;
    IntersectClipRect(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
}
Paint::~Paint() {
    const RECT& r = ps.rcPaint;
    BitBlt(wdc, r.left, r.top, r.right - r.left, r.bottom - r.top, dc, r.left, r.top, SRCCOPY);
    SelectClipRgn(dc, nullptr);
    EndPaint(hwnd, &ps);
}

// ------------------------------------------------------------------ setup
static int CALLBACK font_found(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *(bool*)lp = true;
    return 0;
}
static bool font_exists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, font_found, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

void init(HINSTANCE hi) {
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdipToken, &in, nullptr);
    if (font_exists(L"Segoe UI Variable Text")) g_textFace = L"Segoe UI Variable Text";
    if (font_exists(L"Segoe UI Variable Display")) g_displayFace = L"Segoe UI Variable Display";
    if (font_exists(L"Segoe Fluent Icons")) g_iconFace = L"Segoe Fluent Icons";
    register_controls(hi);
}

void shutdown() {
    if (g_gdipToken) Gdiplus::GdiplusShutdown(g_gdipToken);
    g_gdipToken = 0;
}

}  // namespace ui
