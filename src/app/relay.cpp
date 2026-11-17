// Keeps this desk registered with the relay so others can reach it.
// Failures back off exponentially (2 s .. 60 s, with jitter) and the thread sleeps
// on an event in between, so an unreachable relay costs no CPU; "Retry now" or a
// new relay address wakes it at once.
#include "core/app.h"
#include <iphlpapi.h>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib")
#endif

static std::atomic<SOCKET> g_ctrlSock{ INVALID_SOCKET };
static std::atomic<int> g_relayGen{ 0 };
static std::atomic<bool> g_asleep{ false };   // between suspend and resume: stay off the relay
static HANDLE g_relayWake = nullptr;

SOCKET relay_open(const std::string& command) {
    std::string host;
    uint16_t port;
    if (!relay_address(host, port)) return INVALID_SOCKET;
    SOCKET s = tcp_connect(host, port);
    if (s != INVALID_SOCKET && !send_line(s, command)) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

void relay_wake() {
    ++g_relayGen;
    if (SOCKET s = g_ctrlSock; s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    if (g_relayWake) SetEvent(g_relayWake);
}

// ---- Wake-on-LAN ----
// While online, this PC tells the relay its network cards (MAC) and local networks, so it can be
// woken up later: the relay asks another RemoteDesk computer on the same network to send the
// "magic packet" (a sleeping or switched-off PC can only be woken from inside its own network).
//   host  -> NET <mac,mac> <ip/prefix,ip/prefix>
//   relay -> WAKE <mac,mac> <broadcast,..> <ip,..>     (to a helper on the same network)
static std::string net_line() {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* a = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &size) ==
        ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        a = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &size))
            return "";
    }
    std::string macs, nets;
    for (; a; a = a->Next) {
        // Wired first; Wi-Fi too (it can wake from sleep on some PCs). No virtual or VPN adapters.
        if ((a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) || a->PhysicalAddressLength != 6) continue;
        if (wcsstr(a->Description, L"Virtual") || wcsstr(a->Description, L"VPN") || wcsstr(a->Description, L"Hyper-V")) continue;
        char m[18];
        const BYTE* p = a->PhysicalAddress;
        snprintf(m, sizeof(m), "%02x-%02x-%02x-%02x-%02x-%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
        if (macs.size() < 200) macs += (macs.empty() ? "" : ",") + std::string(m);
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
            char ip[32];
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            if (nets.size() < 300) nets += (nets.empty() ? "" : ",") + std::string(ip) + "/" + std::to_string(u->OnLinkPrefixLength);
        }
    }
    return macs.empty() ? "" : "NET " + macs + " " + (nets.empty() ? "-" : nets);
}

static void split(const std::string& s, char sep, std::vector<std::string>& out) {
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
}

// This PC's active IPv4 addresses with their subnet broadcast address.
static std::vector<std::pair<std::string, std::string>> local_ipv4() {
    std::vector<std::pair<std::string, std::string>> out;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* a = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, a, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        a = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_INET, flags, nullptr, a, &size) != NO_ERROR) return out;
    }
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
            const uint32_t ip = ntohl(sin->sin_addr.s_addr);
            const uint32_t mask = u->OnLinkPrefixLength >= 32 ? 0xFFFFFFFFu : u->OnLinkPrefixLength == 0 ? 0 : ~((1u << (32 - u->OnLinkPrefixLength)) - 1);
            in_addr ia{}, ib{};
            ia.s_addr = sin->sin_addr.s_addr;
            ib.s_addr = htonl((ip & mask) | ~mask);
            char x[32], y[32];
            inet_ntop(AF_INET, &ia, x, sizeof(x));
            inet_ntop(AF_INET, &ib, y, sizeof(y));
            out.push_back({ x, y });
        }
    }
    return out;
}

