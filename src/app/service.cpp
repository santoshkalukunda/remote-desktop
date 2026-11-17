// The RemoteDesk Windows service (setup.exe version, installed for all users).
//
// Normal apps can't see or control the lock screen, the sign-in screen or UAC prompts, and
// don't run at all before someone signs in. The service runs as SYSTEM from boot and keeps an
// "agent" (RemoteDesk.exe --agent) running in whichever Windows session is on the console.
// The agent is SYSTEM too, so it can follow the input desktop wherever it goes (lock screen,
// sign-in, UAC) and it owns the incoming sessions end to end: the encrypted channel terminates
// inside it, so no ordinary program can drive it to click or type. The app the user sees talks
// to the agent over a named pipe for approvals, the sessions list and settings (app/agent_link).
#include "core/app.h"
#include "app/service.h"
#include <wtsapi32.h>

#ifdef _MSC_VER
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

static const wchar_t* SVC_NAME = L"RemoteDeskService";

static std::wstring exe_path() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// ---- status (any user may ask) ----
bool service_running() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    bool running = false;
    if (SC_HANDLE s = OpenServiceW(scm, SVC_NAME, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS st{};
        running = QueryServiceStatus(s, &st) &&
                  (st.dwCurrentState == SERVICE_RUNNING || st.dwCurrentState == SERVICE_START_PENDING);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return running;
}

bool service_installed() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE s = OpenServiceW(scm, SVC_NAME, SERVICE_QUERY_STATUS);
    if (s) CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return s != nullptr;
}

// ---- install / uninstall (setup.exe, elevated) ----
static void stop_and_wait(SC_HANDLE s) {
    SERVICE_STATUS st{};
    ControlService(s, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 60 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(250);
}

int service_install() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return 1;
    const std::wstring cmd = L"\"" + exe_path() + L"\" --service";
    SC_HANDLE s = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (s) {
        stop_and_wait(s);
        ChangeServiceConfigW(s, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(),
                             nullptr, nullptr, nullptr, nullptr, nullptr, L"RemoteDesk");
    } else {
        s = CreateServiceW(scm, SVC_NAME, L"RemoteDesk", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                           SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr, nullptr /* LocalSystem */, nullptr);
    }
    if (!s) { CloseServiceHandle(scm); return 2; }
    SERVICE_DESCRIPTIONW d{ (LPWSTR)L"Lets people you allow reach this computer's lock and sign-in screens with RemoteDesk." };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
    SC_ACTION again[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 } };
    SERVICE_FAILURE_ACTIONSW fa{ 86400, nullptr, nullptr, 3, again };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    // Ctrl+Alt+Del from the viewer: Windows only accepts a software SAS from services when
    // this policy allows it (1 = services). Leave a stricter or broader choice alone.
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD v = 0, sz = sizeof(v);
        if (RegQueryValueExW(k, L"SoftwareSASGeneration", nullptr, nullptr, (BYTE*)&v, &sz) != ERROR_SUCCESS || v == 0) {
            v = 1;
            RegSetValueExW(k, L"SoftwareSASGeneration", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        }
        RegCloseKey(k);
    }
    const bool ok = StartServiceW(s, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok ? 0 : 3;
}

int service_uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return 1;
    if (SC_HANDLE s = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS)) {
        stop_and_wait(s);
        DeleteService(s);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return 0;
}

