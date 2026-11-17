// RemoteDesk.exe --agent: started by the service as SYSTEM in the console session.
// It is this computer's host: it registers the address with the relay and serves incoming
// sessions, following the input desktop onto the lock screen, the sign-in screen and UAC.
// The app the user sees connects over \\.\pipe\RemoteDeskAgent-<session> to show the address,
// approve requests, list and manage sessions, and change the password; it pushes its Pro
// license here so licensed features work from the host side too. Nobody signed in: password
// sessions still work; requests that need approval are declined.
#include "core/app.h"
#include "core/pipe.h"
#include "app/service.h"
#include "app/license.h"
#include "app/settings_internal.h"
#include "ui/ui.h"
#include <sddl.h>
#include <condition_variable>
#include <map>
#include <thread>

bool g_agent = false;

static std::mutex g_uiMx;          // guards g_ui (the connected app, if any)
static PipeConn* g_ui = nullptr;
static WPARAM g_relayW = RS_CONNECTING;
static LPARAM g_relayL = 0;

static bool to_ui(const std::vector<std::string>& f) {
    std::lock_guard<std::mutex> lk(g_uiMx);
    return g_ui && g_ui->write(f);
}

// ---- approvals: asked in the app, answered over the pipe ----
static std::mutex g_askMx;
static std::condition_variable g_askCv;
static std::map<int, int> g_answers;
static int g_nextAsk = 1;
static int g_uiGen = 0;   // bumps when the app disconnects: pending questions end

int agent_ask(const std::string& viewerId, const std::string& code) {
    int id, gen;
    {
        std::lock_guard<std::mutex> lk(g_askMx);
        id = g_nextAsk++;
        gen = g_uiGen;
    }
    if (!to_ui({ "ASK", std::to_string(id), viewerId, code })) return -2;
    std::unique_lock<std::mutex> lk(g_askMx);
    g_askCv.wait_for(lk, std::chrono::seconds(75), [&] { return g_answers.count(id) || g_uiGen != gen; });
    if (!g_answers.count(id)) return g_uiGen != gen ? -2 : -1;
    const int r = g_answers[id];
    g_answers.erase(id);
    return r;
}

static void send_sessions() {
    std::vector<std::string> f{ "SESSIONS" };
    const ULONGLONG now = GetTickCount64();
    for (const SessionInfo& s : sessions()) {
        if (!s.incoming) continue;
        f.push_back(std::to_string(s.key));
        f.push_back(s.peerId);
        f.push_back(std::to_string(now - s.since));
        f.push_back(std::to_string(s.perms));
        f.push_back(s.recording ? "1" : "0");
        f.push_back(narrow(session_transfer_status(s.key)));
        f.push_back(std::to_string(s.call));
        f.push_back(s.muted ? "1" : "0");
    }
    to_ui(f);
}

static void send_state() {
    to_ui({ "ID", my_id() });
    to_ui({ "PW", password_is_set() ? "1" : "0" });
    to_ui({ "RELAY", std::to_string(g_relayW), std::to_string(g_relayL) });
    send_sessions();
}

