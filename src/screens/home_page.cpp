// Home page: a gradient card with this PC's address and the connect form, then the
// sessions that are open right now and the recent connections (one click to reconnect).
#include "screens/main_window.h"
#include "remote/files.h"
#include "remote/audio.h"
#include <windowsx.h>
#include <commdlg.h>
#include <cwctype>
#include <ctime>
#include <thread>

namespace mw::home {

constexpr Box HERO{ CX, 32, CWIDTH, 200 };
constexpr int LX = CX + 36;                     // left half of the hero card
constexpr int RX = CX + 452, RW = CWIDTH - 452 - 36;   // right half: connect form
constexpr Box BOX_ID{ LX - 2, 84, 380, 58 };
constexpr Box BOX_COPY{ LX, 158, 118, 42 };
constexpr Box BOX_CONNECT{ RX + RW - 112, 150, 112, 44 };
constexpr Box BOX_INFO{ CX + 4, 244, CWIDTH - 8, 24 };
constexpr int SECTION_Y = 284;
constexpr int COLS = 2, GAP = 16, CHIP_H = 72, TILE_H = 84;
constexpr int ITEM_W = (CWIDTH - (COLS - 1) * GAP) / COLS;

static const Field FIELDS[] = {
    { IDC_PEER, { RX, 86, RW, 52 }, 0, true },
    { IDC_PEER_PW, { RX, 150, RW - 124, 44 }, 0, false },
};
static const int IDS[] = { IDC_COPY, IDC_PEER, IDC_PEER_PW, IDC_CONNECT };

static int g_sessionsTitleY = 0, g_recentTitleY = 0;   // computed by build_hits()

const Field* field(int id) {
    for (const Field& f : FIELDS) if (f.id == id) return &f;
    return nullptr;
}

// Colour of the hero gradient at a point, so controls on it blend in.
static COLORREF hero_at(const ui::Palette& Pl, int x, int y) {
    const double w = HERO.w, h = HERO.h;
    double t = ((x - HERO.x) * w + (y - HERO.y) * h) / (w * w + h * h);
    return ui::mix(Pl.heroFrom, Pl.heroTo, (int)(std::clamp(t, 0.0, 1.0) * 255));
}

void restyle() {
    const ui::Palette Pl = ui::pal();
    ui::set_surface(ctl(IDC_COPY), hero_at(Pl, BOX_COPY.x + BOX_COPY.w / 2, BOX_COPY.y + BOX_COPY.h / 2));
    ui::set_surface(ctl(IDC_CONNECT), hero_at(Pl, BOX_CONNECT.x + BOX_CONNECT.w / 2, BOX_CONNECT.y + BOX_CONNECT.h / 2));
}

void create(HWND hwnd) {
    ui::make_control(hwnd, IDC_COPY, ui::Kind::HeroGhost, L"Copy", ui::ico::Copy, ui::Surface::Custom);
    make_edit(IDC_PEER, 0, L"Partner address", 32);
    make_edit(IDC_PEER_PW, ES_PASSWORD, L"Password (optional)", 128);
    ui::make_control(hwnd, IDC_CONNECT, ui::Kind::Hero, L"Connect", 0, ui::Surface::Custom);
    restyle();
    refresh();
}

void layout() {
    for (const Field& f : FIELDS) place_field(f);
    place(IDC_COPY, BOX_COPY);
    place(IDC_CONNECT, BOX_CONNECT);
}

void set_visible(bool on) {
    for (int id : IDS) ShowWindow(ctl(id), on ? SW_SHOWNA : SW_HIDE);
}

// ---- sessions and recent connections ----
static Box item_box(int top, int i, int h) {
    return { CX + (i % COLS) * (ITEM_W + GAP), top + (i / COLS) * (h + GAP), ITEM_W, h };
}

static void build_hits() {
    S.hits.clear();
    int y = SECTION_Y;
    g_sessionsTitleY = 0;
    if (!S.sessions.empty()) {
        g_sessionsTitleY = y;
        const int n = std::min<int>((int)S.sessions.size(), COLS * 2);
        for (int i = 0; i < n; ++i) {
            const SessionInfo& s = S.sessions[i];
            Box b = item_box(y + 36, i, CHIP_H);
            RECT r = R(b);
            RECT x{ r.right - P(40), r.top + (r.bottom - r.top - P(30)) / 2, r.right - P(10), r.top + (r.bottom - r.top + P(30)) / 2 };
            S.hits.push_back({ r, x, true, s.key, s.peerId });
        }
        y += 36 + ((n + COLS - 1) / COLS) * (CHIP_H + GAP) + 12;
    }
    g_recentTitleY = y;
    const int rows = std::max(1, (CH - 24 - (y + 36)) / (TILE_H + GAP));
    const int n = std::min<int>((int)S.history.size(), rows * COLS);
    for (int i = 0; i < n; ++i) {
        Box b = item_box(y + 36, i, TILE_H);
        RECT r = R(b);
        RECT x{ r.right - P(36), r.top + P(8), r.right - P(8), r.top + P(36) };
        S.hits.push_back({ r, x, false, 0, S.history[i].id });
    }
}

void refresh() {
    S.history = history_list();
    S.sessions = sessions();
    S.hover = -1;
    build_hits();
    if (S.page == PAGE_HOME) invalidate_content();
}

static COLORREF avatar_color(const std::string& id) {
    static const COLORREF C[] = { RGB(0x63, 0x66, 0xF1), RGB(0x8B, 0x5C, 0xF6), RGB(0x14, 0xB8, 0xA6),
                                  RGB(0xF5, 0x9E, 0x0B), RGB(0xEC, 0x48, 0x99), RGB(0x0E, 0xA5, 0xE9) };
    unsigned h = 0;
    for (char c : id) h = h * 31 + (unsigned char)c;
    return C[h % 6];
}

static std::wstring ago(long long t) {
    long long d = (long long)time(nullptr) - t;
    if (d < 60) return L"Just now";
    if (d < 3600) return std::to_wstring(d / 60) + L" min ago";
    if (d < 86400) return std::to_wstring(d / 3600) + (d < 7200 ? L" hour ago" : L" hours ago");
    if (d < 7 * 86400) return std::to_wstring(d / 86400) + (d < 2 * 86400 ? L" day ago" : L" days ago");
    wchar_t buf[32];
    time_t tt = (time_t)t;
    tm lt{};
    localtime_s(&lt, &tt);
    wcsftime(buf, 32, L"%d %b %Y", &lt);
    return buf;
}

static std::wstring duration(ULONGLONG since) {
    ULONGLONG m = (GetTickCount64() - since) / 60000;
    return m < 1 ? L"just started" : m < 60 ? std::to_wstring(m) + L" min" : std::to_wstring(m / 60) + L" h " + std::to_wstring(m % 60) + L" min";
}

static void section_title(HDC dc, const ui::Palette& Pl, const ui::Fonts& F, int y, const std::wstring& t, size_t count) {
    ui::text(dc, t, ui::rc(CX, y, 400, 26, S.dpi), F.title, Pl.text);
    if (count) {
        HDC tmp = dc;
        int w = ui::text_width(tmp, t, F.title);
        RECT r{ P(CX) + w + P(10), P(y + 3), P(CX) + w + P(10) + P(26), P(y + 23) };
        pill(dc, r, Pl.accentSoft, Pl.accentText, std::to_wstring(count), F.smallBold, false);
    }
}

static void paint_close(HDC dc, const ui::Palette& Pl, const ui::Fonts& F, const RECT& x, bool hot) {
    if (hot) ui::round_rect(dc, x, P(8), Pl.dangerSoft);
    ui::glyph(dc, ui::ico::Close, x, F.iconSmall, hot ? Pl.danger : Pl.faint);
}

static void paint_items(HDC dc, const ui::Palette& Pl, const ui::Fonts& F) {
    if (g_sessionsTitleY) section_title(dc, Pl, F, g_sessionsTitleY, L"Active sessions", S.sessions.size());
    section_title(dc, Pl, F, g_recentTitleY, L"Recent connections", 0);

    for (size_t i = 0; i < S.hits.size(); ++i) {
        const Hit& h = S.hits[i];
        const bool hot = (int)i == S.hover;
        RECT r = h.r;
        ui::round_rect(dc, r, P(14), hot ? (Pl.dark ? Pl.btnHover : Pl.card) : Pl.card, hot ? Pl.accent : Pl.cardBorder,
                       std::max(1, P(hot ? 2 : 1)));
        const int cy = (r.top + r.bottom) / 2;
        const float ar = (float)P(20);
        const float ax = r.left + P(18) + ar;

        std::wstring name, sub;
        COLORREF subColor = Pl.muted;
        wchar_t subIcon = 0;
        if (h.session) {
            const SessionInfo* s = nullptr;
            for (auto& x : S.sessions) if (x.key == h.key) s = &x;
            if (!s) continue;
            ui::circle(dc, ax, (float)cy, ar, s->incoming ? Pl.warningSoft : Pl.accentSoft);
            ui::glyph(dc, s->incoming ? ui::ico::Eye : ui::ico::Monitor,
                      { (int)(ax - ar), (int)(cy - ar), (int)(ax + ar), (int)(cy + ar) }, F.icon,
                      s->incoming ? Pl.warning : Pl.accentText);
            name = widen(format_id(s->peerId));
            const std::wstring transfer = s->incoming ? session_transfer_status(s->key) : L"";
            if (s->incoming && s->recording) {   // the viewer told us: make it impossible to miss
                sub = L"● Recording your screen · " + duration(s->since);
                subColor = Pl.danger;
            } else if (!transfer.empty()) {      // "Sending report.pdf · 45%"
                sub = transfer;
                subColor = Pl.accentText;
            } else if (s->incoming && s->call) { // "On a call" / "Calling…" / "Incoming call"
                sub = std::wstring(call_state_text(s->call, s->muted)) + L" · click to manage";
                subColor = Pl.success;
            } else {
                sub = s->incoming ? std::wstring((s->perms & PERM_CONTROL) ? L"Viewing this PC" : L"View only")
                                    + L" · click to manage"
                                  : L"You're connected · " + duration(s->since);
                subColor = s->incoming ? Pl.warning : Pl.success;
            }
        } else {
            const HistoryEntry* e = nullptr;
            for (auto& x : S.history) if (x.id == h.id) e = &x;
            if (!e) continue;
            ui::circle(dc, ax, (float)cy, ar, avatar_color(e->id));
            ui::glyph(dc, ui::ico::Monitor, { (int)(ax - ar), (int)(cy - ar), (int)(ax + ar), (int)(cy + ar) }, F.icon,
                      RGB(255, 255, 255));
            name = e->name.empty() ? widen(format_id(e->id)) : e->name;
            sub = ago(e->lastUsed);
            if (e->hasPassword) { subIcon = ui::ico::Lock; sub += L"  ·  saved password, connects instantly"; }
            else sub += L"  ·  asks for permission";
            if (const int n = FileEngine::pending_for(e->id)) {   // interrupted sends wait for the next connection
                subIcon = ui::ico::Refresh;
                sub = std::to_wstring(n) + (n == 1 ? L" unfinished file" : L" unfinished files") + L"  ·  connect to continue";
                subColor = Pl.accentText;
            }
        }
        const int tx = (int)(ax + ar) + P(14), right = h.close.left - P(4);
        ui::text(dc, name, { tx, cy - P(22), right, cy }, F.bodyBold, Pl.text);
        RECT sr{ tx, cy + P(2), right, cy + P(22) };
        if (subIcon) {
            ui::glyph(dc, subIcon, { sr.left, sr.top, sr.left + P(14), sr.bottom }, F.iconSmall, subColor);
            sr.left += P(18);
        }
        ui::text(dc, sub, sr, F.small, subColor);
        if (h.session || hot) paint_close(dc, Pl, F, h.close, hot && S.hoverClose);
    }

    if (S.history.empty()) {   // friendly empty state
        Box b{ CX, g_recentTitleY + 36, CWIDTH, TILE_H + 24 };
        RECT r = R(b);
        ui::round_rect(dc, r, P(14), CLR_INVALID, Pl.cardBorder);
        RECT ic{ r.left + P(28), r.top, r.left + P(68), r.bottom };
        ui::glyph(dc, ui::ico::History, ic, F.iconLarge, Pl.faint);
        ui::text(dc, L"No recent connections yet", { r.left + P(80), r.top + P(28), r.right - P(20), r.top + P(52) },
                 F.bodyBold, Pl.muted);
        ui::text(dc, L"Devices you connect to will show up here for one-click access.",
                 { r.left + P(80), r.top + P(54), r.right - P(20), r.top + P(76) }, F.small, Pl.faint);
    }
}

void paint(HDC dc, const ui::Palette& Pl, const ui::Fonts& F) {
    // Hero card
    RECT hero = R(HERO);
    ui::gradient_rect(dc, hero, P(20), Pl.heroFrom, Pl.heroTo);
    ui::text(dc, L"YOUR ADDRESS", ui::rc(LX, 62, 300, 18, S.dpi), F.smallBold, Pl.onHeroMuted);
    ui::text(dc, widen(format_id(my_id())), R(BOX_ID), F.id, Pl.onHero);
    const int dx = CX + 420;
    ui::fill(dc, ui::rc(dx, 62, 1, 140, S.dpi), ui::mix(hero_at(Pl, dx, 130), RGB(255, 255, 255), 70));
    ui::text(dc, L"CONNECT TO A DEVICE", ui::rc(RX, 62, RW, 18, S.dpi), F.smallBold, Pl.onHeroMuted);
    for (const Field& f : FIELDS) {
        const bool focus = GetFocus() == ctl(f.id);
        ui::round_rect(dc, R(f.box), P(10), Pl.field, focus ? RGB(255, 255, 255) : CLR_INVALID, std::max(1, P(2)));
    }

    // Status of the last action (only while there is something to say)
    if (!S.info.empty()) {
        COLORREF c = S.infoKind == INFO_ERROR ? Pl.danger : S.infoKind == INFO_OK ? Pl.success
                   : S.infoKind == INFO_BUSY ? Pl.accentText : Pl.muted;
        wchar_t icon = S.infoKind == INFO_ERROR ? ui::ico::Error : S.infoKind == INFO_OK ? ui::ico::Check
                     : S.infoKind == INFO_BUSY ? ui::ico::Refresh : ui::ico::Info;
        RECT r = R(BOX_INFO);
        ui::glyph(dc, icon, { r.left, r.top, r.left + P(18), r.bottom }, F.iconSmall, c);
        r.left += P(26);
        ui::text(dc, S.info, r, F.bodyBold, c);
    }
    paint_items(dc, Pl, F);
}

// ---- actions ----
// Host side: pick files and send them to the person viewing this PC (they arrive in their
// Downloads\RemoteDesk; the viewer's app decides whether it may receive them).
void send_files_to(int key, const std::string& peerId) {
    std::vector<wchar_t> buf(32768);
    OPENFILENAMEW of{ sizeof(of) };
    of.hwndOwner = g_wnd;
    of.lpstrFile = buf.data();
    of.nMaxFile = (DWORD)buf.size();
    const std::wstring title = L"Send files to " + widen(format_id(peerId));
    of.lpstrTitle = title.c_str();
    of.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    std::vector<std::wstring> paths;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) paths.push_back(first);
    for (; *p; p += wcslen(p) + 1) paths.push_back(first + L"\\" + p);
    if (session_send_files(key, paths))
        set_info(INFO_BUSY, L"Sending " + std::to_wstring(paths.size()) + (paths.size() == 1 ? L" file to " : L" files to ") +
                                widen(format_id(peerId)) + L"…");
    else
        set_info(INFO_ERROR, L"That session has ended.");
}

