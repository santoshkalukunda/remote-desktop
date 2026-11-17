#pragma once
// Start-up only (app/main.cpp); everything else goes through core/app.h.
void load_settings();
bool consent_given();
void save_consent();

// Agent (app/agent.cpp): take over the address and password the app already had.
void adopt_identity(const std::string& id, const std::string& salt, const std::string& hash);
// The app's raw values, to hand to the agent.
void identity(std::string& id, std::string& salt, std::string& hash);
// App while the agent hosts (app/agent_link.cpp): show the agent's address and password state,
// and send password changes there. Off = back to this app's own (it hosts again).
void settings_proxy(bool on);
void proxy_id(const std::string& id);
void proxy_password(bool set);
