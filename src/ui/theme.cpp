// Palettes, the light/dark switch, title bar styling and the font cache.
#include "ui/ui.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <map>

#ifdef _MSC_VER
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace ui {

static std::mutex g_mx;
static Mode g_mode = Mode::System;
static bool g_dark = false;
std::wstring g_textFace = L"Segoe UI", g_displayFace = L"Segoe UI", g_iconFace = L"Segoe MDL2 Assets";

#define C(hex) RGB(((hex) >> 16) & 0xFF, ((hex) >> 8) & 0xFF, (hex) & 0xFF)
static const Palette LIGHT = {
    false,
    C(0xF3F6F5), C(0xFFFFFF), C(0xFFFFFF), C(0xE3E9E6), C(0x0F1F1A), C(0x56655F), C(0x93A19B), C(0xECF1EF),
    C(0x059669), C(0x047857), C(0x065F46), C(0xFFFFFF), C(0xE3F7EE), C(0x047857),
    C(0x059669), C(0x0E9F9B), C(0xFFFFFF), C(0xD3F5E8),
    C(0xF7F9F8), C(0xD9E0DD), C(0xBFC9C5), C(0x10B981),
    C(0xFFFFFF), C(0xF1F5F3), C(0xE4EBE8), C(0xD9E0DD),
    C(0x059669), C(0xD1FAE5), C(0xD97706), C(0xFEF3C7), C(0xDC2626), C(0xB91C1C), C(0xFEE2E2),
    C(0x111318),
};
static const Palette DARK = {
    true,
    C(0x0C1211), C(0x111917), C(0x16201D), C(0x22302B), C(0xEEF4F1), C(0xA0B0A9), C(0x6B7B74), C(0x1E2A26),
    C(0x10B981), C(0x34D399), C(0x059669), C(0xFFFFFF), C(0x0F2E25), C(0x6EE7B7),
    C(0x047857), C(0x0E7490), C(0xFFFFFF), C(0xC6F0E0),
    C(0x101715), C(0x2A3934), C(0x3A4C46), C(0x34D399),
    C(0x1C2723), C(0x23312C), C(0x2C3C36), C(0x2F403A),
    C(0x34D399), C(0x0F2E24), C(0xFBBF24), C(0x33280C), C(0xF87171), C(0xFCA5A5), C(0x3A1A1F),
    C(0x0B0C10),
};
#undef C

static bool os_dark() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

// Popup menus follow the theme (uxtheme ordinals 135/136, Windows 10 1903+; ignored elsewhere).
static void theme_menus(bool dark) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    static auto set = (SetMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    static auto flush = (Flush)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set) set(dark ? 2 : 3);   // ForceDark / ForceLight
    if (flush) flush();
}

static bool update_dark() {   // g_mx held
    bool d = g_mode == Mode::Dark || (g_mode == Mode::System && os_dark());
    bool changed = d != g_dark;
    g_dark = d;
    return changed;
}

Palette pal() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_dark ? DARK : LIGHT;
}
Mode mode() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_mode;
}
void set_mode(Mode m) {
    bool dark;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_mode = m;
        update_dark();
        dark = g_dark;
    }
    theme_menus(dark);
}
bool refresh_system_theme() {
    bool changed, dark;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        changed = update_dark();
        dark = g_dark;
    }
    if (changed) theme_menus(dark);
    return changed;
}

void style_window(HWND h) {
    Palette P = pal();
    BOOL dark = P.dark;
    if (FAILED(DwmSetWindowAttribute(h, 20, &dark, sizeof(dark))))   // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(h, 19, &dark, sizeof(dark));            // pre-20H1 value
    COLORREF caption = P.sidebar, captionText = P.text, border = P.cardBorder;
    DwmSetWindowAttribute(h, 35, &caption, sizeof(caption));          // Windows 11 only
    DwmSetWindowAttribute(h, 36, &captionText, sizeof(captionText));
    DwmSetWindowAttribute(h, 34, &border, sizeof(border));
}

void style_scroll(HWND c) {
    SetWindowTheme(c, pal().dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

// ------------------------------------------------------------------ fonts
static HFONT make_font(const std::wstring& face, int pxh, int weight) {
    return CreateFontW(-pxh, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face.c_str());
}

static int line_height(HFONT f) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ o = SelectObject(dc, f);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(nullptr, dc);
    return tm.tmHeight;
}

const Fonts& fonts(UINT dpi) {
    static std::mutex mx;
    static std::map<UINT, Fonts> cache;
    std::lock_guard<std::mutex> lk(mx);
    auto it = cache.find(dpi);
    if (it != cache.end()) return it->second;
    auto P = [dpi](int v) { return px(v, dpi); };
    Fonts f;
    f.body = make_font(g_textFace, P(14), FW_NORMAL);
    f.bodyBold = make_font(g_textFace, P(14), FW_SEMIBOLD);
    f.small = make_font(g_textFace, P(12), FW_NORMAL);
    f.smallBold = make_font(g_textFace, P(12), FW_SEMIBOLD);
    f.title = make_font(g_textFace, P(16), FW_SEMIBOLD);
    f.heading = make_font(g_displayFace, P(24), FW_SEMIBOLD);
    f.brand = make_font(g_displayFace, P(18), FW_SEMIBOLD);
    f.id = make_font(g_displayFace, P(44), FW_SEMIBOLD);
    f.fieldBig = make_font(g_displayFace, P(20), FW_NORMAL);
    f.icon = make_font(g_iconFace, P(16), FW_NORMAL);
    f.iconSmall = make_font(g_iconFace, P(12), FW_NORMAL);
    f.iconLarge = make_font(g_iconFace, P(20), FW_NORMAL);
    f.iconHuge = make_font(g_iconFace, P(34), FW_NORMAL);
    f.bodyH = line_height(f.body);
    f.fieldBigH = line_height(f.fieldBig);
    return cache.emplace(dpi, f).first->second;
}

}  // namespace ui
