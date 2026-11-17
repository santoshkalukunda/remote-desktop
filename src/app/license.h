#pragma once
// RemoteDesk Pro licensing.
//
//   1. The user types a key (RDPRO-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX, 125 random bits).
//   2. The app sends it with a hashed ID of this PC to our server (relay/license-server.js),
//      which keeps only SHA-256 hashes of keys and enforces seats, expiry and revocation.
//   3. The server answers with a license text signed by its private key (ECDSA P-256).
//   4. The app keeps that license and checks, on every start, that the signature verifies
//      against the public key compiled in (core/license_key.h), that it was issued for this
//      PC and that it hasn't expired. Without the server's private key a license can't be
//      forged, edited or moved to another computer.
#include "core/common.h"

enum ProFeature : uint32_t {
    PRO_FULLSCREEN = 1,
    PRO_SCREENSHOT = 2,
    PRO_RECORDING  = 4,
    PRO_FILES      = 8,
};

struct LicenseInfo {
    bool active = false;
    bool busy = false;          // activating / deactivating
    uint32_t features = 0;
    std::string expires;        // YYYY-MM-DD, or "" = never
    std::wstring name;          // who it was issued to (note given when the key was created)
    std::string serial;         // short key reference, e.g. "RDPRO-7K2M9-..."
    std::wstring error;         // last activation problem, for the Settings card
};

void license_load();                            // at start-up: verify the stored license
LicenseInfo license_info();
bool license_has(uint32_t feature);
void license_activate(const std::string& key);  // background; posts WM_APP_LICENSE
void license_deactivate();                      // frees this PC's seat on the server
void license_refresh();                         // background check for a revoked key (once per run)
std::string device_id();                        // hashed, stable ID of this PC
// A Pro license on either computer unlocks the session: each side sends its signed license
// (MSG_PRO) and the other checks the signature and expiry (not the device: it's the other PC).
std::string license_token();                    // "" when this PC has no active license
uint32_t license_peer_features(const std::string& token);
