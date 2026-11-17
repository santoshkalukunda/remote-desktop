#pragma once
// Windows service + SYSTEM agent (app/service.cpp, app/agent.cpp) and the app's link to the
// agent (app/agent_link.cpp). See service.cpp for why.
#include "core/common.h"

extern bool g_agent;   // this process is the agent (RemoteDesk.exe --agent)

// service.cpp
bool service_installed();
bool service_running();
int service_install();     // --install-service   (setup.exe, elevated)
int service_uninstall();   // --uninstall-service
int service_stop();        // --stop-service      (setup.exe, before replacing files)
int service_run();         // --service           (started by Windows)
HANDLE console_user_token();
std::wstring setup_wake_on_lan();   // wol.cpp (SYSTEM or elevated): network cards + Fast Startup; returns a report   // agent: new handle to the signed-in console user's token, or nullptr

// Agent: do file work as the signed-in user (their Downloads, their access rights); no-op in the
// app. AsSelf undoes it for a moment (the agent's own data folder).
class AsUser {
public:
    AsUser();
    ~AsUser();
    bool on() const { return on_; }
private:
    bool on_ = false;
};
class AsSelf {
public:
    AsSelf();
    ~AsSelf();
private:
    HANDLE tok_ = nullptr;
};

// agent.cpp (inside the agent)
int agent_main(HINSTANCE hi);
int agent_ask(const std::string& viewerId, const std::string& code);   // -1 declined, -2 nobody to ask

// agent_link.cpp (inside the app)
bool agent_link_start();   // service running: connect to it instead of hosting here; false = host here
bool agent_mode();         // the agent hosts; this app is the window onto it
void agent_send(const std::vector<std::string>& fields);
void agent_push_license();   // the Pro license changed here: the agent uses it too
constexpr int AGENT_KEY_BASE = 1000000;   // session keys of the agent's sessions, as listed in the app
