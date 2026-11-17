#pragma once
// Small WinHTTP client (HTTP and HTTPS) used by the updater and license activation,
// plus a minimal reader for flat JSON answers.
#include "core/common.h"
#include <functional>

// Receives each chunk of the body and the total size (0 if unknown); return false to abort.
using HttpSink = std::function<bool(const uint8_t* data, size_t n, size_t total)>;

// `body` is sent as-is (with `contentType`) when non-empty. Fails on transport errors or when
// the body exceeds `limit`; any HTTP status is returned in `status` (sink gets the body).
bool http_request(const std::string& method, const std::string& url, const std::string& body,
                  const wchar_t* contentType, size_t limit, const HttpSink& sink, std::wstring& err,
                  DWORD* status = nullptr);

// GET that also fails on a status other than 200.
bool http_get(const std::string& url, size_t limit, const HttpSink& sink, std::wstring& err);

// Value of "key" in a flat JSON object (strings unescaped, numbers/bools as text), "" if absent.
std::string json_field(const std::string& json, const std::string& key);
std::string json_escape(const std::string& s);
