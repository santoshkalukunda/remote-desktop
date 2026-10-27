#pragma once
#include "common.h"

// Messages posted to the main window from worker threads.
// lParam carries a heap-allocated std::wstring* that the receiver deletes.
constexpr UINT WM_APP_STATUS   = WM_APP + 1;   // relay status line
constexpr UINT WM_APP_SETCLIP  = WM_APP + 2;   // set local clipboard text
constexpr UINT WM_APP_SESSIONS = WM_APP + 3;   // wParam = active incoming sessions
constexpr UINT WM_APP_ID       = WM_APP + 4;   // our ID changed
constexpr UINT WM_APP_INFO     = WM_APP + 5;   // outgoing-connection status line

extern HWND g_mainWnd;

// Settings (implemented in main.cpp, thread-safe).
std::string my_id();
bool relay_address(std::string& host, uint16_t& port);
bool password_matches(const std::string& pw);   // false if none set
bool password_is_set();

void post_text(UINT msg, const std::wstring& text);

// Opens a socket to the relay and sends the first command line.
SOCKET relay_open(const std::string& command);

// host.cpp: serve one incoming viewer. Blocks until the session ends.
void host_session(std::string sid);
void disconnect_all_incoming();

// viewer.cpp: connect to a remote desk and show it. Blocks until closed.
void viewer_session(std::string targetId, std::string password);

// Version (effective date) of PRIVACY.txt. Must match PolicyDate in installer/RemoteDesk.iss;
// changing it asks every user to accept the policy again.
constexpr char POLICY_VERSION[] = "2026-10-26";
constexpr int IDR_PRIVACY = 2;   // PRIVACY.txt, embedded as RCDATA (app.rc)

// welcome.cpp: first-run privacy policy and permissions screen.
// `relay` comes in pre-filled; returns false if the user declined.
struct WelcomeChoice {
    std::string relay;
    bool startWithWindows = false;
};
bool run_welcome(HINSTANCE hi, WelcomeChoice& choice);

// startup.cpp: Microsoft Store (MSIX) integration.
bool is_packaged();
bool launched_at_startup();            // started by the "Start with Windows" task
bool set_start_with_windows(bool on);  // packaged only; false if Windows refused
