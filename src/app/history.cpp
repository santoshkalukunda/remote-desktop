#include "app/history.h"
#include "core/app.h"
#include <wincrypt.h>
#include <ctime>

#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif

constexpr size_t MAX_ENTRIES = 24;
static std::mutex g_mx;

static std::wstring file() { return data_dir() + L"\\history.ini"; }

static std::string get(const std::string& id, const wchar_t* key) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(widen(id).c_str(), key, L"", buf, 2048, file().c_str());
    return narrow(buf);
}
static void put(const std::string& id, const wchar_t* key, const std::string& v) {
    WritePrivateProfileStringW(widen(id).c_str(), key, v.empty() ? nullptr : widen(v).c_str(), file().c_str());
}

static std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> out;
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back((uint8_t)(v(s[i]) << 4 | v(s[i + 1])));
    return out;
}

static std::string protect(const std::string& pw) {
    DATA_BLOB in{ (DWORD)pw.size(), (BYTE*)pw.data() }, out{};
    if (!CryptProtectData(&in, L"RemoteDesk", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string s = hex(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}
static std::string unprotect(const std::string& h) {
    std::vector<uint8_t> b = unhex(h);
    DATA_BLOB in{ (DWORD)b.size(), b.data() }, out{};
    if (b.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string s((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

static std::vector<std::string> ids() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(), file().c_str());
    std::vector<std::string> out;
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
        std::string id = narrow(p);
        if (id.size() == 9 && digits_only(id) == id) out.push_back(id);
    }
    return out;
}

static std::vector<HistoryEntry> list_locked() {
    std::vector<HistoryEntry> v;
    for (const std::string& id : ids()) {
        HistoryEntry e;
        e.id = id;
        e.name = widen(get(id, L"Name"));
        e.lastUsed = atoll(get(id, L"LastUsed").c_str());
        e.count = atoi(get(id, L"Count").c_str());
        e.hasPassword = !get(id, L"Password").empty();
        v.push_back(e);
    }
    std::sort(v.begin(), v.end(), [](const HistoryEntry& a, const HistoryEntry& b) { return a.lastUsed > b.lastUsed; });
    return v;
}

static void changed() { PostMessageW(g_mainWnd, WM_APP_HISTORY, 0, 0); }

std::vector<HistoryEntry> history_list() {
    std::lock_guard<std::mutex> lk(g_mx);
    return list_locked();
}

void history_record(const std::string& id, const std::string& pw) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        put(id, L"LastUsed", std::to_string((long long)time(nullptr)));
        put(id, L"Count", std::to_string(atoi(get(id, L"Count").c_str()) + 1));
        if (!pw.empty()) put(id, L"Password", protect(pw));
        auto all = list_locked();   // drop the oldest beyond the limit
        for (size_t i = MAX_ENTRIES; i < all.size(); ++i)
            WritePrivateProfileStringW(widen(all[i].id).c_str(), nullptr, nullptr, file().c_str());
    }
    changed();
}

std::string history_password(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_mx);
    std::string h = get(id, L"Password");
    return h.empty() ? "" : unprotect(h);
}

void history_forget_password(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        put(id, L"Password", "");
    }
    changed();
}

void history_remove(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        WritePrivateProfileStringW(widen(id).c_str(), nullptr, nullptr, file().c_str());
    }
    changed();
}

void history_rename(const std::string& id, const std::wstring& name) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        put(id, L"Name", narrow(name));
    }
    changed();
}
