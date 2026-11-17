#include "app/updater.h"
#include "app/service.h"
#include "app/http.h"
#include "core/app.h"
#include "core/update_key.h"
#include "core/version.h"
#include "core/server_config.h"
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

constexpr uint16_t UPDATE_PORT = 21117;
constexpr size_t MAX_MANIFEST = 64 * 1024, MAX_INSTALLER = 512u * 1024 * 1024;

static std::mutex g_mx;
static UpdateStatus g_st;
static std::atomic<bool> g_busy{ false };
static std::string g_file, g_sha, g_sig, g_url;   // from the last manifest

const char* app_version() { return RD_VERSION_STR; }

int compare_versions(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && a[i] != '.') x = x * 10 + (isdigit((unsigned char)a[i]) ? a[i] - '0' : 0), ++i;
        while (j < b.size() && b[j] != '.') y = y * 10 + (isdigit((unsigned char)b[j]) ? b[j] - '0' : 0), ++j;
        if (x != y) return x < y ? -1 : 1;
        ++i, ++j;
    }
    return 0;
}

std::string update_url() {
    std::string u = trim(setting(L"UpdateUrl"));   // hidden override, for testing a release
    if (!u.empty()) return u;
    if (RD_UPDATE_URL[0]) return RD_UPDATE_URL;
    std::string host;
    uint16_t port;
    relay_address(host, port);
    return "http://" + join_relay(host, UPDATE_PORT) + "/latest.json";
}

UpdateStatus update_status() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_st;
}

static void set_state(UpdateState s, const std::wstring& error = L"", int percent = 0) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_st.state = s;
        g_st.error = error;
        g_st.percent = percent;
    }
    PostMessageW(g_mainWnd, WM_APP_UPDATE, 0, 0);
}

static std::string resolve(const std::string& base, const std::string& rel) {
    if (rel.find("://") != std::string::npos) return rel;
    size_t scheme = base.find("://");
    size_t root = base.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (!rel.empty() && rel[0] == '/') return base.substr(0, root) + rel;
    return base.substr(0, base.rfind('/') + 1) + rel;
}

// Reads latest.json from our server into g_st / g_file / g_sha / g_sig / g_url.
static bool fetch_manifest(std::wstring& err, std::string& ver) {
    const std::string url = update_url();
    std::string body;
    bool ok = http_get(url, MAX_MANIFEST, [&](const uint8_t* p, size_t n, size_t) {
        body.append((const char*)p, n);
        return true;
    }, err);
    ver = json_field(body, "version");
    if (ok && ver.empty()) { ok = false; err = L"The update server sent an unreadable answer."; }
    if (!ok) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    g_st.latest = ver;
    g_st.notes = widen(json_field(body, "notes"));
    g_file = json_field(body, "file");
    g_sha = json_field(body, "sha256");
    g_sig = json_field(body, "signature");
    g_url = resolve(url, g_file);
    return true;
}

void updater_check(bool manual) {
    if (is_packaged()) return;   // the Microsoft Store updates Store installs
    if (manual) set_state(UpdateState::Checking);   // spinner now; a check already running reports soon
    std::thread([manual] {
        if (!manual) Sleep(5000);   // don't compete with start-up (without blocking a manual check)
        if (g_busy.exchange(true)) return;
        const ULONGLONG started = GetTickCount64();
        std::wstring err;
        std::string ver;
        const bool ok = fetch_manifest(err, ver);
        if (manual) {   // keep the spinner up long enough to be seen, even on a fast server
            const ULONGLONG spent = GetTickCount64() - started;
            if (spent < 700) Sleep((DWORD)(700 - spent));
        }
        if (!ok) {
            g_busy = false;
            if (manual || update_status().state == UpdateState::Checking) set_state(UpdateState::Failed, err);
            return;
        }
        g_busy = false;
        set_state(compare_versions(ver, app_version()) > 0 ? UpdateState::Available : UpdateState::UpToDate);
    }).detach();
}

// ---- verification ----
static std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
            BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha256() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void add(const void* p, size_t n) { if (h) BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
    bool done(uint8_t out[32]) { return h && BCryptFinishHash(h, out, 32, 0) == 0; }
};