static void connect_to(const std::string& id, std::string pw) {
    if (const int key = session_find(id, false)) {   // already open: bring that window up instead of a second one
        session_focus(key);
        set_info(INFO_HINT, L"You're already connected to " + widen(format_id(id)));
        return;
    }
    bool saved = false;
    if (pw.empty()) {   // a password that worked before is used again
        pw = history_password(id);
        saved = !pw.empty();
    }
    std::thread(viewer_session, id, pw, saved).detach();
}

static void on_connect() {
    std::string id = digits_only(narrow(get_text(IDC_PEER)));
    if (id.size() != 9) {
        set_info(INFO_ERROR, id.empty() ? L"Enter the 9-digit address of the other computer." : L"An address has exactly 9 digits.");
        SetFocus(ctl(IDC_PEER));
        return;
    }
    if (id == my_id()) {
        set_info(INFO_ERROR, L"That's this computer's own address.");
        SetFocus(ctl(IDC_PEER));
        return;
    }
    std::string pw = narrow(get_text(IDC_PEER_PW));
    SetWindowTextW(ctl(IDC_PEER_PW), L"");
    connect_to(id, pw);
}

static void CALLBACK copy_reset(HWND h, UINT, UINT_PTR id, DWORD) {
    KillTimer(h, id);
    SetWindowTextW(ctl(IDC_COPY), L"Copy");
    ui::set_icon(ctl(IDC_COPY), ui::ico::Copy);
}

