#pragma once
// In-app updates.
//
// The update server (relay/update-server.js, started with the relay) serves
//   GET /latest.json   { "version", "file", "sha256", "size", "notes", "signature" }
//   GET /files/<name>  the installer
// tools/release.js publishes a build there and signs the manifest with the release
// key; the app only installs a download whose SHA-256 matches and whose signature
// verifies against the public key compiled in (core/update_key.h).
#include "core/common.h"

enum class UpdateState { Idle, Checking, UpToDate, Available, Downloading, Installing, Failed };

struct UpdateStatus {
    UpdateState state = UpdateState::Idle;
    std::string latest;        // newest version on the server
    std::wstring notes;
    std::wstring error;
    int percent = 0;           // while downloading
};

const char* app_version();
int compare_versions(const std::string& a, const std::string& b);   // <0, 0, >0
std::string update_url();      // setting, or http://<relay host>:21117/latest.json
UpdateStatus update_status();
void updater_check(bool manual);   // background; posts WM_APP_UPDATE
void updater_install(bool lockScreen = false);   // download, verify, run the installer, quit
                                                 // lockScreen: reinstall for all users + the service
void updater_cleanup();
bool installed_for_all_users();   // in Program Files (setup "Install for all users")            // at start-up: delete an installer that has run
