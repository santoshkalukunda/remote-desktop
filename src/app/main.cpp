// RemoteDesk entry point: one instance per user, first-run consent, then the main window.
#include "core/app.h"
#include "app/settings_internal.h"
#include "app/license.h"
#include "app/updater.h"
#include "app/service.h"
#include "ui/ui.h"
#include <shellapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#endif

std::atomic<DWORD> g_clipIgnoreSeq{ 0 };
HWND g_mainWnd = nullptr;

static bool has_arg(const wchar_t* arg) {
    int n = 0;
    LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
    bool found = false;
    for (int i = 1; i < n && a; ++i) if (!lstrcmpiW(a[i], arg)) found = true;
    LocalFree(a);
    return found;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int show) {
    // The service and its agent (setup.exe version; see app/service.cpp).
    if (has_arg(L"--service")) return service_run();
    if (has_arg(L"--agent")) return agent_main(hi);
    if (has_arg(L"--install-service")) return service_install();
    if (has_arg(L"--uninstall-service")) return service_uninstall();
    if (has_arg(L"--stop-service")) return service_stop();
    if (has_arg(L"--setup-wol")) {   // Settings > Set up Wake-on-LAN without the service (elevated)
        MessageBoxW(nullptr, setup_wake_on_lan().c_str(), L"RemoteDesk \u2014 Wake-on-LAN", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // One instance per user, so two copies don't fight over the same ID.
    CreateMutexW(nullptr, TRUE, L"Local\\RemoteDeskSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(L"RemoteDeskMain", nullptr)) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
        return 0;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    load_settings();
    ui::init(hi);
    ui::set_mode((ui::Mode)std::clamp(atoi(setting(L"Theme").c_str()), 0, 2));
    updater_cleanup();   // remove a downloaded installer once it has run
    license_load();      // verify the stored RemoteDesk Pro license (signature, this PC, expiry)

    if (!consent_given()) {   // nothing is shared or connected until the user agrees
        WelcomeChoice choice;
        if (!run_welcome(hi, choice)) return 0;
        save_consent();
        if (choice.startWithWindows) set_start_with_windows(true);
    } else if (launched_at_startup() || has_arg(L"--autostart")) {
        show = SW_SHOWMINNOACTIVE;   // signed in to Windows: stay reachable, out of the way
    }

    HWND hwnd = create_main_window(hi, show);
    if (!agent_link_start()) relay_start();   // with the service, its agent hosts this computer
    updater_check(false);   // quiet check in the background

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (HWND p = update_prompt_window(); p && IsDialogMessageW(p, &m)) continue;
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ExitProcess(0);   // detached session threads end with the process
}
