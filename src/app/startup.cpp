// Start with Windows.
//  - Microsoft Store (MSIX): the "RemoteDeskStartup" task declared in packaging/AppxManifest.xml.
//  - setup.exe / portable: a "RemoteDesk" value under ...\CurrentVersion\Run (the installer's
//    "Start when I sign in" option writes the same value, so the app shows and changes it).
#include "core/app.h"
#include <appmodel.h>
#include <shlobj.h>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#endif

[[maybe_unused]] static const wchar_t* STARTUP_TASK_ID = L"RemoteDeskStartup";   // must match the manifest
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE = L"RemoteDesk";

bool is_packaged() {
    UINT32 n = 0;
    return GetCurrentPackageFullName(&n, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
}

// ---- unpackaged: Run key (+ the Startup-folder shortcut older installers made) ----
static bool run_value(HKEY root, REGSAM view, bool remove = false) {
    HKEY k;
    if (RegOpenKeyExW(root, RUN_KEY, 0, (remove ? KEY_SET_VALUE : KEY_QUERY_VALUE) | view, &k) != ERROR_SUCCESS)
        return false;
    bool r = remove ? RegDeleteValueW(k, RUN_VALUE) == ERROR_SUCCESS
                    : RegQueryValueExW(k, RUN_VALUE, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return r;
}

static std::wstring startup_shortcut(const KNOWNFOLDERID& folder) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &p))) r = std::wstring(p) + L"\\RemoteDesk.lnk";
    CoTaskMemFree(p);
    return r;
}

static bool exists(const std::wstring& f) {
    return !f.empty() && GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool machine_wide() {
    return run_value(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY) || run_value(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY) ||
           exists(startup_shortcut(FOLDERID_CommonStartup));
}

static bool unpackaged_enabled() {
    return run_value(HKEY_CURRENT_USER, 0) || exists(startup_shortcut(FOLDERID_Startup)) || machine_wide();
}

static bool unpackaged_set(bool on) {
    if (on) {
        if (unpackaged_enabled()) return true;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --autostart";
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
            return false;
        bool ok = RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                                 (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        RegCloseKey(k);
        return ok;
    }
    run_value(HKEY_CURRENT_USER, 0, true);
    DeleteFileW(startup_shortcut(FOLDERID_Startup).c_str());
    if (machine_wide()) {   // installed "for all users": only works when running as administrator
        run_value(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, true);
        run_value(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, true);
        DeleteFileW(startup_shortcut(FOLDERID_CommonStartup).c_str());
    }
    return !unpackaged_enabled();
}

#ifdef _MSC_VER
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Foundation.h>
#pragma comment(lib, "windowsapp.lib")
using namespace winrt::Windows::ApplicationModel;

// WinRT async calls may not block an STA (UI) thread, so run them on a fresh MTA thread.
template <class F> static bool on_mta(F f) {
    bool r = false;
    std::thread([&] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try { r = f(); } catch (...) {}
        winrt::uninit_apartment();
    }).join();
    return r;
}

bool launched_at_startup() {
    return is_packaged() && on_mta([] {
        auto args = AppInstance::GetActivatedEventArgs();
        return args && args.Kind() == Activation::ActivationKind::StartupTask;
    });
}

bool start_with_windows_enabled() {
    if (!is_packaged()) return unpackaged_enabled();
    return on_mta([] {
        StartupTaskState s = StartupTask::GetAsync(STARTUP_TASK_ID).get().State();
        return s == StartupTaskState::Enabled || s == StartupTaskState::EnabledByPolicy;
    });
}

bool set_start_with_windows(bool on) {
    if (!is_packaged()) return unpackaged_set(on);
    return on_mta([on] {
        StartupTask task = StartupTask::GetAsync(STARTUP_TASK_ID).get();
        if (!on) { task.Disable(); return true; }
        StartupTaskState s = task.RequestEnableAsync().get();   // stays off if the user disabled it in Settings
        return s == StartupTaskState::Enabled || s == StartupTaskState::EnabledByPolicy;
    });
}
#else   // MinGW has no C++/WinRT: the startup task is MSVC-only, which the MSIX build uses.
bool launched_at_startup() { return false; }
bool start_with_windows_enabled() { return !is_packaged() && unpackaged_enabled(); }
bool set_start_with_windows(bool on) { return !is_packaged() && unpackaged_set(on); }
#endif
