// Every live session, in both directions, so the main window can list them:
// outgoing ones (this PC views another; any number at once, each in its own
// window) and incoming ones (someone views this PC).
#include "core/app.h"
#include "remote/files.h"
#include "app/service.h"

// Recursive: some calls into a session report straight back here (e.g. hanging up a call updates
// its state in this list) while the caller still holds the lock.
static std::recursive_mutex g_mx;
static std::vector<SessionInfo> g_list;
static int g_next = 1;
static std::vector<SessionInfo> g_agentList;   // app with the service: the agent's incoming sessions

void sessions_set_agent(const std::vector<SessionInfo>& list) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        g_agentList = list;
    }
    PostMessageW(g_mainWnd, WM_APP_SESSIONS, 0, 0);
}

static bool is_agent_key(int key) { return key >= AGENT_KEY_BASE; }

int session_add(const SessionInfo& s) {
    int key;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        key = g_next++;
        g_list.push_back(s);
        g_list.back().key = key;
    }
    PostMessageW(g_mainWnd, WM_APP_SESSIONS, 0, 0);
    return key;
}

void session_remove(int key) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        g_list.erase(std::remove_if(g_list.begin(), g_list.end(), [key](const SessionInfo& s) { return s.key == key; }),
                     g_list.end());
    }
    PostMessageW(g_mainWnd, WM_APP_SESSIONS, 0, 0);
}

std::vector<SessionInfo> sessions() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::vector<SessionInfo> v = g_list;
    v.insert(v.end(), g_agentList.begin(), g_agentList.end());
    return v;
}

static bool find(int key, SessionInfo& out) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (const SessionInfo& s : g_list)
        if (s.key == key) { out = s; return true; }
    for (const SessionInfo& s : g_agentList)
        if (s.key == key) { out = s; return true; }
    return false;
}

void session_focus(int key) {
    SessionInfo s;
    if (!find(key, s) || !s.window) return;
    if (IsIconic(s.window)) ShowWindow(s.window, SW_RESTORE);
    SetForegroundWindow(s.window);
}

void session_end(int key) {
    if (is_agent_key(key)) { agent_send({ "END", std::to_string(key - AGENT_KEY_BASE) }); return; }
    SessionInfo s;
    if (!find(key, s)) return;
    if (s.incoming) shutdown(s.sock, SD_BOTH);
    else if (s.window) PostMessageW(s.window, WM_CLOSE, 0, 0);
}

void disconnect_all_incoming() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (const SessionInfo& s : g_list)
        if (s.incoming) shutdown(s.sock, SD_BOTH);
    for (const SessionInfo& s : g_agentList) agent_send({ "END", std::to_string(s.key - AGENT_KEY_BASE) });
}

void session_set_recording(int key, bool on) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        for (SessionInfo& s : g_list)
            if (s.key == key) s.recording = on;
    }
    PostMessageW(g_mainWnd, WM_APP_SESSIONS, 0, 0);
}

bool session_send_files(int key, const std::vector<std::wstring>& paths) {
    if (is_agent_key(key)) {   // the agent opens them as you (your access rights), then sends
        std::vector<std::string> f{ "SEND", std::to_string(key - AGENT_KEY_BASE) };
        for (const std::wstring& p : paths) f.push_back(narrow(p));
        agent_send(f);
        return true;
    }
    // Under the registry lock: the session removes itself before its engine goes away.
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (const SessionInfo& s : g_list)
        if (s.key == key && s.files) { s.files->send_files(paths); return true; }
    return false;
}

bool session_set_perms(int key, uint8_t perms) {
    if (is_agent_key(key)) {
        bool found = false;
        {
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            for (SessionInfo& s : g_agentList)
                if (s.key == key) { s.perms = perms; found = true; }
        }
        if (found) agent_send({ "PERMS", std::to_string(key - AGENT_KEY_BASE), std::to_string(perms) });
        return found;
    }
    bool found = false;
    {
        // Under the registry lock: the session unregisters before its HostSession goes away.
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        for (SessionInfo& s : g_list)
            if (s.key == key && s.incoming && s.host) {
                s.perms = perms;
                if (!(perms & PERM_RECORD)) s.recording = false;   // the viewer stops on its side too
                host_apply_perms(s.host, perms);
                found = true;
            }
    }
    if (found) PostMessageW(g_mainWnd, WM_APP_SESSIONS, 0, 0);
    return found;
}

std::wstring session_transfer_status(int key) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);   // the engine lives until its session leaves the registry
    for (const SessionInfo& s : g_list)
        if (s.key == key && s.files) return s.files->status();
    for (const SessionInfo& s : g_agentList)
        if (s.key == key) return s.xfer;
    return L"";
}

int session_find(const std::string& peer, bool incoming) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (const SessionInfo& s : g_list)
        if (s.peerId == peer && s.incoming == incoming) return s.key;
    for (const SessionInfo& s : g_agentList)
        if (s.peerId == peer && incoming) return s.key;
    return 0;
}

bool session_call(int key, int action) {
    if (is_agent_key(key)) {   // the agent's session: it runs the call
        agent_send({ "CALL", std::to_string(key - AGENT_KEY_BASE), std::to_string(action) });
        return true;
    }
    std::lock_guard<std::recursive_mutex> lk(g_mx);   // the session unregisters before its HostSession goes away
    for (const SessionInfo& s : g_list)
        if (s.key == key && s.incoming && s.host) { host_call(s.host, action); return true; }
    return false;
}

void session_set_call(int key, uint8_t state, bool muted) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        for (SessionInfo& s : g_list)
            if (s.key == key) { s.call = state; s.muted = muted; }
    }
    PostMessageW(g_mainWnd, WM_APP_SESSIONS, 0, 0);
}
