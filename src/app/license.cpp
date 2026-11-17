#include "app/license.h"
#include "app/http.h"
#include "core/app.h"
#include "core/crypto.h"
#include "core/license_key.h"
#include "core/server_config.h"
#include "core/version.h"
#include <wincrypt.h>
#include <ctime>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif

constexpr uint16_t API_PORT = 21117;
static std::mutex g_mx;
static LicenseInfo g_info;
static std::atomic<bool> g_refreshed{ false };

static void changed() { PostMessageW(g_mainWnd, WM_APP_LICENSE, 0, 0); }

static std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

static std::string api_url(const char* path) {
    std::string base = trim(setting(L"LicenseUrl"));   // hidden override, for testing
    if (base.empty()) base = RD_LICENSE_URL;
    if (base.empty()) {
        std::string host;
        uint16_t port;
        relay_address(host, port);
        base = "http://" + join_relay(host, API_PORT) + "/api";
    }
    return base + path;
}

std::string device_id() {
    // Windows' per-installation machine GUID, hashed so the server never sees the raw value.
    wchar_t guid[128] = {};
    DWORD sz = sizeof(guid);
    std::string seed;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, guid, &sz) == ERROR_SUCCESS)
        seed = narrow(guid);
    else
        seed = my_id();
    std::string s = "RemoteDesk-device|" + seed;
    uint8_t d[32];
    sha256(s.data(), s.size(), d);
    return hex(d, 16);
}

static std::string today() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char b[16];
    strftime(b, sizeof(b), "%Y-%m-%d", &lt);
    return b;
}

static std::string field(const std::string& text, const std::string& key) {
    size_t p = text.find("\n" + key + "=");
    if (p == std::string::npos) return "";
    p += key.size() + 2;
    size_t e = text.find('\n', p);
    return text.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static std::vector<uint8_t> from_base64(const std::string& b64) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return {};
    std::vector<uint8_t> out(n);
    if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, out.data(), &n, nullptr, nullptr)) return {};
    out.resize(n);
    return out;
}

// Checks a license text + signature and fills `info`; returns false with a reason otherwise.
static bool verify(const std::string& text, const std::string& sigHex, LicenseInfo& info, std::wstring& why,
                   bool thisDevice = true) {
    uint8_t sig[64];
    if (sigHex.size() != 128) { why = L"The license is damaged."; return false; }
    for (int i = 0; i < 64; ++i) sig[i] = (uint8_t)strtoul(sigHex.substr(i * 2, 2).c_str(), nullptr, 16);
    if (!p256_verify(LICENSE_PUBLIC_KEY, text.data(), text.size(), sig)) { why = L"The license signature is not valid."; return false; }
    if (text.rfind("RemoteDesk-license v1\n", 0) != 0) { why = L"Unknown license format."; return false; }
    if (thisDevice && field(text, "device") != device_id()) { why = L"This license belongs to another computer."; return false; }
    const std::string exp = field(text, "expires");
    if (exp != "never" && exp < today()) { why = L"The license expired on " + widen(exp) + L"."; return false; }
    info.active = true;
    info.features = (uint32_t)strtoul(field(text, "features").c_str(), nullptr, 10);
    info.expires = exp == "never" ? "" : exp;
    info.name = widen(field(text, "name"));
    info.serial = field(text, "serial");
    return true;
}

void license_load() {
    const std::string text = setting(L"LicenseText"), sig = setting(L"LicenseSig");
    if (text.empty()) return;
    LicenseInfo info;
    std::wstring why;
    std::vector<uint8_t> raw = from_base64(text);
    if (verify(std::string(raw.begin(), raw.end()), sig, info, why)) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_info = info;
    } else {
        std::lock_guard<std::mutex> lk(g_mx);
        g_info = {};
        g_info.error = why;
    }
}

LicenseInfo license_info() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_info;
}

bool license_has(uint32_t feature) {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_info.active && (g_info.features & feature) == feature;
}

std::string license_token() {
    if (!license_info().active) return "";
    const std::string text = setting(L"LicenseText"), sig = setting(L"LicenseSig");
    return text.empty() || sig.empty() ? "" : text + "\n" + sig;
}

