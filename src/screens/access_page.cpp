// Access page: what people connecting to this PC may do.
//   - defaults for requests you approve (pre-set in the "wants to connect" prompt)
//   - what a correct password grants (unattended access)
//   - everyone connected right now, with a Manage button each (change or revoke at any time)
#include "screens/main_window.h"
#include "remote/audio.h"

namespace mw::access {

constexpr int CARD_W = (CWIDTH - 16) / 2, TOP = 80, CARD_H = 330, IW = CARD_W - 48;
constexpr Box C_ASK{ CX, TOP, CARD_W, CARD_H };
constexpr Box C_PWD{ CX + CARD_W + 16, TOP, CARD_W, CARD_H };
constexpr Box C_NOW{ CX, TOP + CARD_H + 16, CWIDTH, CH - (TOP + CARD_H + 16) - 24 };
constexpr int ROW_H = 52, ROWS_TOP = C_NOW.y + 66;

static const struct { uint8_t bit; const wchar_t* title; } PERMS[] = {
    { PERM_CONTROL, L"Control mouse and keyboard" },
    { PERM_CLIPBOARD, L"Share the clipboard" },
    { PERM_RECORD, L"Screenshots and recording" },
    { PERM_FILES, L"Send and receive files" },
    { PERM_AUDIO, L"Hear this computer's sound" },
};
constexpr int N_PERMS = sizeof(PERMS) / sizeof(PERMS[0]);

static Box toggle_box(const Box& card, int i) { return { card.x + 24, card.y + 100 + i * 44, IW, 32 }; }

static void sync() {
    const uint8_t ask = default_perms(false), pwd = default_perms(true);
    for (int i = 0; i < N_PERMS; ++i) {
        ui::set_checked(ctl(IDC_ASK_FIRST + i), (ask & PERMS[i].bit) != 0);
        ui::set_checked(ctl(IDC_PWD_FIRST + i), (pwd & PERMS[i].bit) != 0);
    }
}

void create(HWND hwnd) {
    for (int i = 0; i < N_PERMS; ++i) {
        ui::make_control(hwnd, IDC_ASK_FIRST + i, ui::Kind::Toggle, PERMS[i].title);
        ui::make_control(hwnd, IDC_PWD_FIRST + i, ui::Kind::Toggle, PERMS[i].title);
    }
    sync();
}

void layout() {
    for (int i = 0; i < N_PERMS; ++i) {
        place(IDC_ASK_FIRST + i, toggle_box(C_ASK, i));
        place(IDC_PWD_FIRST + i, toggle_box(C_PWD, i));
    }
}

void set_visible(bool on) {
    for (int i = 0; i < N_PERMS; ++i) {
        ShowWindow(ctl(IDC_ASK_FIRST + i), on ? SW_SHOWNA : SW_HIDE);
        ShowWindow(ctl(IDC_PWD_FIRST + i), on ? SW_SHOWNA : SW_HIDE);
    }
    if (on) sync();
}

static std::vector<SessionInfo> incoming() {
    std::vector<SessionInfo> v;
    for (const SessionInfo& s : S.sessions) if (s.incoming) v.push_back(s);
    return v;
}

static std::wstring summary(uint8_t p) {
    std::wstring s = L"Sees your screen";
    if (p & PERM_CONTROL) s += L" · controls";
    if (p & PERM_CLIPBOARD) s += L" · clipboard";
    if (p & PERM_RECORD) s += L" · screenshots & recording";
    if (p & PERM_FILES) s += L" · files";
    if (p & PERM_AUDIO) s += L" · sound";
    if (!(p & PERM_CONTROL)) s += L"  (view only)";
    return s;
}

static RECT row_rect(int i) { return ui::rc(C_NOW.x + 12, ROWS_TOP + i * ROW_H, C_NOW.w - 24, ROW_H - 4, S.dpi); }
static int max_rows() { return (C_NOW.y + C_NOW.h - 12 - ROWS_TOP) / ROW_H; }

void paint(HDC dc, const ui::Palette& Pl, const ui::Fonts& F) {
    ui::text(dc, L"Access", ui::rc(CX, 26, 400, 34, S.dpi), F.heading, Pl.text);

    card(dc, Pl, C_ASK);
    card_title(dc, Pl, F, C_ASK.x + 24, C_ASK.y + 22, ui::ico::Shield, L"When someone asks to connect");
    ui::text(dc, L"Switched on in the approval prompt to begin with.", ui::rc(C_ASK.x + 24, C_ASK.y + 70, IW, 20, S.dpi),
             F.small, Pl.muted);

    card(dc, Pl, C_PWD);
    card_title(dc, Pl, F, C_PWD.x + 24, C_PWD.y + 22, ui::ico::Key, L"With your password");
    ui::text(dc, password_is_set() ? L"What unattended access allows, with no prompt."
                                   : L"Applies once unattended access is on (Settings).",
             ui::rc(C_PWD.x + 24, C_PWD.y + 70, IW, 20, S.dpi), F.small, Pl.muted);

    card(dc, Pl, C_NOW);
    const std::vector<SessionInfo> now = incoming();
    card_title(dc, Pl, F, C_NOW.x + 24, C_NOW.y + 22, ui::ico::Eye, L"Connected to this PC now");
    ui::text(dc, L"Change or take back permissions at any time — it applies at once.",
             ui::rc(C_NOW.x + 24 + 330, C_NOW.y + 24, C_NOW.w - 48 - 330, 22, S.dpi), F.small, Pl.muted,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (now.empty()) {
        ui::text(dc, L"Nobody is connected.", ui::rc(C_NOW.x + 24, ROWS_TOP + 8, C_NOW.w - 48, 22, S.dpi), F.body, Pl.faint);
        return;
    }
    const int n = std::min<int>((int)now.size(), max_rows());
    for (int i = 0; i < n; ++i) {
        const SessionInfo& s = now[i];
        RECT r = row_rect(i);
        ui::round_rect(dc, r, P(12), Pl.dark ? Pl.btnHover : Pl.divider);
        RECT ic{ r.left + P(12), r.top + P(8), r.left + P(44), r.bottom - P(8) };
        ui::glyph(dc, s.recording ? 0xE7C8 : ui::ico::Eye, ic, F.icon, s.recording ? Pl.danger : Pl.warning);
        ui::text(dc, widen(format_id(s.peerId)), { r.left + P(52), r.top, r.left + P(200), r.bottom }, F.bodyBold, Pl.text);
        std::wstring xfer = session_transfer_status(s.key);
        if (xfer.empty() && s.call) xfer = call_state_text(s.call, s.muted);
        ui::text(dc, s.recording ? L"● Recording your screen · " + summary(s.perms) : !xfer.empty() ? xfer : summary(s.perms),
                 { r.left + P(200), r.top, r.right - P(130), r.bottom }, F.small, s.recording ? Pl.danger : Pl.muted);
        RECT b{ r.right - P(118), r.top + P(8), r.right - P(10), r.bottom - P(8) };
        pill(dc, b, Pl.accentSoft, Pl.accentText, L"Manage ›", F.smallBold, false);
    }
}

bool click(LPARAM lp) {
    const std::vector<SessionInfo> now = incoming();
    const int n = std::min<int>((int)now.size(), max_rows());
    for (int i = 0; i < n; ++i)
        if (in_rect(row_rect(i), lp)) { show_session_panel(g_wnd, now[i].key); return true; }
    return false;
}

bool set_cursor(LPARAM lp) {
    const int n = std::min<int>((int)incoming().size(), max_rows());
    for (int i = 0; i < n; ++i)
        if (in_rect(row_rect(i), lp)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return true; }
    return false;
}

bool command(int id, int) {
    const bool ask = id >= IDC_ASK_FIRST && id < IDC_ASK_FIRST + N_PERMS, pwd = id >= IDC_PWD_FIRST && id < IDC_PWD_FIRST + N_PERMS;
    if (!ask && !pwd) return false;
    const int first = ask ? IDC_ASK_FIRST : IDC_PWD_FIRST;
    uint8_t p = 0;
    for (int i = 0; i < N_PERMS; ++i) if (ui::checked(ctl(first + i))) p |= PERMS[i].bit;
    set_default_perms(pwd, p);
    return true;
}

void refresh() { if (S.page == PAGE_ACCESS) invalidate(C_NOW, 2); }

}  // namespace mw::access