// The magic packet: 6 x FF, then the MAC 16 times, on UDP ports 9 and 7. Sent out of every network
// card of this PC (a broadcast to 255.255.255.255 otherwise leaves through one card only), to the
// general and the subnet broadcast, the target's subnets, and the target's last address directly.
static void send_magic(const std::string& macList, const std::string& bcastList, const std::string& ipList) {
    std::vector<std::string> macs, targets{ "255.255.255.255" };
    split(macList, ',', macs);
    split(bcastList, ',', targets);
    split(ipList, ',', targets);
    std::vector<std::vector<uint8_t>> packets;
    for (const std::string& m : macs) {
        unsigned b[6];
        if (sscanf(m.c_str(), "%2x-%2x-%2x-%2x-%2x-%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) continue;
        std::vector<uint8_t> pkt(102, 0xFF);
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 6; ++j) pkt[6 + i * 6 + j] = (uint8_t)b[j];
        packets.push_back(pkt);
    }
    std::vector<std::pair<std::string, std::string>> locals = local_ipv4();
    locals.push_back({ "0.0.0.0", "" });   // and once without choosing a card
    for (int round = 0; round < 3; ++round) {
        for (const auto& [ip, bcast] : locals) {
            SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (u == INVALID_SOCKET) continue;
            BOOL on = TRUE;
            setsockopt(u, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof(on));
            sockaddr_in from{};
            from.sin_family = AF_INET;
            inet_pton(AF_INET, ip.c_str(), &from.sin_addr);
            if (bind(u, (sockaddr*)&from, sizeof(from)) != 0) { closesocket(u); continue; }
            std::vector<std::string> to = targets;
            if (!bcast.empty()) to.push_back(bcast);
            for (const std::string& t : to) {
                sockaddr_in dst{};
                dst.sin_family = AF_INET;
                if (t == "-" || inet_pton(AF_INET, t.c_str(), &dst.sin_addr) != 1) continue;
                for (u_short port : { 9, 7 }) {
                    dst.sin_port = htons(port);
                    for (const auto& pkt : packets) sendto(u, (const char*)pkt.data(), (int)pkt.size(), 0, (sockaddr*)&dst, sizeof(dst));
                }
            }
            closesocket(u);
        }
        Sleep(300);
    }
}

int wake_request(const std::string& id) {
    SOCKET s = relay_open("WAKE " + id);
    if (s == INVALID_SOCKET) return WAKE_FAILED;
    std::string acc, line;
    const LineResult r = read_line(s, acc, line, 10000);
    closesocket(s);
    if (r != LineResult::Ok) return WAKE_FAILED;   // e.g. an older relay without Wake-on-LAN
    if (line == "ONLINE") return WAKE_ONLINE;
    if (line == "UNKNOWN") return WAKE_UNKNOWN;
    if (line == "NOHELPER") return 0;
    if (line == "INET") return WAKE_INTERNET;   // nobody on its network: sent over the internet instead
    if (line.rfind("SENT ", 0) == 0) return std::max(1, atoi(line.c_str() + 5));
    return WAKE_FAILED;
}

static void post_relay(RelayState st, RelayReason why = RR_NONE, DWORD retryMs = 0) {
    PostMessageW(g_mainWnd, WM_APP_RELAY, (WPARAM)(st | (why << 8)), (LPARAM)retryMs);
}

static void control_thread() {
    int failures = 0;
    ULONGLONG takenSince = 0;
    for (;;) {
        if (g_asleep) {   // going to sleep: don't re-register until Windows resumes
            WaitForSingleObject(g_relayWake, INFINITE);
            failures = 0;
            continue;
        }
        const int gen = g_relayGen;
        if (failures == 0) post_relay(RS_CONNECTING);   // later retries just show "Offline, retrying"
        RelayReason why = RR_UNREACHABLE;
        SOCKET s = relay_open("REG " + my_id());
        if (s != INVALID_SOCKET && gen != g_relayGen) {   // relay changed while connecting
            closesocket(s);
            continue;
        }
        if (s != INVALID_SOCKET) {
            g_ctrlSock = s;
            std::string acc, line;
            LineResult r = read_line(s, acc, line, 10000);
            if (r == LineResult::Ok && line == "TAKEN") {
                // Usually our own previous connection, which the relay keeps until its idle
                // timeout (75 s) after a network drop: keep the address and retry. Only a
                // clash that outlasts that is another computer, and then we pick a new one.
                const ULONGLONG now = GetTickCount64();
                if (!takenSince) takenSince = now;
                g_ctrlSock = INVALID_SOCKET;
                closesocket(s);
                if (now - takenSince > 150000) {
                    takenSince = 0;
                    new_id();
                    PostMessageW(g_mainWnd, WM_APP_ID, 0, 0);
                    continue;
                }
                post_relay(RS_OFFLINE, RR_TAKEN, 10000);
                WaitForSingleObject(g_relayWake, 10000);
                continue;
            }
            takenSince = 0;
            if (r == LineResult::Ok && line == "OK") {
                failures = 0;
                post_relay(RS_ONLINE);
                if (const std::string net = net_line(); !net.empty()) send_line(s, net);   // so it can be woken later
                for (;;) {
                    r = read_line(s, acc, line, 25000);
                    if (r == LineResult::Timeout) {
                        if (!send_line(s, "PING")) break;
                    } else if (r == LineResult::Error) {
                        break;
                    } else if (line.rfind("CALL ", 0) == 0) {
                        std::thread(host_session, line.substr(5)).detach();
                    } else if (line.rfind("WAKE ", 0) == 0) {   // help wake a computer on our network
                        std::vector<std::string> f;
                        split(line.substr(5), ' ', f);
                        if (!f.empty())
                            std::thread(send_magic, f[0], f.size() > 1 ? f[1] : std::string(), f.size() > 2 ? f[2] : std::string()).detach();
                    }
                }
                why = RR_LOST;
            } else {
                why = RR_REFUSED;
            }
            g_ctrlSock = INVALID_SOCKET;
            closesocket(s);
        }
        if (gen != g_relayGen) { failures = 0; continue; }   // new address or "Retry now"

        DWORD delay = why == RR_LOST && failures == 0 ? 1000 : std::min<DWORD>(60000, 2000u << std::min(failures, 5));
        delay = delay * (85 + (GetTickCount() % 31)) / 100;   // jitter: many clients don't retry in lockstep
        ++failures;
        post_relay(RS_OFFLINE, why, delay);
        WaitForSingleObject(g_relayWake, delay);
    }
}

// ---- Sleep ----
// A PC going to sleep can't close its connection, so the relay would keep it "online" until its
// idle timeout, and viewers connecting meanwhile would wait for an answer instead of waking it.
// So just before sleep we say "BYE" (the relay then reports us offline at once) and stay off the
// relay until Windows resumes. Registered via powrprof at run time: this works in the service's
// agent too (no window messages needed) and keeps the link line unchanged.
#define RD_DEVICE_NOTIFY_CALLBACK 2
typedef ULONG(CALLBACK* RdPowerCallback)(PVOID context, ULONG type, PVOID setting);
struct RdPowerSubscribe { RdPowerCallback callback; PVOID context; };

static ULONG CALLBACK on_power(PVOID, ULONG type, PVOID) {
    if (type == PBT_APMSUSPEND) {
        g_asleep = true;
        if (SOCKET s = g_ctrlSock; s != INVALID_SOCKET) {
            send_line(s, "BYE");
            shutdown(s, SD_BOTH);   // the control thread then waits for resume
        }
    } else if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) {
        if (g_asleep.exchange(false)) relay_wake();   // reconnect right away
    }
    return 0;
}

static void watch_sleep() {
    HMODULE pp = LoadLibraryW(L"powrprof.dll");
    if (!pp) return;
    using Fn = DWORD(WINAPI*)(DWORD flags, HANDLE recipient, PVOID* handle);
    auto reg = (Fn)GetProcAddress(pp, "PowerRegisterSuspendResumeNotification");
    if (!reg) return;   // Windows 7: the relay's silence check still catches it
    static RdPowerSubscribe sub{ on_power, nullptr };
    PVOID handle = nullptr;
    reg(RD_DEVICE_NOTIFY_CALLBACK, &sub, &handle);   // kept for the life of the process
}

void relay_start() {
    g_relayWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    watch_sleep();
    std::thread(control_thread).detach();
}
