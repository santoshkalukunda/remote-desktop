// Settings page: Unattended access, Appearance & startup, Updates, RemoteDesk Pro.
// Server addresses are not here on purpose: they are ours (core/server_config.h).
#include "screens/main_window.h"
#include "app/license.h"
#include "app/updater.h"
#include "app/service.h"
#include <shellapi.h>

namespace mw::settings {

constexpr int CARD_W = (CWIDTH - 16) / 2, CARD_H = 252, IW = CARD_W - 48;
constexpr Box C_PW{ CX, 80, CARD_W, CARD_H };
constexpr Box C_GENERAL{ CX + CARD_W + 16, 80, CARD_W, CARD_H };
constexpr Box C_UPDATE{ CX, 80 + CARD_H + 16, CARD_W, CARD_H };
constexpr Box C_PRO{ CX + CARD_W + 16, 80 + CARD_H + 16, CARD_W, CARD_H };

constexpr int PX = C_PW.x + 24, PY = C_PW.y;
constexpr int GX = C_GENERAL.x + 24, GY = C_GENERAL.y;
constexpr int UX = C_UPDATE.x + 24, UY = C_UPDATE.y;
constexpr int QX = C_PRO.x + 24, QY = C_PRO.y;

static const Field FIELDS[] = {
    { IDC_PW, { PX, PY + 110, IW, 44 }, 0, false },
    { IDC_LICENSE_KEY, { QX, QY + 110, IW, 44 }, 0, false },
};
constexpr Box BOX_PW_SAVE{ PX, PY + 170, (IW - 12) / 2, 44 };
constexpr Box BOX_PW_OFF{ PX + (IW - 12) / 2 + 12, PY + 170, (IW - 12) / 2, 44 };
constexpr Box BOX_THEME{ GX, GY + 96, IW, 44 };
constexpr Box BOX_STARTUP{ GX, GY + 152, IW, 32 };
constexpr Box BOX_LOCKSCREEN{ GX, GY + 196, IW, 40 };
constexpr Box BOX_WOL{ GX + IW - 210, GY + 198, 210, 36 };
constexpr Box BOX_UPDATE_BTN{ UX, UY + 186, IW, 44 };
constexpr Box BOX_LICENSE_BTN{ QX, QY + 170, IW, 44 };
constexpr Box BOX_LICENSE_OFF{ QX, QY + 186, IW, 44 };
static const int IDS[] = { IDC_PW, IDC_PW_SAVE, IDC_PW_OFF, IDC_THEME, IDC_STARTUP, IDC_LOCKSCREEN, IDC_WOL, IDC_UPDATE_BTN,
                           IDC_LICENSE_KEY, IDC_LICENSE_BTN, IDC_LICENSE_OFF };

const Field* field(int id) {
    for (const Field& f : FIELDS) if (f.id == id) return &f;
    return nullptr;
}

// Label, spinner and progress fill of an update button for the current update state.
void show_update_state(HWND b, const std::wstring& idleText) {
    const UpdateStatus u = update_status();
    std::wstring t = idleText;
    bool busy = false;
    int progress = -1;
    switch (u.state) {
    case UpdateState::Checking: t = L"Checking for updates…"; busy = true; break;
    case UpdateState::Available: t = L"Update to " + widen(u.latest); break;
    case UpdateState::Downloading: t = L"Downloading  " + std::to_wstring(u.percent) + L"%"; busy = true; progress = u.percent; break;
    case UpdateState::Installing: t = L"Installing…"; busy = true; progress = 100; break;
    default: break;
    }
    SetWindowTextW(b, t.c_str());
    ui::set_icon(b, u.state == UpdateState::Available ? ui::ico::Download : ui::ico::Refresh);
    ui::set_busy(b, busy);
    ui::set_progress(b, progress);
}

static void update_button() {
    show_update_state(ctl(IDC_UPDATE_BTN), L"Check for updates");
    EnableWindow(ctl(IDC_UPDATE_BTN), !is_packaged());
}

static void pw_buttons() {
    const bool on = password_is_set();
    SetWindowTextW(ctl(IDC_PW_SAVE), on ? L"Change" : L"Turn on");
    if (S.page == PAGE_SETTINGS) ShowWindow(ctl(IDC_PW_OFF), on ? SW_SHOWNA : SW_HIDE);
}

// Pro card: key field + Activate while unlicensed, details + Deactivate once licensed.
static void license_controls() {
    const LicenseInfo L = license_info();
    const bool vis = S.page == PAGE_SETTINGS;
    ShowWindow(ctl(IDC_LICENSE_KEY), vis && !L.active ? SW_SHOWNA : SW_HIDE);
    ShowWindow(ctl(IDC_LICENSE_BTN), vis && !L.active ? SW_SHOWNA : SW_HIDE);
    ShowWindow(ctl(IDC_LICENSE_OFF), vis && L.active ? SW_SHOWNA : SW_HIDE);
    ui::set_busy(ctl(IDC_LICENSE_BTN), L.busy);
    SetWindowTextW(ctl(IDC_LICENSE_BTN), L.busy ? L"Activating…" : L"Activate");
    ui::set_busy(ctl(IDC_LICENSE_OFF), L.busy);
    SetWindowTextW(ctl(IDC_LICENSE_OFF), L.busy ? L"Deactivating…" : L"Deactivate on this PC");
}

void create(HWND hwnd) {
    using ui::Kind;
    make_edit(IDC_PW, ES_PASSWORD, L"At least 8 characters", 128);
    ui::make_control(hwnd, IDC_PW_SAVE, Kind::Primary, L"Turn on", ui::ico::Lock);
    ui::make_control(hwnd, IDC_PW_OFF, Kind::Danger, L"Turn off", ui::ico::Close);
    HWND theme = ui::make_control(hwnd, IDC_THEME, Kind::Segmented, L"");
    ui::set_options(theme, { { ui::ico::System, L"System" }, { ui::ico::Sun, L"Light" }, { ui::ico::Moon, L"Dark" } });
    ui::set_selected(theme, (int)ui::mode());
    ui::make_control(hwnd, IDC_STARTUP, Kind::Toggle, L"Start with Windows");
    ui::make_control(hwnd, IDC_LOCKSCREEN, Kind::Secondary, L"Enable lock-screen access", ui::ico::Lock);
    ui::make_control(hwnd, IDC_WOL, Kind::Link, L"Set up Wake-on-LAN", ui::ico::Power);
    ui::make_control(hwnd, IDC_UPDATE_BTN, Kind::Primary, L"Check for updates", ui::ico::Refresh);
    make_edit(IDC_LICENSE_KEY, ES_UPPERCASE, L"RDPRO-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX", 64);
    ui::make_control(hwnd, IDC_LICENSE_BTN, Kind::Primary, L"Activate", ui::ico::Key);
    ui::make_control(hwnd, IDC_LICENSE_OFF, Kind::Secondary, L"Deactivate on this PC", ui::ico::Close);
    ui::set_checked(ctl(IDC_STARTUP), start_with_windows_enabled());
    update_button();
}

void layout() {
    for (const Field& f : FIELDS) place_field(f);
    place(IDC_PW_SAVE, BOX_PW_SAVE);
    place(IDC_PW_OFF, BOX_PW_OFF);
    place(IDC_THEME, BOX_THEME);
    place(IDC_STARTUP, BOX_STARTUP);
    place(IDC_LOCKSCREEN, BOX_LOCKSCREEN);
    place(IDC_WOL, BOX_WOL);
    place(IDC_UPDATE_BTN, BOX_UPDATE_BTN);
    place(IDC_LICENSE_BTN, BOX_LICENSE_BTN);
    place(IDC_LICENSE_OFF, BOX_LICENSE_OFF);
}

// Lock-screen access: the service is running (on), can be enabled (button), or not in this build.
static bool g_serviceOn = false;
static void lockscreen_controls() {
    g_serviceOn = service_running();
    const bool show = S.page == PAGE_SETTINGS && !g_serviceOn && !is_packaged();
    ShowWindow(ctl(IDC_LOCKSCREEN), show ? SW_SHOWNA : SW_HIDE);
    // Waking it only helps when it can then be reached at the sign-in screen: with the service.
    ShowWindow(ctl(IDC_WOL), S.page == PAGE_SETTINGS && g_serviceOn ? SW_SHOWNA : SW_HIDE);
    const UpdateState u = update_status().state;
    const bool busy = u == UpdateState::Downloading || u == UpdateState::Installing;
    ui::set_busy(ctl(IDC_LOCKSCREEN), busy);
    SetWindowTextW(ctl(IDC_LOCKSCREEN), busy ? L"Starting setup\u2026" : L"Enable lock-screen access");
}

void set_visible(bool on) {
    for (int id : IDS) ShowWindow(ctl(id), on ? SW_SHOWNA : SW_HIDE);
    if (on) lockscreen_controls();
    if (on) { pw_buttons(); license_controls(); }
}

void paint(HDC dc, const ui::Palette& Pl, const ui::Fonts& F) {
    ui::text(dc, L"Settings", ui::rc(CX, 26, 400, 34, S.dpi), F.heading, Pl.text);

    // Unattended access
    card(dc, Pl, C_PW);
    card_title(dc, Pl, F, PX, PY + 22, ui::ico::Key, L"Unattended access");
    {
        const bool on = password_is_set();
        RECT r = ui::rc(PX + IW - 56, PY + 29, 56, 22, S.dpi);
        pill(dc, r, on ? Pl.successSoft : (Pl.dark ? Pl.btnHover : Pl.divider), on ? Pl.success : Pl.muted, on ? L"ON" : L"OFF",
             F.smallBold, false);
    }
    ui::text(dc, L"Connect to this PC with a password, no approval needed.", ui::rc(PX, PY + 74, IW, 20, S.dpi), F.small,
             Pl.muted);
    const bool pwError = !S.errPw.empty();
    paint_field(dc, Pl, FIELDS[0], pwError);
    if (!S.errPw.empty() || !S.pwNote.empty())
        ui::text(dc, pwError ? S.errPw : S.pwNote, ui::rc(PX, PY + 226, IW, 18, S.dpi), F.small, pwError ? Pl.danger : Pl.success);

    // Appearance & startup
    card(dc, Pl, C_GENERAL);
    card_title(dc, Pl, F, GX, GY + 22, ui::ico::Palette, L"Appearance & startup");
    ui::text(dc, L"Theme", ui::rc(GX, GY + 74, IW, 18, S.dpi), F.smallBold, Pl.muted);
    if (g_serviceOn)
        ui::text(dc, L"\u2713  Lock-screen access is on", ui::rc(GX, GY + 204, IW - 220, 24, S.dpi), F.smallBold, Pl.success);
    else if (is_packaged())
        ui::text(dc, L"Lock-screen access needs the setup.exe version", ui::rc(GX, GY + 204, IW, 24, S.dpi), F.small, Pl.faint);

    // Updates
    card(dc, Pl, C_UPDATE);
    card_title(dc, Pl, F, UX, UY + 22, ui::ico::Download, L"Updates");
    const UpdateStatus u = update_status();
    ui::text(dc, L"Installed version " + widen(app_version()), ui::rc(UX, UY + 76, IW, 22, S.dpi), F.bodyBold, Pl.text);
    std::wstring st;
    COLORREF sc = Pl.muted;
    if (is_packaged()) st = L"Updated automatically by the Microsoft Store";
    else switch (u.state) {
    case UpdateState::UpToDate: st = L"You're up to date"; sc = Pl.success; break;
    case UpdateState::Available: st = L"Version " + widen(u.latest) + L" is available"; sc = Pl.accentText; break;
    case UpdateState::Failed: st = u.error; sc = Pl.danger; break;
    case UpdateState::Downloading: st = L"Downloading and verifying…"; sc = Pl.accentText; break;
    case UpdateState::Installing: st = L"Starting the installer…"; sc = Pl.accentText; break;
    default: st = L"Checks automatically when RemoteDesk starts"; break;
    }
    ui::text(dc, st, ui::rc(UX, UY + 102, IW, 20, S.dpi), F.small, sc);
    if (u.state == UpdateState::Available && !u.notes.empty())
        ui::text(dc, u.notes, ui::rc(UX, UY + 124, IW, 40, S.dpi), F.small, Pl.faint,
                 DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);

    // RemoteDesk Pro
    card(dc, Pl, C_PRO);
    card_title(dc, Pl, F, QX, QY + 22, 0xE735 /* star */, L"RemoteDesk Pro");
    const LicenseInfo L = license_info();
    {
        RECT r = ui::rc(QX + IW - 72, QY + 29, 72, 22, S.dpi);
        pill(dc, r, L.active ? Pl.successSoft : (Pl.dark ? Pl.btnHover : Pl.divider), L.active ? Pl.success : Pl.muted,
             L.active ? L"ACTIVE" : L"FREE", F.smallBold, false);
    }
    if (L.active) {
        ui::text(dc, L.name.empty() ? std::wstring(L"Licensed") : L"Licensed to " + L.name, ui::rc(QX, QY + 74, IW, 22, S.dpi),
                 F.bodyBold, Pl.text);
        ui::text(dc, (L.expires.empty() ? std::wstring(L"Never expires") : L"Valid until " + widen(L.expires)) + L"  ·  this PC",
                 ui::rc(QX, QY + 100, IW, 20, S.dpi), F.small, Pl.success);
        ui::text(dc, L"Full screen, screenshots, recording, file transfer", ui::rc(QX, QY + 124, IW, 20, S.dpi), F.small, Pl.muted);
        ui::text(dc, L"Key " + widen(L.serial), ui::rc(QX, QY + 146, IW, 20, S.dpi), F.small, Pl.faint);
    } else {
        ui::text(dc, L"Full screen, screenshots, recording and file transfer.", ui::rc(QX, QY + 74, IW, 20, S.dpi), F.small,
                 Pl.muted);
        paint_field(dc, Pl, FIELDS[1], !L.error.empty());
    }
    if (!L.error.empty())
        ui::text(dc, L.error, ui::rc(QX, QY + 226, IW, 18, S.dpi), F.small, Pl.danger);
}

// ---- actions ----
static void invalidate_card(const Box& b) { invalidate(b, 2); }

static void on_set_password() {
    std::string pw = narrow(get_text(IDC_PW));
    S.pwNote.clear();
    S.errPw = pw.size() < 8 ? (pw.empty() ? L"Type a password first" : L"Use at least 8 characters") : L"";
    invalidate_card(C_PW);
    if (!S.errPw.empty()) { SetFocus(ctl(IDC_PW)); return; }
    set_password(pw);
    SetWindowTextW(ctl(IDC_PW), L"");
    S.pwNote = L"Password saved — unattended access is on";
    pw_buttons();
}

static void on_password_off() {
    set_password("");
    S.errPw.clear();
    S.pwNote = L"Unattended access is off";
    pw_buttons();
    invalidate_card(C_PW);
}

static void on_update_button() {
    if (update_status().state == UpdateState::Available) updater_install();
    else updater_check(true);
}

static void on_activate() {
    const std::string key = trim(narrow(get_text(IDC_LICENSE_KEY)));
    if (key.empty()) { SetFocus(ctl(IDC_LICENSE_KEY)); return; }
    license_activate(key);
}

void update_changed() {
    update_button();
    lockscreen_controls();
    invalidate_card(C_GENERAL);
    invalidate_card(C_UPDATE);
}

void license_changed() {
    if (license_info().active) SetWindowTextW(ctl(IDC_LICENSE_KEY), L"");
    license_controls();
    invalidate_card(C_PRO);
}

bool command(int id, int code) {
    switch (id) {
    case IDC_PW_SAVE: on_set_password(); return true;
    case IDC_PW_OFF: on_password_off(); return true;
    case IDC_UPDATE_BTN: on_update_button(); return true;
    case IDC_LICENSE_BTN: on_activate(); return true;
    case IDC_LICENSE_OFF: license_deactivate(); return true;
    case IDC_LOCKSCREEN: {
        // Reinstalls this version (latest from our server) for all users with the service.
        // Windows asks for administrator approval here; the app restarts when it's done.
        const int r = MessageBoxW(g_wnd,
            L"RemoteDesk will reinstall itself for all users with the RemoteDesk service, so people you allow can "
            L"reach this computer's lock and sign-in screens (with your unattended password or your approval).\n\n"
            L"Windows will ask for administrator approval. RemoteDesk restarts when it's done.",
            L"Enable lock-screen access", MB_OKCANCEL | MB_ICONINFORMATION);
        if (r == IDOK) updater_install(true);
        return true;
    }
    case IDC_WOL: {
        const int r = MessageBoxW(g_wnd,
            L"So that RemoteDesk can switch this computer on from another one, this will:\n\n"
            L"\u2022 turn on the wake settings of its wired network cards (including USB adapters) and let them wake the PC\n"
            L"\u2022 turn off their power saving (Energy-Efficient / Green Ethernet) that can cut the port while it sleeps\n"
            L"\u2022 keep networking on during Modern Standby, and turn off Windows Fast Startup\n\n"
            L"This PC also needs:\n"
            L"\u2022 a network cable (a USB network adapter is fine; Wi-Fi can't wake it)\n"
            L"\u2022 in the BIOS/UEFI: Wake on LAN on; Deep Sleep, ErP and energy-saving modes off\n"
            L"\u2022 to be plugged into power, and put to Sleep rather than Shut down (most laptops switch the network "
            L"card off when shut down)\n\n"
            L"Restart the PC once afterwards. The full guide is in Start menu \u203a RemoteDesk \u203a Wake-on-LAN guide.",
            L"Set up Wake-on-LAN", MB_OKCANCEL | MB_ICONINFORMATION);
        if (r != IDOK) return true;
        if (agent_mode()) {
            agent_send({ "SETUPWOL" });   // the service's agent does it (it has the rights); the report comes back
        } else {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            ShellExecuteW(g_wnd, L"runas", exe, L"--setup-wol", nullptr, SW_SHOWNORMAL);
        }
        return true;
    }
    case IDC_STARTUP: {
        bool want = ui::checked(ctl(IDC_STARTUP));
        if (!set_start_with_windows(want)) ui::set_checked(ctl(IDC_STARTUP), !want);
        return true;
    }
    case IDC_THEME: {
        int m = ui::selected(ctl(IDC_THEME));
        ui::set_mode((ui::Mode)m);
        set_setting(L"Theme", std::to_string(m));
        apply_theme();
        return true;
    }
    case IDC_PW:
        if (code == EN_CHANGE && (!S.errPw.empty() || !S.pwNote.empty())) {
            S.errPw.clear();
            S.pwNote.clear();
            invalidate_card(C_PW);
        }
        return true;
    case IDC_LICENSE_KEY: return true;
    }
    return false;
}

bool on_enter(HWND focus) {
    if (focus == ctl(IDC_PW)) on_set_password();
    else if (focus == ctl(IDC_LICENSE_KEY)) on_activate();
    else return false;
    return true;
}

bool click(LPARAM lp) {
    for (const Field& f : FIELDS)
        if (IsWindowVisible(ctl(f.id)) && in_rect(R(f.box), lp)) {
            SetFocus(ctl(f.id));
            SendMessageW(ctl(f.id), EM_SETSEL, 0x7fffffff, 0x7fffffff);
            return true;
        }
    return false;
}

bool set_cursor(LPARAM lp) {
    for (const Field& f : FIELDS)
        if (IsWindowVisible(ctl(f.id)) && in_rect(R(f.box), lp)) { SetCursor(LoadCursorW(nullptr, IDC_IBEAM)); return true; }
    return false;
}

}  // namespace mw::settings
