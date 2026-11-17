// Custom-drawn controls (one window class, several looks) and EDIT placeholders.
//
// Every visual state change is eased: hover/press colours fade, toggles slide, the
// segmented picker's highlight glides, progress fills grow and busy buttons spin.
// A control runs a ~60 fps timer only while something is moving.
#include "ui/ui.h"
#include <windowsx.h>
#include <cmath>

namespace ui {

static const wchar_t* CTL_CLASS = L"RemoteDeskControl";
constexpr UINT_PTR ANIM_TIMER = 1;
constexpr UINT ANIM_MS = 15;
constexpr float EASE_MS = 70.0f;   // time constant of the exponential ease
constexpr float SPIN_DEG_PER_MS = 0.40f;

struct Ctl {
    Kind kind;
    Surface surface;
    wchar_t icon = 0;
    COLORREF custom = 0;
    bool hover = false, pressed = false, checked = false, badge = false, busy = false;
    bool cancelable = false;   // busy, but still clickable (it reads "Cancel" meanwhile)
    bool focused = false, kbFocus = true, mouseDown = false;
    std::vector<std::pair<wchar_t, std::wstring>> options;
    int sel = 0, hoverSeg = -1;
    int progress = -1;   // target percent, -1 = none

    // animated values (move toward the targets above)
    float hoverT = 0, pressT = 0, checkT = 0, segPos = 0, shownPct = 0, spin = 0;
    bool animating = false, primed = false;
    ULONGLONG lastTick = 0;
};

static Ctl* ctl_of(HWND h) { return (Ctl*)GetWindowLongPtrW(h, GWLP_USERDATA); }

static COLORREF surface_color(const Palette& P, const Ctl* c) {
    switch (c->surface) {
    case Surface::Card: return P.card;
    case Surface::Field: return P.field;
    case Surface::Sidebar: return P.sidebar;
    case Surface::Custom: return c->custom;
    default: return P.bg;
    }
}

static int t255(float t) { return (int)(std::clamp(t, 0.0f, 1.0f) * 255 + 0.5f); }

// ------------------------------------------------------------------ animation
static bool step(float& v, float target, float k) {   // true while still moving
    v += (target - v) * k;
    if (std::fabs(target - v) < 0.004f) { v = target; return false; }
    return true;
}

static void animate(HWND h, Ctl* c) {
    if (!c->primed) {   // first paint: start at the targets, no animation from zero
        c->checkT = c->checked ? 1.0f : 0.0f;
        c->segPos = (float)c->sel;
        c->primed = true;
    }
    if (c->animating) return;
    c->animating = true;
    c->lastTick = GetTickCount64();
    SetTimer(h, ANIM_TIMER, ANIM_MS, nullptr);
}

static void tick(HWND h, Ctl* c) {
    const ULONGLONG now = GetTickCount64();
    const float dt = (float)std::min<ULONGLONG>(now - c->lastTick, 100);
    c->lastTick = now;
    const float k = 1.0f - std::exp(-dt / EASE_MS);
    const bool en = IsWindowEnabled(h) != FALSE;
    bool moving = false;
    moving |= step(c->hoverT, c->hover && en && (!c->busy || c->cancelable) ? 1.0f : 0.0f, k);
    moving |= step(c->pressT, c->pressed ? 1.0f : 0.0f, k * 1.6f);
    moving |= step(c->checkT, c->checked ? 1.0f : 0.0f, k);
    moving |= step(c->segPos, (float)c->sel, k);
    if (c->progress >= 0) moving |= step(c->shownPct, (float)c->progress, k * 0.6f);
    if (c->busy) {
        c->spin = std::fmod(c->spin + dt * SPIN_DEG_PER_MS, 360.0f);
        moving = true;
    }
    InvalidateRect(h, nullptr, FALSE);
    if (!moving) {
        KillTimer(h, ANIM_TIMER);
        c->animating = false;
    }
}

// ------------------------------------------------------------------ painting
static void fire(HWND h) {
    SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
}

static int seg_at(HWND h, Ctl* c, int x) {
    RECT r;
    GetClientRect(h, &r);
    int n = (int)c->options.size();
    if (n == 0 || r.right <= 0) return -1;
    return std::clamp((int)(x * n / r.right), 0, n - 1);
}

static void spinner(HDC dc, const Ctl* c, float cx, float cy, COLORREF fg, COLORREF bg, UINT dpi) {
    const float r = (float)px(7, dpi), w = std::max(1.5f, px(2, dpi) * 1.0f);
    arc(dc, cx, cy, r, 0, 360, w, mix(fg, bg, 170));         // faint track
    arc(dc, cx, cy, r, c->spin - 90, 110, w, fg);              // moving head
}

// Icon (or spinner) + label, centred in r (or left-aligned from r.left when `left`).
static void draw_content(HDC dc, const Ctl* c, wchar_t icon, RECT r, const std::wstring& t, HFONT f, HFONT fi, COLORREF fg,
                         COLORREF bg, UINT dpi, bool left = false) {
    const bool busy = c->busy && icon == c->icon;   // segment options never spin
    const bool showIcon = icon || busy;
    const int iw = showIcon ? px(18, dpi) : 0, gap = showIcon && !t.empty() ? px(10, dpi) : 0;
    const int tw = t.empty() ? 0 : text_width(dc, t, f);
    int x = left ? r.left : r.left + (r.right - r.left - (iw + gap + tw)) / 2;
    if (busy) spinner(dc, c, x + iw / 2.0f, (r.top + r.bottom) / 2.0f, fg, bg, dpi);
    else if (icon) glyph(dc, icon, { x, r.top, x + iw, r.bottom }, fi, fg);
    if (!t.empty()) text(dc, t, { x + iw + gap, r.top, std::min<LONG>(r.right, x + iw + gap + tw + 2), r.bottom }, f, fg);
}

// Button background, with the progress fill when a percentage is set.
static void button_bg(HDC dc, const Ctl* c, const RECT& r, int rad, COLORREF bg, COLORREF done, COLORREF border = CLR_INVALID) {
    if (c->progress < 0) { round_rect(dc, r, rad, bg, border); return; }
    round_rect(dc, r, rad, bg, border);
    const int w = (int)((r.right - r.left) * std::clamp(c->shownPct, 0.0f, 100.0f) / 100.0f);
    if (w <= 0) return;
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, r.left, r.top, r.left + w, r.bottom);
    round_rect(dc, r, rad, done);
    RestoreDC(dc, saved);
}