static void on_copy() {
    clipboard_set(g_wnd, widen(my_id()));
    SetWindowTextW(ctl(IDC_COPY), L"Copied");
    ui::set_icon(ctl(IDC_COPY), ui::ico::Check);
    SetTimer(g_wnd, 77, 1600, copy_reset);
}

// Keeps the partner address as "123 456 789" while typing or pasting.
static void format_peer() {
    if (S.formatting) return;
    HWND e = ctl(IDC_PEER);
    std::wstring s = get_text(IDC_PEER);
    DWORD selEnd = 0;
    SendMessageW(e, EM_GETSEL, 0, (LPARAM)&selEnd);
    size_t digitsBefore = 0;
    for (size_t i = 0; i < s.size() && i < selEnd; ++i) if (iswdigit(s[i])) ++digitsBefore;
    std::string d = digits_only(narrow(s));
    if (d.size() > 9) d.resize(9);
    std::wstring f = widen(format_id(d));
    if (f == s) return;
    S.formatting = true;
    SetWindowTextW(e, f.c_str());
    size_t pos = 0, seen = 0;
    while (pos < f.size() && seen < digitsBefore) { if (iswdigit(f[pos])) ++seen; ++pos; }
    SendMessageW(e, EM_SETSEL, pos, pos);
    S.formatting = false;
}