// ---- commands from the app ----
static void handle(const std::vector<std::string>& f) {
    if (f.empty()) return;
    const std::string& c = f[0];
    auto num = [&](size_t i) { return i < f.size() ? atoi(f[i].c_str()) : 0; };
    if (c == "HELLO" && f.size() >= 5) {
        // First contact after the service was installed: keep the address and password people
        // already know instead of the agent's own.
        if (setting(L"Adopted").empty()) {
            adopt_identity(f[1], f[2], f[3]);
            set_setting(L"Adopted", "1");
            relay_wake();   // register under the adopted address
        }
        // What the password grants is set on the app's Access page: take it every time the app
        // connects (not only the first time), so a newer app's options (e.g. sound) reach us too.
        if (!f[4].empty()) set_default_perms(true, (uint8_t)num(4));
        send_state();
    } else if (c == "ANSWER" && f.size() >= 3) {
        std::lock_guard<std::mutex> lk(g_askMx);
        g_answers[num(1)] = num(2);
        g_askCv.notify_all();
    } else if (c == "END") {
        session_end(num(1));
    } else if (c == "PERMS") {
        session_set_perms(num(1), (uint8_t)num(2));
    } else if (c == "SEND" && f.size() >= 3) {
        std::vector<std::wstring> paths;
        for (size_t i = 2; i < f.size(); ++i) paths.push_back(widen(f[i]));
        if (!session_send_files(num(1), paths)) post_info(INFO_ERROR, L"That session has ended.");
    } else if (c == "SETPW" && f.size() >= 2) {
        set_password(f[1]);
        to_ui({ "PW", password_is_set() ? "1" : "0" });
    } else if (c == "CALL") {
        session_call(num(1), num(2));
    } else if (c == "SETUPWOL") {
        std::thread([] { to_ui({ "WOLRESULT", narrow(setup_wake_on_lan()) }); }).detach();
    } else if (c == "DEFPERMS") {
        set_default_perms(true, (uint8_t)num(1));
    } else if (c == "LICENSE" && f.size() >= 3) {
        set_setting(L"LicenseText", f[1]);
        set_setting(L"LicenseSig", f[2]);
        license_load();
    }
}

// Only the app in this session (any signed-in user here), never from the network.
static void pipe_thread() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::wstring name = agent_pipe_name(session);
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr);
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        HANDLE h = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
                                    65536, 0, &sa);
        if (h == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD n;
        bool ok = ConnectNamedPipe(h, &ov) ? true
                : GetLastError() == ERROR_PIPE_CONNECTED ? true
                : GetLastError() == ERROR_IO_PENDING && GetOverlappedResult(h, &ov, &n, TRUE);
        ULONG clientSession = 0xFFFFFFFF;
        if (ok && (!GetNamedPipeClientSessionId(h, &clientSession) || clientSession != session)) ok = false;
        if (!ok) { CloseHandle(h); continue; }
        PipeConn conn(h);
        {
            std::lock_guard<std::mutex> lk(g_uiMx);
            g_ui = &conn;
        }
        std::vector<std::string> f;
        while (conn.read(f)) handle(f);
        {
            std::lock_guard<std::mutex> lk(g_uiMx);
            g_ui = nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(g_askMx);
            ++g_uiGen;
            g_askCv.notify_all();
        }
    }
}

static LRESULT CALLBACK agent_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_RELAY:
        g_relayW = wp;
        g_relayL = lp;
        to_ui({ "RELAY", std::to_string(wp), std::to_string(lp) });
        return 0;
    case WM_APP_ID:
        to_ui({ "ID", my_id() });
        return 0;
    case WM_APP_SESSIONS:
        send_sessions();
        return 0;
    case WM_APP_INFO: {
        auto* t = (std::wstring*)lp;
        to_ui({ "INFO", std::to_string(wp), narrow(*t) });
        delete t;
        return 0;
    }
    case WM_APP_SETCLIP: {
        auto* t = (std::wstring*)lp;
        clipboard_set(h, *t);
        delete t;
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int agent_main(HINSTANCE hi) {
    g_agent = true;
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    CreateMutexW(nullptr, TRUE, (L"Global\\RemoteDeskAgent-" + std::to_wstring(session)).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    load_settings();   // the agent's own: %SystemRoot%\System32\config\systemprofile\AppData\Roaming\RemoteDesk
    ui::init(hi);
    license_load();

    WNDCLASSW wc{};
    wc.lpfnWndProc = agent_proc;
    wc.hInstance = hi;
    wc.lpszClassName = L"RemoteDeskAgent";
    RegisterClassW(&wc);
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"RemoteDesk agent", WS_POPUP, 0, 0, 0, 0, nullptr,
                                nullptr, hi, nullptr);
    std::thread(pipe_thread).detach();
    relay_start();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ExitProcess(0);
}
