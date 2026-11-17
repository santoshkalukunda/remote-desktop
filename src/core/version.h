#pragma once
// The single place the version number lives. It feeds the exe's version resource
// (app.rc), the installer (setup.exe reads it back from the exe), the update check
// and tools/release.js. Bump it for every release.
#define RD_VERSION_MAJOR 1
#define RD_VERSION_MINOR 5
#define RD_VERSION_PATCH 2

#define RD_STR2(x) #x
#define RD_STR(x) RD_STR2(x)
#define RD_VERSION_STR RD_STR(RD_VERSION_MAJOR) "." RD_STR(RD_VERSION_MINOR) "." RD_STR(RD_VERSION_PATCH)
