#include "app/http.h"
#include "core/version.h"
#include <winhttp.h>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

bool http_request(const std::string& method, const std::string& url, const std::string& body,
                  const wchar_t* contentType, size_t limit, const HttpSink& sink, std::wstring& err, DWORD* statusOut) {
    std::wstring wurl = widen(url), wmethod = widen(method);
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { err = L"The server address is not a valid URL."; return false; }
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"RemoteDesk/" RD_VERSION_STR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, wmethod.c_str(), path, nullptr, nullptr, nullptr,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (s) WinHttpSetTimeouts(s, 8000, 8000, 15000, 30000);
    std::wstring headers;
    if (!body.empty() && contentType) headers = std::wstring(L"Content-Type: ") + contentType + L"\r\n";
    if (r && WinHttpSendRequest(r, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1L,
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                                (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
        if (statusOut) *statusOut = code;
        DWORD total = 0;
        len = sizeof(total);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &total, &len, nullptr);
        std::vector<uint8_t> buf(64 * 1024);
        size_t got = 0;
        ok = true;
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) { ok = false; err = L"The download was interrupted."; break; }
            if (n == 0) break;
            got += n;
            if (got > limit) { ok = false; err = L"The answer is larger than expected."; break; }
            if (!sink(buf.data(), (size_t)n, (size_t)total)) { ok = false; break; }
        }
    } else if (err.empty()) {
        err = L"Couldn't reach the RemoteDesk server.";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

bool http_get(const std::string& url, size_t limit, const HttpSink& sink, std::wstring& err) {
    DWORD status = 0;
    bool started = false;
    bool ok = http_request("GET", url, "", nullptr, limit, [&](const uint8_t* p, size_t n, size_t total) {
        if (!started) { started = true; if (status != 200) return false; }
        return sink(p, n, total);
    }, err, &status);
    if (status != 200 && status != 0) { err = L"The server answered " + std::to_wstring(status) + L"."; return false; }
    return ok;
}

std::string json_field(const std::string& j, const std::string& key) {
    size_t k = j.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    size_t c = j.find(':', k);
    if (c == std::string::npos) return "";
    size_t p = j.find_first_not_of(" \t\r\n", c + 1);
    if (p == std::string::npos) return "";
    if (j[p] != '"') {
        size_t e = j.find_first_of(",}\r\n", p);
        return trim(j.substr(p, e - p));
    }
    std::string out;
    for (size_t i = p + 1; i < j.size() && j[i] != '"'; ++i) {
        if (j[i] == '\\' && i + 1 < j.size()) {
            char n = j[++i];
            out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
        } else {
            out += j[i];
        }
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}
