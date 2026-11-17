#pragma once
// Declarations shared across the app. Implementations:
//   app/settings.cpp  settings.ini, relay address, unattended password
//   app/relay.cpp     the always-on relay registration (control connection)
//   app/history.cpp   recent connections, saved passwords (DPAPI)
//   app/updater.cpp   update check / download / install
//   app/startup.cpp   Start with Windows
//   remote/*.cpp      host side, viewer side, list of active sessions
//   screens/*.cpp     main window and the first-run screen
#include "core/common.h"

// Messages posted to the main window from worker threads.
constexpr UINT WM_APP_RELAY    = WM_APP + 1;   // wParam = RelayState | (RelayReason << 8), lParam = retry delay ms
constexpr UINT WM_APP_SETCLIP  = WM_APP + 2;   // lParam = std::wstring* (receiver deletes): set local clipboard
constexpr UINT WM_APP_SESSIONS = WM_APP + 3;   // the list of active sessions changed
constexpr UINT WM_APP_ID       = WM_APP + 4;   // our ID changed
constexpr UINT WM_APP_INFO     = WM_APP + 5;   // wParam = InfoKind, lParam = std::wstring* (receiver deletes)
constexpr UINT WM_APP_HISTORY  = WM_APP + 6;   // recent connections changed
constexpr UINT WM_APP_UPDATE   = WM_APP + 7;   // update state changed (see app/updater.h)
constexpr UINT WM_APP_LICENSE  = WM_APP + 8;   // license state changed (see app/license.h)
constexpr UINT WM_APP_NOTE     = WM_APP + 9;   // lParam = std::wstring* (receiver deletes): show in a message box

enum RelayState { RS_CONNECTING, RS_ONLINE, RS_OFFLINE };
enum RelayReason { RR_NONE, RR_UNREACHABLE, RR_LOST, RR_REFUSED, RR_TAKEN };
enum InfoKind { INFO_HINT, INFO_BUSY, INFO_ERROR, INFO_OK };

extern HWND g_mainWnd;

// ---- settings (app/settings.cpp, thread-safe) ----
std::string my_id();
void new_id();
bool relay_address(std::string& host, uint16_t& port);
bool password_matches(const std::string& pw);   // false if none set
bool password_is_set();
void set_password(const std::string& pw);       // empty = unattended access off
std::string setting(const wchar_t* key);
void set_setting(const wchar_t* key, const std::string& value);
std::wstring data_dir();                        // %APPDATA%\RemoteDesk
uint8_t default_perms(bool password);           // Access page: what a password grants / an approval starts with
void set_default_perms(bool password, uint8_t perms);

// Status line on the Home page (any thread).
void post_info(InfoKind kind, const std::wstring& text);

// ---- relay (app/relay.cpp) ----
SOCKET relay_open(const std::string& command);   // connects and sends the first command line
void relay_start();
void relay_wake();                               // reconnect now (new address or "Retry")
// Ask the relay to wake an offline computer through another RemoteDesk PC on its network:
// number of helpers asked, 0 = none on its network is on, or one of these.
constexpr int WAKE_FAILED = -1, WAKE_UNKNOWN = -2, WAKE_ONLINE = -3, WAKE_INTERNET = -4;
int wake_request(const std::string& id);

// ---- sessions (remote/) ----
void host_session(std::string sid);              // serve one incoming viewer; blocks
int ask_access(const std::string& viewerId, const std::string& code);   // AccessPerm bits, or -1 = declined
void viewer_session(std::string targetId, std::string password, bool savedPassword);   // blocks until closed
bool viewer_connecting();        // an attempt is still dialing / waking / waiting for approval
void viewer_cancel_connect();    // stop those attempts (the Connect button's Cancel)
void viewers_restyle();                          // theme changed: repaint open viewer windows

class FileEngine;
struct HostSession;

