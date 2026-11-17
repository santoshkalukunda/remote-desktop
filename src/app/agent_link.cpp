// The app's link to the agent (see app/service.cpp). With the service running, the agent is this
// computer's host and this app is its window: it shows the agent's address and relay status,
// asks the person here to approve requests, lists and manages the agent's sessions, and passes on
// password, permission and license changes. If no agent answers (e.g. a Remote Desktop session,
// or the service is stopped), the app hosts by itself as before.
#include "core/app.h"
#include "core/pipe.h"
#include "app/service.h"
#include "app/license.h"
#include "app/settings_internal.h"
#include <aclapi.h>
#include <thread>

static std::atomic<bool> g_mode{ false };
static std::mutex g_linkMx;
static PipeConn* g_link = nullptr;

bool agent_mode() { return g_mode; }

void agent_send(const std::vector<std::string>& f) {
    std::lock_guard<std::mutex> lk(g_linkMx);
    if (g_link) g_link->write(f);
}

void agent_push_license() {
    if (!g_mode) return;
    agent_send({ "LICENSE", setting(L"LicenseText"), setting(L"LicenseSig") });
}

// The pipe must be served by the agent, not by some program of the signed-in user pretending to be
// it: it has to be in this session and owned by SYSTEM (or Administrators, the default owner of
// SYSTEM's objects). An ordinary user can't create a pipe owned by either. (A user can't look up a
// SYSTEM process's file name, so that can't be the check.)
static bool genuine_agent(HANDLE pipe) {
    ULONG sid = 0xFFFFFFFF;
    DWORD mine = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mine);
    if (!GetNamedPipeServerSessionId(pipe, &sid) || sid != mine) return false;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS)
        return false;
    const bool ok = owner && (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
    LocalFree(sd);
    return ok;
}

static void on_message(const std::vector<std::string>& f) {
    if (f.empty()) return;
    const std::string& c = f[0];
    if (c == "ID" && f.size() >= 2) {
        proxy_id(f[1]);
        PostMessageW(g_mainWnd, WM_APP_ID, 0, 0);
    } else if (c == "PW" && f.size() >= 2) {
        proxy_password(f[1] == "1");
        PostMessageW(g_mainWnd, WM_APP_ID, 0, 0);   // repaints the Settings card too
    } else if (c == "RELAY" && f.size() >= 3) {
        PostMessageW(g_mainWnd, WM_APP_RELAY, (WPARAM)strtoull(f[1].c_str(), nullptr, 10), (LPARAM)strtoll(f[2].c_str(), nullptr, 10));
    } else if (c == "INFO" && f.size() >= 3) {
        post_info((InfoKind)atoi(f[1].c_str()), widen(f[2]));
    } else if (c == "SESSIONS") {
        std::vector<SessionInfo> list;
        const ULONGLONG now = GetTickCount64();
        for (size_t i = 1; i + 8 <= f.size(); i += 8) {   // 8 fields per session
            SessionInfo s{ AGENT_KEY_BASE + atoi(f[i].c_str()), f[i + 1], true, nullptr, INVALID_SOCKET, now - strtoull(f[i + 2].c_str(), nullptr, 10) };
            s.perms = (uint8_t)atoi(f[i + 3].c_str());
            s.recording = f[i + 4] == "1";
            s.xfer = widen(f[i + 5]);
            s.call = (uint8_t)atoi(f[i + 6].c_str());
            s.muted = f[i + 7] == "1";
            list.push_back(s);
        }
        sessions_set_agent(list);
    } else if (c == "WOLRESULT" && f.size() >= 2) {
        auto* t = new std::wstring(widen(f[1]));
        if (!PostMessageW(g_mainWnd, WM_APP_NOTE, 0, (LPARAM)t)) delete t;
    } else if (c == "ASK" && f.size() >= 4) {
        // The approval prompt is ours (the person at this PC); the answer goes back to the agent.
        std::thread([id = f[1], viewer = f[2], code = f[3]] {
            const int r = ask_access(viewer, code);
            agent_send({ "ANSWER", id, std::to_string(r) });
        }).detach();
    }
}

static void link_thread() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::wstring name = agent_pipe_name(session);
    const ULONGLONG start = GetTickCount64();
    bool everConnected = false;
    for (;;) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE && !genuine_agent(h)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
        if (h == INVALID_HANDLE_VALUE) {
            if (!everConnected && GetTickCount64() - start > 15000) {
                // No agent for this session: host here, as without the service.
                g_mode = false;
                settings_proxy(false);
                PostMessageW(g_mainWnd, WM_APP_ID, 0, 0);
                relay_start();
                return;
            }
            Sleep(1000);
            continue;
        }
        everConnected = true;
        PipeConn link(h);
        {
            std::lock_guard<std::mutex> lk(g_linkMx);
            g_link = &link;
        }
        std::string id, salt, hash;
        identity(id, salt, hash);
        agent_send({ "HELLO", id, salt, hash, std::to_string(default_perms(true)) });
        agent_push_license();
        std::vector<std::string> f;
        while (link.read(f)) on_message(f);
        {
            std::lock_guard<std::mutex> lk(g_linkMx);
            g_link = nullptr;
        }
        sessions_set_agent({});
        PostMessageW(g_mainWnd, WM_APP_RELAY, RS_CONNECTING, 0);   // the service restarts it
        Sleep(1000);
    }
}

bool agent_link_start() {
    if (!service_running()) return false;
    g_mode = true;
    settings_proxy(true);
    PostMessageW(g_mainWnd, WM_APP_RELAY, RS_CONNECTING, 0);
    std::thread(link_thread).detach();
    return true;
}