bool command(int id, int code) {
    switch (id) {
    case IDC_CONNECT:
        if (viewer_connecting()) viewer_cancel_connect();   // the button reads "Cancel" meanwhile
        else on_connect();
        return true;
    case IDC_COPY: on_copy(); return true;
    case IDC_PEER:
        if (code == EN_CHANGE) {
            format_peer();
            if (S.infoKind == INFO_ERROR && !S.info.empty()) set_info(INFO_HINT, L"");
        }
        return true;
    case IDC_PEER_PW: return true;
    }
    return false;
}

bool on_enter(HWND focus) {
    if (S.page != PAGE_HOME) return false;
    if (focus == ctl(IDC_COPY)) on_copy();
    else on_connect();
    return true;
}

static int hit_at(LPARAM lp, bool& onClose) {
    for (size_t i = 0; i < S.hits.size(); ++i)
        if (in_rect(S.hits[i].r, lp)) {
            onClose = in_rect(S.hits[i].close, lp);
            return (int)i;
        }
    onClose = false;
    return -1;
}

bool mouse_move(LPARAM lp) {
    bool onClose;
    int h = hit_at(lp, onClose);
    if (h == S.hover && onClose == S.hoverClose) return false;
    if (S.hover >= 0 && S.hover < (int)S.hits.size()) {
        RECT r = S.hits[S.hover].r;
        InflateRect(&r, P(2), P(2));
        InvalidateRect(g_wnd, &r, FALSE);
    }
    S.hover = h;
    S.hoverClose = onClose;
    if (h >= 0) {
        RECT r = S.hits[h].r;
        InflateRect(&r, P(2), P(2));
        InvalidateRect(g_wnd, &r, FALSE);
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, g_wnd, 0 };
        TrackMouseEvent(&t);
    }
    return true;
}

