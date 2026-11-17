#pragma once
// Small UI toolkit shared by the RemoteDesk windows:
//   theme.cpp    light/dark palettes, title bar colours, DPI-aware fonts
//   draw.cpp     anti-aliased shapes (GDI+), text, flicker-free double buffering
//   controls.cpp custom-drawn buttons, toggles, segmented pickers, nav items, placeholders
//   art.cpp      the viewer's ring pointer and the remote-pointer halo
#include "core/common.h"
#include <utility>

// The Windows SDK's rpcndr.h does "#define small char", which breaks Fonts::small below.
#ifdef small
#undef small
#endif

namespace ui {

// ------------------------------------------------------------------ theme
enum class Mode { System = 0, Light = 1, Dark = 2 };

struct Palette {
    bool dark;
    COLORREF bg, sidebar, card, cardBorder, text, muted, faint, divider;
    COLORREF accent, accentHover, accentPressed, onAccent, accentSoft, accentText;
    COLORREF heroFrom, heroTo, onHero, onHeroMuted;
    COLORREF field, fieldBorder, fieldHover, focus;
    COLORREF btn, btnHover, btnPressed, btnBorder;
    COLORREF success, successSoft, warning, warningSoft, danger, dangerHover, dangerSoft;
    COLORREF viewerBg;
};

void init(HINSTANCE hi);     // once, before any window is created
void shutdown();
Palette pal();               // thread-safe copy
Mode mode();
void set_mode(Mode m);
bool refresh_system_theme(); // re-reads the Windows setting; true if the effective theme changed
void style_window(HWND hwnd);  // title bar follows the theme
void style_scroll(HWND ctl);   // themed scrollbars (multiline edits)

struct Fonts {
    HFONT body, bodyBold, small, smallBold, title, heading, brand, id, fieldBig;
    HFONT icon, iconSmall, iconLarge, iconHuge;
    int bodyH, fieldBigH;      // line heights in pixels
};
const Fonts& fonts(UINT dpi);  // cached per DPI, never freed
inline int px(int v, UINT dpi) { return MulDiv(v, (int)dpi, 96); }
inline RECT rc(int x, int y, int w, int h, UINT dpi) { return { px(x, dpi), px(y, dpi), px(x + w, dpi), px(y + h, dpi) }; }

// Segoe Fluent Icons / Segoe MDL2 Assets code points
namespace ico {
constexpr wchar_t Copy = 0xE8C8, Check = 0xE73E, Refresh = 0xE72C, Globe = 0xE774, Lock = 0xE72E,
                  People = 0xE716, Close = 0xE711, Chevron = 0xE70D, Remote = 0xE8AF, Monitor = 0xE7F4,
                  Sun = 0xE706, Moon = 0xE708, System = 0xE770, Power = 0xE7E8, Shield = 0xEA18,
                  Info = 0xE946, History = 0xE81C, Error = 0xE783, Arrow = 0xE72A, Home = 0xE80F,
                  Settings = 0xE713, Download = 0xE896, Palette = 0xE790, Key = 0xE192, Eye = 0xE890;
}

// ------------------------------------------------------------------ drawing
COLORREF mix(COLORREF a, COLORREF b, int t);   // t = 0..255 of b
void fill(HDC dc, const RECT& r, COLORREF c);
void round_rect(HDC dc, const RECT& r, int radius, COLORREF fillc, COLORREF border = CLR_INVALID, int borderPx = 1);
void gradient_rect(HDC dc, const RECT& r, int radius, COLORREF from, COLORREF to);   // left-top -> right-bottom
void circle(HDC dc, float cx, float cy, float r, COLORREF c);
void arc(HDC dc, float cx, float cy, float r, float startDeg, float sweepDeg, float width, COLORREF c);   // round caps
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c);
void text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c,
          UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
int text_width(HDC dc, const std::wstring& s, HFONT f);
void glyph(HDC dc, wchar_t g, RECT r, HFONT f, COLORREF c);   // centred icon

// Double-buffered WM_PAINT: draw on `dc` (clipped to the invalid area), copied on destruction.
struct Paint {
    explicit Paint(HWND h);
    ~Paint();
    HWND hwnd;
    PAINTSTRUCT ps;
    HDC wdc, dc;
    RECT client;
};

// ------------------------------------------------------------------ controls
// All send WM_COMMAND/BN_CLICKED to the parent.
enum class Kind { Primary, Secondary, Ghost, Danger, Icon, Link, Toggle, Segmented, Hero, HeroGhost, Nav };
enum class Surface { Window, Card, Field, Sidebar, Custom };
HWND make_control(HWND parent, int id, Kind kind, const std::wstring& text, wchar_t icon = 0,
                  Surface surface = Surface::Card);
void set_icon(HWND c, wchar_t icon);
void set_surface(HWND c, COLORREF color);   // for Surface::Custom (e.g. on the gradient card)
bool checked(HWND c);
void set_checked(HWND c, bool on);
void set_badge(HWND c, bool on);            // small dot on a nav item
void set_kind(HWND c, Kind kind);           // change the look (e.g. Secondary -> Danger while recording)
// Busy: a spinner replaces the icon and clicks are ignored (the button keeps its colour).
// Progress 0..100 also fills the button from the left; -1 = none. Both animate smoothly.
void set_busy(HWND c, bool busy, bool cancelable = false);   // cancelable: still clickable while spinning
void set_progress(HWND c, int percent);
void set_options(HWND seg, const std::vector<std::pair<wchar_t, std::wstring>>& options);
int selected(HWND seg);
void set_selected(HWND seg, int i);

// Single-line EDIT: draws `text` in a faint colour while empty.
void placeholder(HWND edit, const std::wstring& text);

// ------------------------------------------------------------------ pointer art
HCURSOR viewer_cursor(UINT dpi);   // small ring: the local pointer over a remote screen
void draw_pointer_halo(uint8_t* bgra, int w, int h, int cx, int cy);   // around the remote pointer

}  // namespace ui