int service_stop() {   // setup.exe: before replacing the exe
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 1;
    if (SC_HANDLE s = OpenServiceW(scm, SVC_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS)) {
        stop_and_wait(s);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return 0;
}

// ---- the service: keep one agent in the console session ----
static SERVICE_STATUS_HANDLE g_ss = nullptr;
static HANDLE g_stop = nullptr, g_sessionChange = nullptr;

static void report(DWORD state, DWORD exitCode = NO_ERROR) {
    SERVICE_STATUS st{};
    st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    st.dwCurrentState = state;
    st.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
    st.dwWin32ExitCode = exitCode;
    st.dwWaitHint = state == SERVICE_RUNNING ? 0 : 5000;
    SetServiceStatus(g_ss, &st);
}

static DWORD WINAPI control(DWORD ctrl, DWORD, LPVOID, LPVOID) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report(SERVICE_STOP_PENDING);
        SetEvent(g_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:   // sign-in, sign-out, switch user, remote session takeover
        SetEvent(g_sessionChange);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

// Starts "RemoteDesk.exe --agent" as SYSTEM inside `session`, on its interactive desktop.
static HANDLE launch_agent(DWORD session) {
    HANDLE self = nullptr, tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &self)) return nullptr;
    const bool dup = DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &tok);
    CloseHandle(self);
    if (!dup) return nullptr;
    if (!SetTokenInformation(tok, TokenSessionId, &session, sizeof(session))) { CloseHandle(tok); return nullptr; }
    std::wstring cmd = L"\"" + exe_path() + L"\" --agent";
    STARTUPINFOW si{ sizeof(si) };
    si.lpDesktop = (LPWSTR)L"winsta0\\default";
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessAsUserW(tok, nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                         nullptr, &si, &pi);
    CloseHandle(tok);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void WINAPI service_main(DWORD, LPWSTR*) {
    g_ss = RegisterServiceCtrlHandlerExW(SVC_NAME, control, nullptr);
    if (!g_ss) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_sessionChange = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    report(SERVICE_RUNNING);

    HANDLE agent = nullptr;
    DWORD agentSession = 0xFFFFFFFF;
    int quickExits = 0;
    ULONGLONG started = 0;
    for (;;) {
        const DWORD console = WTSGetActiveConsoleSessionId();
        if (agent && WaitForSingleObject(agent, 0) == WAIT_OBJECT_0) {   // it ended (crash?)
            quickExits = GetTickCount64() - started < 10000 ? quickExits + 1 : 0;
            CloseHandle(agent);
            agent = nullptr;
        }
        if (agent && console != agentSession) {   // the console moved to another session
            TerminateProcess(agent, 0);
            WaitForSingleObject(agent, 5000);
            CloseHandle(agent);
            agent = nullptr;
            quickExits = 0;
        }
        if (!agent && console != 0xFFFFFFFF) {
            if (quickExits >= 3) {   // keeps failing: back off instead of spinning
                if (WaitForSingleObject(g_stop, 30000) == WAIT_OBJECT_0) break;
                quickExits = 0;
            }
            agent = launch_agent(console);
            agentSession = console;
            started = GetTickCount64();
        }
        HANDLE waits[3] = { g_stop, g_sessionChange, agent };
        const DWORD r = WaitForMultipleObjects(agent ? 3 : 2, waits, FALSE, 5000);
        if (r == WAIT_OBJECT_0) break;
    }
    if (agent) {
        TerminateProcess(agent, 0);
        WaitForSingleObject(agent, 5000);
        CloseHandle(agent);
    }
    report(SERVICE_STOPPED);
}

int service_run() {
    SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)SVC_NAME, service_main }, { nullptr, nullptr } };
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

// ---- for the agent: the person signed in at the console (files go to their Downloads) ----
HANDLE console_user_token() {   // a new handle (caller closes), or nullptr when nobody is signed in
    // Only SYSTEM may ask for the signed-in user's token. Callers are often already acting as that
    // user (AsUser, e.g. while saving a received file), so step back to SYSTEM for the question.
    AsSelf self;
    const DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE t = nullptr;
    if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &t)) return nullptr;
    return t;
}

AsUser::AsUser() {
    if (!g_agent) return;
    if (HANDLE t = console_user_token()) {
        on_ = ImpersonateLoggedOnUser(t) != FALSE;
        CloseHandle(t);
    }
}
AsUser::~AsUser() { if (on_) RevertToSelf(); }

AsSelf::AsSelf() {
    if (OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE, TRUE, &tok_)) RevertToSelf();
    else tok_ = nullptr;
}
AsSelf::~AsSelf() {
    if (tok_) { SetThreadToken(nullptr, tok_); CloseHandle(tok_); }
}
