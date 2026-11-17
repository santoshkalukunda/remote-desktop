// "Set up Wake-on-LAN" on the computer that should be woken (runs as SYSTEM in the agent, or
// elevated with --setup-wol). The BIOS setting alone isn't enough: Windows and the network card's
// driver must also listen for the magic packet while the PC is off, and Fast Startup (on by default)
// often stops them. For each wired network card this turns on the driver's own wake settings where
// the driver has them, lets the card wake the PC, and turns Fast Startup off.
#include "core/common.h"
#include "app/service.h"
#include <iphlpapi.h>

static const wchar_t* NET_CLASS = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";

static std::wstring reg_str(HKEY k, const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}

// The driver's settings key for the adapter whose NetCfgInstanceId is `guid`.
static HKEY open_driver_key(const std::wstring& guid) {
    HKEY cls;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NET_CLASS, 0, KEY_READ, &cls) != ERROR_SUCCESS) return nullptr;
    HKEY found = nullptr;
    wchar_t name[64];
    for (DWORD i = 0;; ++i) {
        DWORD n = 64;
        if (RegEnumKeyExW(cls, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(cls, name, 0, KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS) continue;
        if (!lstrcmpiW(reg_str(k, L"NetCfgInstanceId").c_str(), guid.c_str())) { found = k; break; }
        RegCloseKey(k);
    }
    RegCloseKey(cls);
    return found;
}

// Sets an advanced driver setting only if this driver has it (Ndi\params\<keyword>).
static bool set_if_supported(HKEY drv, const wchar_t* keyword, const wchar_t* value) {
    HKEY p;
    if (RegOpenKeyExW(drv, (std::wstring(L"Ndi\\params\\") + keyword).c_str(), 0, KEY_READ, &p) != ERROR_SUCCESS) return false;
    RegCloseKey(p);
    if (reg_str(drv, keyword) == value) return false;   // already so
    return RegSetValueExW(drv, keyword, 0, REG_SZ, (const BYTE*)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// Runs a command hidden; returns what it printed.
static std::string run_hidden(std::wstring cmd) {
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    std::string out;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return out;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (ok) {
        char buf[4096];
        DWORD n;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    return out;
}

static std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring setup_wake_on_lan() {
    std::wstring report;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf(size);
    auto* a = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, a, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        a = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, a, &size) != NO_ERROR) a = nullptr;
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    int cards = 0;
    bool anyUsb = false;
    std::vector<std::wstring> armedNames;
    // Which kind of sleep this PC has. Most new laptops have no classic Sleep (S3) but Modern
    // Standby, which by default switches networking off while asleep.
    SYSTEM_POWER_CAPABILITIES caps{};
    bool modern = false, s3 = false;
    if (HMODULE pp = LoadLibraryW(L"powrprof.dll")) {
        using Fn = BOOLEAN(WINAPI*)(PSYSTEM_POWER_CAPABILITIES);
        if (auto fn = (Fn)GetProcAddress(pp, "GetPwrCapabilities"); fn && fn(&caps)) {
#ifdef _MSC_VER
            modern = caps.AoAc != 0;        // Modern Standby
#else
            modern = caps.spare2[2] != 0;   // the same byte: MinGW's header still calls it spare2[2]
#endif
            s3 = caps.SystemS3 != 0;
        }
    }
    for (; a; a = a->Next) {
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD || a->PhysicalAddressLength != 6) continue;
        const std::wstring desc = a->Description;
        bool skip = false;
        for (const wchar_t* w : { L"Virtual", L"VPN", L"Hyper-V", L"Bluetooth", L"TAP-", L"Miniport", L"Loopback", L"VirtualBox", L"VMware" })
            if (desc.find(w) != std::wstring::npos) skip = true;
        if (skip) continue;
        HKEY drv = open_driver_key(widen(a->AdapterName));
        if (!drv) continue;
        ++cards;
        const bool usb = reg_str(drv, L"DeviceInstanceID").rfind(L"USB\\", 0) == 0;
        anyUsb |= usb;
        std::vector<std::wstring> on;
        // Standard keyword, Realtek "Shutdown Wake-On-LAN", Intel's PME (wake from power off).
        if (set_if_supported(drv, L"*WakeOnMagicPacket", L"1")) on.push_back(L"Wake on Magic Packet");
        if (set_if_supported(drv, L"S5WakeOnLan", L"1")) on.push_back(L"Shutdown Wake-On-LAN");
        if (set_if_supported(drv, L"WakeOnMagicPacketFromS5", L"1")) on.push_back(L"Wake from power off");
        if (set_if_supported(drv, L"EnablePME", L"1")) on.push_back(L"PME (wake from power off)");
        if (set_if_supported(drv, L"*ModernStandbyWoLMagicPacket", L"1")) on.push_back(L"Wake in Modern Standby");
        // Power saving that can switch the port off while the PC sleeps (Realtek / Intel names).
        std::vector<std::wstring> off;
        if (set_if_supported(drv, L"*EEE", L"0")) off.push_back(L"Energy-Efficient Ethernet");
        if (set_if_supported(drv, L"EnableGreenEthernet", L"0")) off.push_back(L"Green Ethernet");
        if (set_if_supported(drv, L"AdvancedEEE", L"0")) off.push_back(L"Advanced EEE");
        if (set_if_supported(drv, L"PowerSavingMode", L"0")) off.push_back(L"Power Saving Mode");
        if (set_if_supported(drv, L"WolShutdownLinkSpeed", L"0")) on.push_back(L"WOL link speed 10 Mbps first");
        for (const std::wstring& x : off) on.push_back(x + L" off");
        // "Allow the computer to turn off this device" / wake permissions: clear the "disabled" bits.
        DWORD pnp = 0, sz = sizeof(pnp);
        if (RegGetValueW(drv, nullptr, L"PnPCapabilities", RRF_RT_REG_DWORD, nullptr, &pnp, &sz) == ERROR_SUCCESS && (pnp & 0x18)) {
            pnp &= ~0x18u;
            RegSetValueExW(drv, L"PnPCapabilities", 0, REG_DWORD, (const BYTE*)&pnp, sizeof(pnp));
            on.push_back(L"power management");
        }
        RegCloseKey(drv);
        // Windows' own switch: "Allow this device to wake the computer".
        run_hidden(L"\"" + std::wstring(sys) + L"\\powercfg.exe\" /deviceenablewake \"" + desc + L"\"");
        armedNames.push_back(desc);
        report += L"\u2022 " + desc + L": " +
                  (on.empty() ? std::wstring(L"wake settings already on (or the driver has none to change)")
                              : L"changed " + [&] { std::wstring s; for (auto& x : on) s += (s.empty() ? L"" : L", ") + x; return s; }()) +
                  L"; allowed to wake the PC.\n";
        if (usb)
            report += L"   USB adapter: it usually wakes the PC from Sleep. From shut down only if the BIOS keeps USB "
                      L"powered while off (look for \u201CUSB power in S4/S5\u201D, \u201CWake on USB\u201D or turn off "
                      L"\u201CDeep Sleep\u201D / \u201CErP\u201D).\n";
    }
    // Did Windows actually arm them? (A driver without Wake-on-LAN support can't be armed.)
    const std::wstring armed = lower(widen(run_hidden(L"\"" + std::wstring(sys) + L"\\powercfg.exe\" /devicequery wake_armed")));
    for (const std::wstring& n : armedNames) {
        if (armed.find(lower(n)) != std::wstring::npos)
            report += L"   \u2713 Windows has armed \u201C" + n + L"\u201D to wake the PC.\n";
        else
            report += L"   \u2717 Windows could NOT arm \u201C" + n + L"\u201D to wake the PC: its driver may not support "
                      L"Wake-on-LAN. Try the newest driver from the adapter's or the PC's maker.\n";
    }
    // Modern Standby: keep networking on while asleep, so the card can hear the wake-up.
    if (modern) {
        const std::wstring pc = L"\"" + std::wstring(sys) + L"\\powercfg.exe\" ";
        run_hidden(pc + L"/setacvalueindex SCHEME_CURRENT fea3413e-7e05-4911-9a71-700331f1c294 f15576e8-98b7-4186-b944-eafa664402d9 1");
        run_hidden(pc + L"/setactive SCHEME_CURRENT");
        report += L"\u2022 This PC sleeps with Modern Standby: networking now stays on while it's asleep (plugged in).\n";
    } else if (s3) {
        report += L"\u2022 This PC has classic Sleep (S3).\n";
    }
    if (!cards) report += L"No wired network card found. Wake-on-LAN needs a network cable (Wi-Fi can't wake a PC that's off).\n";

    // Fast Startup is a half-hibernate that keeps many network cards from listening while "off".
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        DWORD v = 1, sz = sizeof(v);
        RegGetValueW(k, nullptr, L"HiberbootEnabled", RRF_RT_REG_DWORD, nullptr, &v, &sz);
        if (v) {
            v = 0;
            RegSetValueExW(k, L"HiberbootEnabled", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
            report += L"\u2022 Windows Fast Startup turned off (it stops many network cards listening while shut down).\n";
        } else {
            report += L"\u2022 Windows Fast Startup is already off.\n";
        }
        RegCloseKey(k);
    }
    report += L"\nNext:\n"
              L"1. Restart this PC once so the network card picks up the settings.\n"
              L"2. Keep it plugged into power and put it to Sleep (not Shut down: most laptops switch the network card "
              L"off when shut down).\n"
              L"3. Check the port lights: if they go dark while it sleeps, the card has no power — look in the BIOS for "
              L"Wake on LAN (on) and Deep Sleep / ErP / energy-saving modes (off).\n"
              L"4. Connect to it from another computer with RemoteDesk: it wakes up within a minute.\n\n"
              L"The full guide: Start menu › RemoteDesk › Wake-on-LAN guide.";
    if (anyUsb) report += L"\n\nUSB network adapters: Sleep works far more often than shut down.";
    return report;
}