static void paint_ctl(HWND h, Ctl* c) {
    Paint p(h);
    const Palette P = pal();
    const UINT dpi = GetDpiForWindow(h);
    const Fonts& F = fonts(dpi);
    const COLORREF surf = surface_color(P, c);
    const bool en = IsWindowEnabled(h) != FALSE;
    const COLORREF white = RGB(255, 255, 255);
    RECT r = p.client;
    fill(p.dc, r, surf);
    wchar_t buf[256];
    GetWindowTextW(h, buf, 256);
    const std::wstring t = buf;
    const int rad = px(10, dpi);
    const int ring = std::max(1, px(2, dpi));
    const bool showFocus = c->focused && c->kbFocus;
    const int ht = t255(c->hoverT), pt = t255(c->pressT);

    switch (c->kind) {
    case Kind::Primary: {
        COLORREF bg = !en ? mix(P.accent, surf, 120) : mix(mix(P.accent, P.accentHover, ht), P.accentPressed, pt);
        if (c->progress >= 0) button_bg(p.dc, c, r, rad, mix(P.accentPressed, RGB(0, 0, 0), 40), P.accent);
        else round_rect(p.dc, r, rad, bg);
        draw_content(p.dc, c, c->icon, r, t, F.bodyBold, F.icon, en ? P.onAccent : mix(P.onAccent, surf, 90), bg, dpi);
        break;
    }
    case Kind::Secondary: {
        COLORREF bg = mix(mix(P.btn, P.btnHover, ht), P.btnPressed, pt);
        round_rect(p.dc, r, rad, bg, mix(P.btnBorder, P.accent, ht / 3));
        draw_content(p.dc, c, c->icon, r, t, F.bodyBold, F.icon, en ? P.text : P.faint, bg, dpi);
        break;
    }
    case Kind::Ghost:
    case Kind::Icon: {
        COLORREF bg = mix(mix(surf, P.btnHover, ht), P.btnPressed, pt);
        if (ht || pt) round_rect(p.dc, r, rad, bg);
        COLORREF fg = !en ? P.faint : c->kind == Kind::Icon ? mix(P.muted, P.text, ht) : P.text;
        draw_content(p.dc, c, c->icon, r, t, F.body, c->kind == Kind::Icon ? F.iconSmall : F.icon, fg, bg, dpi);
        break;
    }
    case Kind::Danger: {
        COLORREF bg = mix(mix(P.dangerSoft, P.danger, ht), P.dangerHover, pt);
        round_rect(p.dc, r, rad, bg);
        draw_content(p.dc, c, c->icon, r, t, F.bodyBold, F.icon, !en ? P.faint : mix(P.danger, white, ht), bg, dpi);
        break;
    }
    case Kind::Link: {
        COLORREF fg = !en ? P.faint : mix(P.accentText, P.accentHover, ht);
        draw_content(p.dc, c, c->icon, r, t, F.smallBold, F.iconSmall, fg, surf, dpi);
        break;
    }
    case Kind::Hero: {   // solid white on the gradient card
        COLORREF bg = !en ? mix(white, surf, 120) : mix(mix(white, mix(white, P.heroFrom, 30), ht), mix(white, P.heroFrom, 60), pt);
        button_bg(p.dc, c, r, rad, bg, mix(white, P.heroFrom, 40));
        draw_content(p.dc, c, c->icon, r, t, F.bodyBold, F.icon, P.heroFrom, bg, dpi);
        break;
    }
    case Kind::HeroGhost: {   // translucent white on the gradient card
        COLORREF bg = mix(surf, white, 46 + ht * 26 / 255 + pt * 24 / 255);
        round_rect(p.dc, r, rad, bg);
        draw_content(p.dc, c, c->icon, r, t, F.bodyBold, F.icon, white, bg, dpi);
        break;
    }
    case Kind::Nav: {
        COLORREF bg = c->checked ? P.accentSoft : mix(surf, P.btnHover, ht);
        if (c->checked || ht) round_rect(p.dc, r, rad, bg);
        if (c->checked)
            round_rect(p.dc, { r.left, r.top + px(10, dpi), r.left + px(4, dpi), r.bottom - px(10, dpi) }, px(2, dpi), P.accent);
        RECT cr{ r.left + px(18, dpi), r.top, r.right - px(12, dpi), r.bottom };
        draw_content(p.dc, c, c->icon, cr, t, c->checked ? F.bodyBold : F.body, F.icon,
                     c->checked ? P.accentText : mix(P.muted, P.text, ht), bg, dpi, true);
        if (c->badge) circle(p.dc, (float)r.right - px(18, dpi), (r.top + r.bottom) / 2.0f, (float)px(4, dpi), P.danger);
        break;
    }
    case Kind::Toggle: {
        const int tw = px(42, dpi), th = px(24, dpi);
        const float k = std::clamp(c->checkT, 0.0f, 1.0f);
        RECT tr{ r.left + 1, (r.bottom - th) / 2, r.left + 1 + tw, (r.bottom - th) / 2 + th };
        COLORREF on = !en ? mix(P.accent, surf, 120) : mix(P.accent, P.accentHover, ht);
        COLORREF track = mix(P.field, on, t255(k));
        COLORREF border = mix(mix(P.fieldBorder, P.fieldHover, ht), on, t255(k));
        round_rect(p.dc, tr, th / 2, track, border, std::max(1, px(1, dpi)));
        const float left = tr.left + th / 2.0f, right = tr.right - th / 2.0f;
        const float kr = px(7, dpi) + (px(9, dpi) - px(7, dpi)) * k - 0.5f * k;
        circle(p.dc, left + (right - left) * k, (tr.top + tr.bottom) / 2.0f, kr,
               mix(en ? P.faint : P.divider, white, t255(k)));
        if (showFocus) {
            RECT fr = tr;
            InflateRect(&fr, px(3, dpi), px(3, dpi));
            round_rect(p.dc, fr, th / 2 + px(3, dpi), CLR_INVALID, P.focus, ring);
        }
        text(p.dc, t, { tr.right + px(12, dpi), r.top, r.right, r.bottom }, F.body, en ? P.text : P.faint);
        return;
    }
    case Kind::Segmented: {
        round_rect(p.dc, r, rad, P.field, P.fieldBorder, std::max(1, px(1, dpi)));
        const int n = (int)c->options.size();
        if (n == 0) break;
        const int in = px(4, dpi);
        const float segW = (float)(r.right - r.left) / n;
        auto seg_rect = [&](float pos) {
            return RECT{ (LONG)(r.left + segW * pos + in), r.top + in, (LONG)(r.left + segW * (pos + 1) - in), r.bottom - in };
        };
        if (c->hoverSeg >= 0 && c->hoverSeg != c->sel && en) round_rect(p.dc, seg_rect((float)c->hoverSeg), rad - in / 2, P.btnHover);
        round_rect(p.dc, seg_rect(c->segPos), rad - in / 2, P.accent);   // the gliding highlight
        for (int i = 0; i < n; ++i) {
            const float w = std::max(0.0f, 1.0f - std::fabs(c->segPos - i));
            COLORREF fg = mix(P.muted, P.onAccent, t255(w));
            draw_content(p.dc, c, c->options[i].first, seg_rect((float)i), c->options[i].second, i == c->sel ? F.smallBold : F.small, F.iconSmall,
                         fg, P.accent, dpi);
        }
        break;
    }
    }
    if (showFocus)
        round_rect(p.dc, r, rad, CLR_INVALID, c->kind == Kind::Hero || c->kind == Kind::HeroGhost ? white : P.focus, ring);
}

