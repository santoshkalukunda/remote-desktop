#pragma once
// Recent outgoing connections, kept in %APPDATA%\RemoteDesk\history.ini.
// A password that got us in is saved encrypted with Windows DPAPI (only this
// Windows account on this PC can decrypt it) and reused for one-click reconnects.
#include "core/common.h"

struct HistoryEntry {
    std::string id;          // 9 digits
    std::wstring name;       // optional label; empty = show the address
    long long lastUsed = 0;  // unix time
    int count = 0;
    bool hasPassword = false;
};

std::vector<HistoryEntry> history_list();                // most recent first
void history_record(const std::string& id, const std::string& passwordThatWorked);   // "" keeps any saved one
std::string history_password(const std::string& id);    // "" if none saved
void history_forget_password(const std::string& id);
void history_remove(const std::string& id);
void history_rename(const std::string& id, const std::wstring& name);