uint32_t license_peer_features(const std::string& token) {
    const size_t nl = token.find('\n');
    if (nl == std::string::npos || token.size() > 8192) return 0;
    std::vector<uint8_t> raw = from_base64(token.substr(0, nl));
    LicenseInfo info;
    std::wstring why;
    return verify(std::string(raw.begin(), raw.end()), token.substr(nl + 1), info, why, false) ? info.features : 0;
}

static void set_busy(bool busy, const std::wstring& error = L"") {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_info.busy = busy;
        g_info.error = error;
    }
    changed();
}

static std::string computer_name() {
    wchar_t n[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD sz = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(n, &sz);
    return narrow(n);
}

static bool post_json(const char* path, const std::string& body, std::string& answer, std::wstring& err) {
    DWORD status = 0;
    bool ok = http_request("POST", api_url(path), body, L"application/json", 64 * 1024,
                           [&](const uint8_t* p, size_t n, size_t) { answer.append((const char*)p, n); return true; },
                           err, &status);
    if (ok && answer.empty()) { err = L"The RemoteDesk server sent an empty answer."; return false; }
    // Our license API always answers JSON. Anything else (e.g. 404/405 text) means the server
    // runs an older server.js without licensing: say so instead of a vague failure.
    if (ok && answer.find('{') == std::string::npos) {
        err = L"The RemoteDesk server can't activate keys yet (HTTP " + std::to_wstring(status) +
              L"). It needs the latest server.js, restarted.";
        return false;
    }
    return ok;
}

void license_activate(const std::string& key) {
    set_busy(true);
    std::thread([key] {
        const std::string body = "{\"key\":\"" + json_escape(key) + "\",\"device\":\"" + device_id() +
                                 "\",\"computer\":\"" + json_escape(computer_name()) + "\",\"app\":\"" RD_VERSION_STR "\"}";
        std::string answer;
        std::wstring err;
        if (!post_json("/activate", body, answer, err)) { set_busy(false, err); return; }
        if (json_field(answer, "ok") != "true") {
            std::string e = json_field(answer, "error");
            set_busy(false, e.empty() ? L"Activation failed." : widen(e));
            return;
        }
        const std::string b64 = json_field(answer, "license"), sig = json_field(answer, "signature");
        std::vector<uint8_t> raw = from_base64(b64);
        LicenseInfo info;
        std::wstring why;
        if (!verify(std::string(raw.begin(), raw.end()), sig, info, why)) { set_busy(false, why); return; }
        set_setting(L"LicenseText", b64);
        set_setting(L"LicenseSig", sig);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_info = info;
        }
        changed();
    }).detach();
}

void license_deactivate() {
    const LicenseInfo cur = license_info();
    set_busy(true);
    std::thread([cur] {
        std::string answer;
        std::wstring err;
        // Free the seat on the server; the license is removed here either way.
        post_json("/deactivate", "{\"serial\":\"" + json_escape(cur.serial) + "\",\"device\":\"" + device_id() + "\"}",
                  answer, err);
        set_setting(L"LicenseText", "");
        set_setting(L"LicenseSig", "");
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_info = {};
        }
        changed();
    }).detach();
}

void license_refresh() {
    const LicenseInfo cur = license_info();
    if (!cur.active || g_refreshed.exchange(true)) return;
    std::thread([cur] {
        std::string answer;
        std::wstring err;
        if (!post_json("/check", "{\"serial\":\"" + json_escape(cur.serial) + "\",\"device\":\"" + device_id() + "\"}",
                       answer, err))
            return;   // offline: keep working with the signed license
        if (json_field(answer, "valid") == "false") {   // revoked or seat removed on the server
            set_setting(L"LicenseText", "");
            set_setting(L"LicenseSig", "");
            std::string e = json_field(answer, "error");
            {
                std::lock_guard<std::mutex> lk(g_mx);
                g_info = {};
                g_info.error = e.empty() ? L"This license is no longer valid." : widen(e);
            }
            changed();
        }
    }).detach();
}