static LRESULT CALLBACK ctl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Ctl* c = ctl_of(h);
    if (!c) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        if (!c->primed) animate(h, c);
        paint_ctl(h, c);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == ANIM_TIMER) tick(h, c);
        return 0;
    case WM_SETTEXT:
    case WM_ENABLE: {
        LRESULT r = DefWindowProcW(h, msg, wp, lp);
        if (msg == WM_ENABLE && !wp) c->hover = c->pressed = false;
        animate(h, c);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_MOUSEMOVE: {
        bool changed = !c->hover;
        if (!c->hover) {
            c->hover = true;
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
        }
        if (c->kind == Kind::Segmented) {
            int s = seg_at(h, c, GET_X_LPARAM(lp));
            changed = changed || s != c->hoverSeg;
            c->hoverSeg = s;
        }
        if (changed) { animate(h, c); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        c->hover = false;
        c->hoverSeg = -1;
        animate(h, c);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, c->busy && !c->cancelable ? IDC_APPSTARTING : IsWindowEnabled(h) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (c->busy && !c->cancelable) return 0;
        if (c->kind != Kind::Icon) {   // icon buttons sit inside fields: keep the caret there
            c->mouseDown = true;
            SetFocus(h);
            c->mouseDown = false;
        }
        c->pressed = true;
        SetCapture(h);
        animate(h, c);
        return 0;
    case WM_LBUTTONUP: {
        if (!c->pressed) return 0;
        c->pressed = false;
        ReleaseCapture();
        animate(h, c);
        RECT r;
        GetClientRect(h, &r);
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!PtInRect(&r, pt)) return 0;
        if (c->kind == Kind::Toggle) c->checked = !c->checked;
        if (c->kind == Kind::Segmented) {
            int s = seg_at(h, c, pt.x);
            if (s < 0 || s == c->sel) return 0;
            c->sel = s;
        }
        fire(h);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (c->pressed) { c->pressed = false; animate(h, c); }
        return 0;
    case WM_KEYDOWN:
        if (c->busy && !c->cancelable) return 0;
        if (wp == VK_SPACE && c->kind != Kind::Segmented) {
            c->pressed = true;
            animate(h, c);
            return 0;
        }
        if (c->kind == Kind::Segmented && (wp == VK_LEFT || wp == VK_RIGHT)) {
            int s = std::clamp(c->sel + (wp == VK_LEFT ? -1 : 1), 0, (int)c->options.size() - 1);
            if (s != c->sel) { c->sel = s; animate(h, c); fire(h); }
            return 0;
        }
        break;
    case WM_KEYUP:
        if (wp == VK_SPACE && c->pressed) {
            c->pressed = false;
            if (c->kind == Kind::Toggle) c->checked = !c->checked;
            animate(h, c);
            fire(h);
            return 0;
        }
        break;
    case WM_GETDLGCODE:
        if (c->kind == Kind::Segmented) return DLGC_WANTARROWS;
        if (c->kind == Kind::Toggle) return DLGC_BUTTON;
        return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;   // Enter clicks the focused button
    case BM_SETSTYLE:
        return 0;   // the dialog manager's default-button styling does not apply
    case WM_SETFOCUS:
        c->focused = true;
        c->kbFocus = !c->mouseDown && !(GetKeyState(VK_LBUTTON) & 0x8000);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        c->focused = false;
        c->pressed = false;
        animate(h, c);
        return 0;
    case WM_NCDESTROY:
        KillTimer(h, ANIM_TIMER);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        delete c;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void register_controls(HINSTANCE hi) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = ctl_proc;
    wc.hInstance = hi;
    wc.lpszClassName = CTL_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassW(&wc);
}

HWND make_control(HWND parent, int id, Kind kind, const std::wstring& t, wchar_t icon, Surface surface) {
    auto* c = new Ctl{ kind, surface, icon };
    HWND h = CreateWindowExW(0, CTL_CLASS, t.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (!h) { delete c; return nullptr; }
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
    return h;
}

void set_icon(HWND h, wchar_t icon) {
    if (Ctl* c = ctl_of(h)) { c->icon = icon; InvalidateRect(h, nullptr, FALSE); }
}
void set_surface(HWND h, COLORREF color) {
    if (Ctl* c = ctl_of(h)) { c->surface = Surface::Custom; c->custom = color; InvalidateRect(h, nullptr, FALSE); }
}
bool checked(HWND h) {
    Ctl* c = ctl_of(h);
    return c && c->checked;
}
void set_checked(HWND h, bool on) {
    if (Ctl* c = ctl_of(h)) { c->checked = on; animate(h, c); InvalidateRect(h, nullptr, FALSE); }
}
void set_badge(HWND h, bool on) {
    if (Ctl* c = ctl_of(h); c && c->badge != on) { c->badge = on; InvalidateRect(h, nullptr, FALSE); }
}
void set_busy(HWND h, bool busy, bool cancelable) {
    Ctl* c = ctl_of(h);
    if (!c) return;
    c->cancelable = busy && cancelable;
    if (c->busy == busy) return;
    c->busy = busy;
    if (busy) c->pressed = false;
    animate(h, c);
    InvalidateRect(h, nullptr, FALSE);
}
void set_progress(HWND h, int percent) {
    Ctl* c = ctl_of(h);
    if (!c || c->progress == percent) return;
    if (c->progress < 0 && percent >= 0) c->shownPct = 0;   // a new download starts from the left
    c->progress = percent;
    animate(h, c);
    InvalidateRect(h, nullptr, FALSE);
}
void set_options(HWND h, const std::vector<std::pair<wchar_t, std::wstring>>& o) {
    if (Ctl* c = ctl_of(h)) { c->options = o; InvalidateRect(h, nullptr, FALSE); }
}
int selected(HWND h) {
    Ctl* c = ctl_of(h);
    return c ? c->sel : 0;
}
void set_selected(HWND h, int i) {
    if (Ctl* c = ctl_of(h)) { c->sel = i; animate(h, c); InvalidateRect(h, nullptr, FALSE); }
}

// ------------------------------------------------------------------ placeholders
struct Placeholder {
    WNDPROC orig;
    std::wstring text;
};
static const wchar_t* PH_PROP = L"RemoteDeskPlaceholder";

static LRESULT CALLBACK placeholder_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ph = (Placeholder*)GetPropW(h, PH_PROP);
    if (!ph) return DefWindowProcW(h, msg, wp, lp);
    WNDPROC orig = ph->orig;
    if (msg == WM_PAINT && GetWindowTextLengthW(h) == 0) {
        const Palette P = pal();
        HideCaret(h);
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        fill(dc, r, P.field);
        HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        text(dc, ph->text, r, f, P.faint, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        EndPaint(h, &ps);
        ShowCaret(h);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)orig);
        RemovePropW(h, PH_PROP);
        delete ph;
    }
    LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
    if ((msg == WM_SETTEXT || msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_PASTE || msg == WM_CUT ||
         msg == WM_CLEAR || msg == WM_UNDO) && GetWindowTextLengthW(h) <= 1)
        InvalidateRect(h, nullptr, TRUE);   // switching between placeholder and text
    return r;
}

void placeholder(HWND edit, const std::wstring& t) {
    if (auto* ph = (Placeholder*)GetPropW(edit, PH_PROP)) { ph->text = t; InvalidateRect(edit, nullptr, TRUE); return; }
    auto* ph = new Placeholder{ nullptr, t };
    SetPropW(edit, PH_PROP, ph);
    ph->orig = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)placeholder_proc);
}

}  // namespace ui

namespace ui {
void set_kind(HWND h, Kind kind) {
    if (Ctl* c = ctl_of(h); c && c->kind != kind) { c->kind = kind; InvalidateRect(h, nullptr, FALSE); }
}
}  // namespace ui
