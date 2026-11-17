#pragma once
// Internal to the main window: shared state and helpers for
//   main_window.cpp   frame, sidebar, message routing, theme, relay status
//   home_page.cpp     your address, connect, active sessions, recent connections
//   settings_page.cpp relay server, unattended access, general, updates
#include "core/app.h"
#include "app/history.h"
#include "ui/ui.h"

namespace mw {

enum Page { PAGE_HOME, PAGE_ACCESS, PAGE_SETTINGS };

enum {
    IDC_NAV_HOME = 100, IDC_NAV_SETTINGS, IDC_RETRY, IDC_UPDATE_PILL, IDC_NAV_ACCESS,
    IDC_COPY = 200, IDC_PEER, IDC_PEER_PW, IDC_CONNECT,
    IDC_PW = 310, IDC_PW_SAVE, IDC_PW_OFF,
    IDC_THEME = 320, IDC_STARTUP, IDC_LOCKSCREEN, IDC_WOL,
    IDC_UPDATE_BTN = 330,
    IDC_LICENSE_KEY = 340, IDC_LICENSE_BTN, IDC_LICENSE_OFF,
    IDC_ASK_FIRST = 350,   // 5 toggles: defaults for approved requests
    IDC_PWD_FIRST = 360,   // 5 toggles: what the password grants
};

// Layout in 96-DPI units.
constexpr int CW = 1120, CH = 700;
constexpr int SIDEBAR_W = 240;
constexpr int CX = SIDEBAR_W + 36, CWIDTH = CW - SIDEBAR_W - 72;   // content column

struct Box { int x, y, w, h; };

struct Field {   // an EDIT inside a painted rounded box
    int id;
    Box box;
    int dropId;  // icon button at the right end of the box, or 0
    bool big;    // larger font
};

// A clickable painted item on the Home page (session chip or recent tile).
struct Hit {
    RECT r, close;
    bool session;
    int key;            // session key
    std::string id;     // peer address
};

struct State {
    UINT dpi = 96;
    Page page = PAGE_HOME;
    HBRUSH fieldBrush = nullptr;
    HICON logo = nullptr;

    RelayState relay = RS_CONNECTING;
    RelayReason reason = RR_NONE;
    ULONGLONG retryAt = 0;

    InfoKind infoKind = INFO_HINT;
    std::wstring info;                 // status line under the hero card
    std::vector<HistoryEntry> history;
    std::vector<SessionInfo> sessions;
    std::vector<Hit> hits;
    int hover = -1;                    // index into hits
    bool hoverClose = false;
    bool formatting = false;

    std::string promptedVersion;       // the "update available" prompt shows once per version
    // Settings page messages (empty = none); errors win over notes.
    std::wstring errPw, pwNote;
};
extern State S;
extern HWND g_wnd;

HWND ctl(int id);
inline int P(int v) { return ui::px(v, S.dpi); }
inline RECT R(const Box& b) { return ui::rc(b.x, b.y, b.w, b.h, S.dpi); }
std::wstring get_text(int id);
void invalidate(const Box& b, int grow = 6);
void invalidate_content();
bool in_rect(const RECT& r, LPARAM lp);

// fields
HWND make_edit(int id, DWORD style, const wchar_t* placeholder, int limit);
void place_field(const Field& f);
void paint_field(HDC dc, const ui::Palette& Pl, const Field& f, bool error = false);
void place(int id, const Box& b);
int popup_menu(const Field& f, const std::vector<std::wstring>& items, const std::wstring& empty,
               const std::wstring& extra = L"");
void card(HDC dc, const ui::Palette& Pl, const Box& b);
void card_title(HDC dc, const ui::Palette& Pl, const ui::Fonts& F, int x, int y, wchar_t icon, const std::wstring& t);
void pill(HDC dc, RECT r, COLORREF bg, COLORREF fg, const std::wstring& t, HFONT f, bool dot);
void set_info(InfoKind k, const std::wstring& t);
void show_page(Page p);
void apply_theme();   // after the theme mode changed

namespace home {
void create(HWND hwnd);
void layout();
void set_visible(bool on);
void restyle();   // colours that depend on the theme (controls on the gradient)
void paint(HDC dc, const ui::Palette& Pl, const ui::Fonts& F);
bool command(int id, int code);   // true if handled
bool mouse_move(LPARAM lp);
bool click(LPARAM lp);
bool set_cursor(LPARAM lp);
void refresh();   // history / sessions changed
const Field* field(int id);
bool on_enter(HWND focus);
void send_files_to(int key, const std::string& peerId);   // host: file dialog, then send to that viewer
}

namespace access {   // access_page.cpp: default permissions + who is connected now
void create(HWND hwnd);
void layout();
void set_visible(bool on);
void paint(HDC dc, const ui::Palette& Pl, const ui::Fonts& F);
bool command(int id, int code);
bool click(LPARAM lp);
bool set_cursor(LPARAM lp);
void refresh();   // sessions changed
}

namespace settings {
void create(HWND hwnd);
void layout();
void set_visible(bool on);
void paint(HDC dc, const ui::Palette& Pl, const ui::Fonts& F);
bool command(int id, int code);
bool click(LPARAM lp);
bool set_cursor(LPARAM lp);
void update_changed();
void license_changed();
void show_update_state(HWND button, const std::wstring& idleText);   // spinner, progress, label
const Field* field(int id);
bool on_enter(HWND focus);
}

}  // namespace mw