static bool verify_signature(const std::string& version, const std::string& sha, const std::string& sigHex) {
    bool keySet = false;
    for (uint8_t b : UPDATE_PUBLIC_KEY) keySet = keySet || b;
    if (!keySet || sigHex.size() != 128) return false;
    uint8_t sig[64];
    for (int i = 0; i < 64; ++i) sig[i] = (uint8_t)strtoul(sigHex.substr(i * 2, 2).c_str(), nullptr, 16);

    std::string msg = "RemoteDesk-update\n" + version + "\n" + sha + "\n";
    Sha256 hs;
    hs.add(msg.data(), msg.size());
    uint8_t digest[32];
    if (!hs.done(digest)) return false;

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    struct { BCRYPT_ECCKEY_BLOB h; uint8_t xy[64]; } blob{};
    blob.h.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.h.cbKey = 32;
    memcpy(blob.xy, UPDATE_PUBLIC_KEY, 64);
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)&blob, sizeof(blob), 0) == 0 &&
              BCryptVerifySignature(key, nullptr, digest, 32, sig, 64, 0) == 0;
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static std::wstring temp_installer(const std::string& version) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"RemoteDesk-setup-" + widen(version) + L".exe";
}

void updater_install(bool lockScreen) {
    if (is_packaged() || g_busy.exchange(true)) return;
    set_state(UpdateState::Downloading);
    std::thread([lockScreen] {
        auto fail = [](const std::wstring& e) { g_busy = false; set_state(UpdateState::Failed, e); };
        std::string version, sha, sig, url;
        if (lockScreen) {   // the newest setup from our server, even if this app is already that version
            std::wstring err;
            if (!fetch_manifest(err, version)) { fail(err); return; }
            if (compare_versions(version, app_version()) < 0) { fail(L"The server's setup is older than this app."); return; }
        }
        {
            std::lock_guard<std::mutex> lk(g_mx);
            version = g_st.latest; sha = g_sha; sig = g_sig; url = g_url;
        }
        if (!verify_signature(version, sha, sig)) {
            fail(L"Update rejected: not signed with the release key.");
            return;
        }
        const std::wstring path = temp_installer(version);
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { fail(L"Couldn't save the download."); return; }
        Sha256 hs;
        size_t got = 0;
        int lastPct = -1;
        std::wstring err;
        bool ok = http_get(url, MAX_INSTALLER, [&](const uint8_t* p, size_t n, size_t total) {
            DWORD w = 0;
            if (!WriteFile(f, p, (DWORD)n, &w, nullptr) || w != n) { err = L"Couldn't save the download."; return false; }
            hs.add(p, n);
            got += n;
            int pct = total ? (int)(got * 100 / total) : 0;
            if (pct != lastPct) { lastPct = pct; set_state(UpdateState::Downloading, L"", pct); }
            return true;
        }, err);
        CloseHandle(f);
        uint8_t digest[32];
        if (ok && (!hs.done(digest) || to_hex(digest, 32) != sha)) { ok = false; err = L"Update rejected: the download is damaged."; }
        if (!ok) { DeleteFileW(path.c_str()); fail(err); return; }

        // The installer keeps the settings, closes this app and starts the new version.
        std::wstring args = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /ACCEPTPRIVACY=yes";
        // Keep how it was installed: for all users (Program Files; updating needs an administrator,
        // one Windows prompt) or only for this user (updates silently). Enabling lock-screen access
        // moves it to all users with the service (setup removes a "for me only" copy).
        const bool allUsers = lockScreen || installed_for_all_users();
        args += allUsers ? L" /ALLUSERS" : L" /CURRENTUSER";
        if (lockScreen) args += L" /MERGETASKS=lockscreen";
        set_setting(L"PendingUpdateFile", narrow(path));
        set_state(UpdateState::Installing);
        const INT_PTR started = (INT_PTR)ShellExecuteW(nullptr, allUsers ? L"runas" : L"open", path.c_str(), args.c_str(),
                                                       nullptr, SW_SHOWNORMAL);
        if (started <= 32) {   // SE_ERR_ACCESSDENIED: "No" on the administrator prompt
            DeleteFileW(path.c_str());
            set_setting(L"PendingUpdateFile", "");
            fail(started == SE_ERR_ACCESSDENIED ? L"Administrator approval was declined, so nothing changed."
                                                : L"Windows didn't start the installer.");
            return;
        }
        PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);
    }).detach();
}

bool installed_for_all_users() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (REFKNOWNFOLDERID id : { FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86 }) {
        PWSTR p = nullptr;
        bool in = false;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
            const size_t n = wcslen(p);
            in = !_wcsnicmp(exe, p, n) && exe[n] == L'\\';
        }
        CoTaskMemFree(p);
        if (in) return true;
    }
    return service_installed();
}

void updater_cleanup() {
    std::string f = setting(L"PendingUpdateFile");
    if (f.empty()) return;
    DeleteFileW(widen(f).c_str());
    set_setting(L"PendingUpdateFile", "");
}
