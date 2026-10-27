// MSIX / Microsoft Store integration: package detection and the
// "Start with Windows" startup task declared in packaging/AppxManifest.xml.
// The setup.exe install uses a Startup-folder shortcut instead (see installer/).
#include "app.h"
#include <appmodel.h>
#include <thread>

static const wchar_t* STARTUP_TASK_ID = L"RemoteDeskStartup";   // must match the manifest

bool is_packaged() {
    UINT32 n = 0;
    return GetCurrentPackageFullName(&n, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
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

bool set_start_with_windows(bool on) {
    return is_packaged() && on_mta([on] {
        StartupTask task = StartupTask::GetAsync(STARTUP_TASK_ID).get();
        if (!on) { task.Disable(); return true; }
        StartupTaskState s = task.RequestEnableAsync().get();   // stays off if the user disabled it in Settings
        return s == StartupTaskState::Enabled || s == StartupTaskState::EnabledByPolicy;
    });
}
#else   // MinGW has no C++/WinRT: the startup task is MSVC-only, which the MSIX build uses.
bool launched_at_startup() { return false; }
bool set_start_with_windows(bool) { return false; }
#endif
