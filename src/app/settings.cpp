// settings.ini in %APPDATA%\RemoteDesk: this desk's ID, the relay, the unattended
// password hash and UI preferences. config.ini next to the exe (written by setup.exe)
// supplies the relay chosen during installation.
#include "core/app.h"
#include "core/crypto.h"
#include "app/settings_internal.h"
#include "core/server_config.h"
#include "app/service.h"
#include <cstdio>

static std::mutex g_mx;
static std::wstring g_dir, g_ini;
static std::wstring g_cfg;   // config.ini next to the exe (read-only in MSIX)
static std::string g_id, g_host, g_pwSalt, g_pwHash;
static uint16_t g_port = DEFAULT_RELAY_PORT;
static bool g_proxy = false, g_proxyPw = false;   // the agent hosts (see settings_proxy)
static std::string g_proxyId;

static std::string cfg_get(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[512];
    GetPrivateProfileStringW(section, key, L"", buf, 512, g_cfg.c_str());
    return narrow(buf);
}
static std::string ini_get(const wchar_t* key) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"RemoteDesk", key, L"", buf, 1024, g_ini.c_str());
    return narrow(buf);
}
static void ini_set(const wchar_t* key, const std::string& v) {
    WritePrivateProfileStringW(L"RemoteDesk", key, widen(v).c_str(), g_ini.c_str());
}

void load_settings() {
    wchar_t appdata[MAX_PATH] = L".";
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    g_dir = std::wstring(appdata) + L"\\RemoteDesk";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_ini = g_dir + L"\\settings.ini";
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_cfg = exe;
    g_cfg = g_cfg.substr(0, g_cfg.find_last_of(L'\\') + 1) + L"config.ini";

    std::lock_guard<std::mutex> lk(g_mx);
    g_id = ini_get(L"Id");
    if (g_id.size() != 9 || digits_only(g_id) != g_id) { g_id = random_id(); ini_set(L"Id", g_id); }

    // Admin override (setup.exe /RELAYHOST=.. /RELAYPORT=.., or the MSIX build script), else our server.
    std::string host = cfg_get(L"RemoteDesk", L"RelayHost");
    uint16_t port = RD_RELAY_PORT;
    if (host.empty()) split_relay(cfg_get(L"RemoteDesk", L"Relay"), host, port);
    else parse_port(cfg_get(L"RemoteDesk", L"RelayPort"), port);
    if (!valid_host(host)) { host = RD_RELAY_HOST; port = RD_RELAY_PORT; }
    g_host = host;
    g_port = port;
    g_pwSalt = ini_get(L"PasswordSalt");
    g_pwHash = ini_get(L"PasswordHash");
}

bool consent_given() {
    return cfg_get(L"Consent", L"PrivacyPolicy") == POLICY_VERSION || ini_get(L"AcceptedPolicy") == POLICY_VERSION;
}

void save_consent() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char when[32];
    snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    std::lock_guard<std::mutex> lk(g_mx);
    ini_set(L"AcceptedPolicy", POLICY_VERSION);
    ini_set(L"Permissions", "screen,control,clipboard,network,keepawake");
    ini_set(L"AcceptedOn", when);
}

std::wstring data_dir() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_dir;
}

std::string setting(const wchar_t* key) {
    std::lock_guard<std::mutex> lk(g_mx);
    return ini_get(key);
}
void set_setting(const wchar_t* key, const std::string& v) {
    std::lock_guard<std::mutex> lk(g_mx);
    ini_set(key, v);
}

void new_id() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_id = random_id();
    ini_set(L"Id", g_id);
}

std::string my_id() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_proxy && !g_proxyId.empty() ? g_proxyId : g_id;
}

void adopt_identity(const std::string& id, const std::string& salt, const std::string& hash) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (id.size() == 9 && digits_only(id) == id) { g_id = id; ini_set(L"Id", id); }
    g_pwSalt = salt; g_pwHash = hash;
    ini_set(L"PasswordSalt", salt);
    ini_set(L"PasswordHash", hash);
}

void identity(std::string& id, std::string& salt, std::string& hash) {
    std::lock_guard<std::mutex> lk(g_mx);
    id = g_id; salt = g_pwSalt; hash = g_pwHash;
}

void settings_proxy(bool on) { std::lock_guard<std::mutex> lk(g_mx); g_proxy = on; }
void proxy_id(const std::string& id) { std::lock_guard<std::mutex> lk(g_mx); g_proxyId = id; }
void proxy_password(bool set) { std::lock_guard<std::mutex> lk(g_mx); g_proxyPw = set; }

bool relay_address(std::string& host, uint16_t& port) {
    std::lock_guard<std::mutex> lk(g_mx);
    host = g_host;
    port = g_port;
    return !host.empty();
}

bool password_is_set() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_proxy) return g_proxyPw;
    return !g_pwHash.empty();
}

bool password_matches(const std::string& pw) {
    std::string salt, hash;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        salt = g_pwSalt; hash = g_pwHash;
    }
    return !hash.empty() && equal_ct(hash_password(pw, salt), hash);
}

void set_password(const std::string& pw) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_proxy) {   // the agent checks passwords: it keeps the hash
            g_proxyPw = !pw.empty();
            agent_send({ "SETPW", pw });
            return;
        }
    }
    std::string salt = pw.empty() ? "" : random_hex(16);
    std::string hash = pw.empty() ? "" : hash_password(pw, salt);
    std::lock_guard<std::mutex> lk(g_mx);
    g_pwSalt = salt; g_pwHash = hash;
    ini_set(L"PasswordSalt", salt);
    ini_set(L"PasswordHash", hash);
}

void post_info(InfoKind kind, const std::wstring& text) {
    auto* p = new std::wstring(text);
    if (!PostMessageW(g_mainWnd, WM_APP_INFO, (WPARAM)kind, (LPARAM)p)) delete p;
}

// Access page. Password sessions grant everything unless narrowed; approval prompts start
// with control + clipboard on, the rest off (as before this setting existed).
uint8_t default_perms(bool password) {
    const std::string v = setting(password ? L"PasswordPerms" : L"AskPerms");
    if (v.empty()) return password ? (uint8_t)PERM_ALL : (uint8_t)(PERM_CONTROL | PERM_CLIPBOARD);
    const uint8_t p = (uint8_t)(strtoul(v.c_str(), nullptr, 10) & PERM_ALL);
    // 15 was "everything" before 1.5 added sound: a password that granted everything still does.
    // (Saving the Access page writes the current value, so a deliberate choice is kept from then on.)
    if (password && p == (PERM_CONTROL | PERM_CLIPBOARD | PERM_FILES | PERM_RECORD) && setting(L"PermsV").empty())
        return PERM_ALL;
    return p;
}

void set_default_perms(bool password, uint8_t perms) {
    set_setting(password ? L"PasswordPerms" : L"AskPerms", std::to_string(perms & PERM_ALL));
    if (password) set_setting(L"PermsV", "2");   // saved with sound known: taken as chosen (see default_perms)
    if (password && !g_agent && agent_mode()) agent_send({ "DEFPERMS", std::to_string(perms & PERM_ALL) });
}