struct SessionInfo {
    int key;
    std::string peerId;
    bool incoming;       // someone is viewing this PC
    HWND window;         // outgoing: the viewer window
    SOCKET sock;         // incoming: to disconnect
    ULONGLONG since;     // GetTickCount64 at start
    FileEngine* files = nullptr;   // incoming: to send files back (use session_send_files)
    bool recording = false;              // incoming: the viewer is recording this screen
    uint8_t perms = 0;                   // incoming: what the viewer may do now (AccessPerm bits)
    uint8_t call = 0;                    // incoming: voice call state (CallLink::State: 0 none, 1 calling, 2 ringing, 3 on a call)
    bool muted = false;                  // incoming: our microphone is muted in that call
    HostSession* host = nullptr;         // incoming: to change permissions (use session_set_perms)
    std::wstring xfer;                   // the agent's sessions: file transfer status line
};
int session_add(const SessionInfo& s);           // returns its key
void session_remove(int key);
std::vector<SessionInfo> sessions();
int session_find(const std::string& peer, bool incoming);
void sessions_set_agent(const std::vector<SessionInfo>& list);   // app with the service: the agent's sessions   // key of a live session with that peer, or 0
void session_focus(int key);
void session_end(int key);
void session_set_recording(int key, bool on);
bool session_send_files(int key, const std::vector<std::wstring>& paths);   // incoming sessions
bool session_set_perms(int key, uint8_t perms);
std::wstring session_transfer_status(int key);   // incoming: "Sending report.pdf · 45%", or ""   // incoming: grant or revoke, takes effect at once
void host_apply_perms(HostSession* hs, uint8_t perms);   // host.cpp; called by session_set_perms
// Voice calls with someone viewing this PC (Session window, prompts; through the agent too).
enum CallDo { CALL_DO_START = 1, CALL_DO_ACCEPT, CALL_DO_DECLINE, CALL_DO_HANGUP, CALL_DO_MUTE, CALL_DO_UNMUTE };
bool session_call(int key, int action);
void session_set_call(int key, uint8_t state, bool muted);   // host.cpp reports the call state
void host_call(HostSession* hs, int action);                 // host.cpp; called by session_call
void disconnect_all_incoming();

// Version (effective date) of PRIVACY.txt. Must match PolicyDate in installer/RemoteDesk.iss;
// changing it asks every user to accept the policy again.
constexpr char POLICY_VERSION[] = "2026-10-26";
constexpr int IDR_PRIVACY = 2;   // PRIVACY.txt, embedded as RCDATA (app.rc)

// ---- first-run screen (screens/welcome.cpp) ----
struct WelcomeChoice {
    bool startWithWindows = false;
};
bool run_welcome(HINSTANCE hi, WelcomeChoice& choice);

// ---- main window (screens/main_window.cpp) ----
HWND create_main_window(HINSTANCE hi, int show);

// ---- session window on the host (screens/session_panel.cpp): permissions, send files, disconnect ----
void show_session_panel(HWND owner, int key);
void session_panel_refresh();   // the session list changed (closes the window if its session ended)

// ---- calls (screens/call_prompt.cpp) ----
#include <functional>
HWND show_call_prompt(HWND owner, const std::wstring& who, std::function<void(bool)> answer);   // answer(accepted)
void close_call_prompt(HWND prompt);    // the caller hung up first
void call_prompts_refresh(HWND owner);  // main window: prompts for sessions ringing this PC
// Before starting or accepting a call: is the microphone usable? If Windows blocks it (privacy
// settings), explains and offers to open the settings page; if there's none, asks whether to go
// ahead listening only. True = go ahead with the call.
bool ensure_microphone(HWND owner);

// ---- "update available" prompt (screens/update_prompt.cpp) ----
void show_update_prompt(HWND owner);
HWND update_prompt_window();   // for the dialog keys in the message loop, or nullptr

// ---- Start with Windows (app/startup.cpp) ----
bool is_packaged();
bool launched_at_startup();            // started by Windows at sign-in
bool start_with_windows_enabled();
bool set_start_with_windows(bool on);  // true if the setting is now `on`