bool click(LPARAM lp) {
    for (const Field& f : FIELDS)
        if (in_rect(R(f.box), lp)) {
            SetFocus(ctl(f.id));
            SendMessageW(ctl(f.id), EM_SETSEL, 0x7fffffff, 0x7fffffff);
            return true;
        }
    if (in_rect(R(BOX_ID), lp)) { on_copy(); return true; }
    bool onClose;
    int i = hit_at(lp, onClose);
    if (i < 0) return false;
    const Hit h = S.hits[i];
    if (h.session) {
        bool incoming = false;
        for (auto& x : S.sessions) if (x.key == h.key) incoming = x.incoming;
        if (onClose) session_end(h.key);
        else if (incoming) show_session_panel(g_wnd, h.key);   // permissions, send files, disconnect
        else session_focus(h.key);
    } else if (onClose) {
        history_remove(h.id);
    } else {
        connect_to(h.id, "");
    }
    return true;
}

bool set_cursor(LPARAM lp) {
    for (const Field& f : FIELDS)
        if (in_rect(R(f.box), lp)) { SetCursor(LoadCursorW(nullptr, IDC_IBEAM)); return true; }
    bool onClose;
    if (in_rect(R(BOX_ID), lp) || hit_at(lp, onClose) >= 0) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return true; }
    return false;
}

}  // namespace mw::home
