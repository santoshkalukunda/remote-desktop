#include "remote/files.h"
#include "core/app.h"
#include "core/crypto.h"
#include "app/service.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <memory>
#include <mutex>
#include <sstream>

constexpr ULONGLONG KEEP_PARTIAL_DAYS = 30;   // unfinished transfers are forgotten after this

// Incremental SHA-256 (CNG).
struct Hasher {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Hasher() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
            BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    }
    ~Hasher() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void add(const void* p, size_t n) { if (h) BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
    bool done(uint8_t out[32]) { return h && BCryptFinishHash(h, out, 32, 0) == 0; }
};

// Feeds the first `n` bytes of an open file into `h` (resuming: the bytes sent earlier).
static bool hash_prefix(HANDLE f, uint64_t n, Hasher& h) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(f, zero, nullptr, FILE_BEGIN)) return false;
    std::vector<uint8_t> buf(1 << 20);
    while (n > 0) {
        DWORD want = (DWORD)std::min<uint64_t>(n, buf.size()), got = 0;
        if (!ReadFile(f, buf.data(), want, &got, nullptr) || got != want) return false;
        h.add(buf.data(), got);
        n -= got;
    }
    return true;
}

static std::wstring hex(const uint8_t* p, size_t n) {
    static const wchar_t* d = L"0123456789abcdef";
    std::wstring s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

// Whole small files, by wide path (MinGW's fstreams don't take wide paths).
static std::string read_all(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
    CloseHandle(f);
    return out;
}

static bool write_all(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(f);
    return ok;
}

static uint64_t file_time(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }
static uint64_t now_filetime() { FILETIME ft; GetSystemTimeAsFileTime(&ft); return file_time(ft); }
constexpr uint64_t FT_DAY = 864000000000ull;   // 100 ns units

std::wstring FileEngine::downloads_dir() {
    PWSTR p = nullptr;
    std::wstring dir;
    HANDLE user = g_agent ? console_user_token() : nullptr;   // agent: the signed-in person's Downloads
    if ((!g_agent || user) && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, user, &p))) dir = p;
    if (user) CloseHandle(user);
    CoTaskMemFree(p);
    if (dir.empty()) return L"";
    dir += L"\\RemoteDesk";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Downloads\RemoteDesk\.partial (hidden): files still arriving, kept across sessions and restarts.
static std::wstring partial_dir() {
    std::wstring d = FileEngine::downloads_dir();
    if (d.empty()) return L"";
    d += L"\\.partial";
    if (CreateDirectoryW(d.c_str(), nullptr)) SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_HIDDEN);
    return d;
}

// Forget partial files nobody resumed for a month.
static void clean_partials() {
    static std::once_flag once;
    std::call_once(once, [] {
        const std::wstring d = partial_dir();
        if (d.empty()) return;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        const uint64_t limit = now_filetime() - KEEP_PARTIAL_DAYS * FT_DAY;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && file_time(fd.ftLastWriteTime) < limit)
                DeleteFileW((d + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    });
}

// ---- outbox: files being sent, remembered until they arrive (resume after a restart) ----
//   %APPDATA%\RemoteDesk\outbox.ini, one per line: token \t peer \t size \t mtime \t added \t path
struct OutEntry { std::wstring token; std::string peer; uint64_t size = 0, mtime = 0, added = 0; std::wstring path; };
static std::mutex g_outboxMx;

static std::wstring outbox_path() { return data_dir() + L"\\outbox.ini"; }

static std::vector<OutEntry> outbox_load() {
    std::vector<OutEntry> v;
    std::stringstream f(read_all(outbox_path()));
    std::string line;
    const uint64_t limit = now_filetime() - KEEP_PARTIAL_DAYS * FT_DAY;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> c;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, '\t')) c.push_back(part);
        if (c.size() != 6) continue;
        OutEntry e{ widen(c[0]), c[1], strtoull(c[2].c_str(), nullptr, 10), strtoull(c[3].c_str(), nullptr, 10),
                    strtoull(c[4].c_str(), nullptr, 10), widen(c[5]) };
        if (e.added >= limit) v.push_back(e);
    }
    return v;
}

static void outbox_save(const std::vector<OutEntry>& v) {
    const std::wstring path = outbox_path(), tmp = path + L".tmp";
    std::ostringstream f;
    for (const OutEntry& e : v)
        f << narrow(e.token) << '\t' << e.peer << '\t' << e.size << '\t' << e.mtime << '\t' << e.added << '\t'
          << narrow(e.path) << "\r\n";
    if (write_all(tmp, f.str())) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void outbox_put(const OutEntry& e) {
    AsSelf self;   // the agent's own data folder, not the user's
    std::lock_guard<std::mutex> lk(g_outboxMx);
    std::vector<OutEntry> v = outbox_load();
    for (const OutEntry& x : v) if (x.token == e.token) return;
    v.push_back(e);
    outbox_save(v);
}

static void outbox_drop(const std::wstring& token) {
    AsSelf self;
    std::lock_guard<std::mutex> lk(g_outboxMx);
    std::vector<OutEntry> v = outbox_load();
    const size_t n = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const OutEntry& e) { return e.token == token; }), v.end());
    if (v.size() != n) outbox_save(v);
}

static bool file_stamp(const std::wstring& path, uint64_t& size, uint64_t& mtime) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    mtime = file_time(a.ftLastWriteTime);
    return true;
}

static std::wstring interrupted(const Transfer& t, bool) {
    return L"Interrupted at " + std::to_wstring(t.percent()) + L"% — it continues on the next connection";
}

// A name from the other computer is only a name: no folders, no reserved characters.
static std::wstring safe_name(std::wstring n) {
    size_t slash = n.find_last_of(L"\\/");
    if (slash != std::wstring::npos) n = n.substr(slash + 1);
    for (wchar_t& c : n)
        if (c < 32 || wcschr(L"<>:\"|?*", c)) c = L'_';
    while (!n.empty() && (n.back() == L'.' || n.back() == L' ')) n.pop_back();
    if (n.empty() || n == L"." || n == L"..") n = L"file";
    if (n.size() > 180) n = n.substr(n.size() - 180);
    return n;
}

// "report.pdf", then "report (2).pdf", ... never overwriting anything.
static HANDLE create_unique(const std::wstring& dir, const std::wstring& name, std::wstring& path) {
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    for (int i = 1; i < 1000; ++i) {
        path = dir + L"\\" + (i == 1 ? name : stem + L" (" + std::to_wstring(i) + L")" + ext);
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) return f;
    }
    return INVALID_HANDLE_VALUE;
}

static void put32(std::vector<uint8_t>& b, uint32_t v) { b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
static void put64(std::vector<uint8_t>& b, uint64_t v) { b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 8); }
static uint32_t get32(const std::vector<uint8_t>& p) { uint32_t v = 0; if (p.size() >= 4) memcpy(&v, p.data(), 4); return v; }

FileEngine::FileEngine(SecureChannel& ch, std::string peerId, std::function<std::wstring()> allowed,
                       std::function<void()> changed, std::function<void(const Transfer&)> event)
    : ch_(ch), peer_(std::move(peerId)), allowed_(std::move(allowed)), changed_(std::move(changed)), event_(std::move(event)) {
    {
        AsUser user;
        clean_partials();
    }
    const uint32_t caps = CAP_FILE_RESUME | CAP_AUDIO | CAP_FILE_PAUSE;   // one capability message for the whole session
    ch_.send(MSG_CAPS, &caps, 4);   // older peers ignore it and get all-or-nothing transfers
    thread_ = std::thread(&FileEngine::worker, this);
}

FileEngine::~FileEngine() {
    stop();
    if (thread_.joinable()) thread_.join();
}

void FileEngine::stop() {
    AsUser user;   // agent: closing and keeping partial files in the user's Downloads
    std::vector<Transfer> gone;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (stop_) return;
        stop_ = true;
        for (Transfer& t : out_)
            if (t.state == Transfer::Queued || t.state == Transfer::Waiting || t.state == Transfer::Active) {
                t.state = Transfer::Failed;
                t.error = t.resumable ? interrupted(t, true) : L"The session ended";
                gone.push_back(t);
            }
        for (auto& [id, in] : in_)
            if (in.t.state == Transfer::Active) {
                finish_in(in, Transfer::Failed, in.t.resumable ? interrupted(in.t, false) : L"The session ended", true);
                gone.push_back(in.t);
            }
        cv_.notify_all();
    }
    if (event_) for (const Transfer& t : gone) event_(t);
}

std::vector<Transfer> FileEngine::list() {
    std::lock_guard<std::mutex> lk(mx_);
    std::vector<Transfer> v(out_.begin(), out_.end());
    for (auto& [id, in] : in_) v.push_back(in.t);
    return v;
}

std::wstring FileEngine::status() {
    std::lock_guard<std::mutex> lk(mx_);
    const Transfer* cur = nullptr;
    int more = 0;
    auto look = [&](const Transfer& t) {
        if (t.state != Transfer::Active && t.state != Transfer::Waiting && t.state != Transfer::Queued) return;
        if (!cur && t.state == Transfer::Active) cur = &t; else ++more;
    };
    for (const Transfer& t : out_) look(t);
    for (auto& [id, in] : in_) look(in.t);
    if (!cur) return more ? L"Preparing files…" : L"";
    std::wstring s = std::wstring(cur->outgoing ? L"Sending " : L"Receiving ") + cur->name + L" · " +
                     std::to_wstring(cur->percent()) + L"%";
    if (more) s += L" (+" + std::to_wstring(more) + L" more)";
    return s;
}

static OutEntry entry_for(const Transfer& t, const std::string& peer) {
    OutEntry e;
    e.token = hex(t.token, 16);
    e.peer = peer;
    e.path = t.path;
    e.added = now_filetime();
    file_stamp(t.path, e.size, e.mtime);
    return e;
}

void FileEngine::send_files(const std::vector<std::wstring>& paths) {
    AsUser user;   // agent: only files the signed-in person may read
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (stop_) return;
        for (const std::wstring& p : paths) {
            uint64_t size, mtime;
            if (!file_stamp(p, size, mtime)) continue;   // folders are not sent
            Transfer t;
            t.id = nextId_++;
            t.outgoing = true;
            t.path = p;
            t.name = p.substr(p.find_last_of(L"\\/") + 1);
            t.size = size;
            BCryptGenRandom(nullptr, t.token, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            out_.push_back(t);
        }
    }
    cv_.notify_all();
    notify();
}

int FileEngine::pending_for(const std::string& peer) {
    std::lock_guard<std::mutex> lk(g_outboxMx);
    int n = 0;
    for (const OutEntry& e : outbox_load()) n += e.peer == peer;
    return n;
}

void FileEngine::resume_pending() {   // mx_ held
    std::vector<OutEntry> all;
    {
        AsSelf self;
        std::lock_guard<std::mutex> lk(g_outboxMx);
        all = outbox_load();
    }
    for (const OutEntry& e : all) {
        if (e.peer != peer_) continue;
        bool queued = false;
        for (const Transfer& t : out_) if (hex(t.token, 16) == e.token) queued = true;
        if (queued) continue;
        uint64_t size, mtime;
        if (!file_stamp(e.path, size, mtime) || size != e.size || mtime != e.mtime) {   // moved or changed: start over is wrong
            std::thread([tok = e.token] { outbox_drop(tok); }).detach();
            continue;
        }
        Transfer t;
        t.id = nextId_++;
        t.outgoing = true;
        t.path = e.path;
        t.name = e.path.substr(e.path.find_last_of(L"\\/") + 1);
        t.size = size;
        t.resumed = true;
        for (int i = 0; i < 16; ++i) t.token[i] = (uint8_t)wcstoul(e.token.substr(i * 2, 2).c_str(), nullptr, 16);
        out_.push_back(t);
    }
}

void FileEngine::cancel(uint32_t id, bool outgoing) {
    AsUser user;
    std::vector<uint8_t> b;
    put32(b, id);
    std::wstring dropToken;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (outgoing) {
            for (Transfer& t : out_)
                if (t.id == id && (t.state == Transfer::Queued || t.state == Transfer::Waiting || t.state == Transfer::Active ||
                                   t.state == Transfer::Paused)) {
                    t.state = Transfer::Cancelled;
                    dropToken = hex(t.token, 16);
                    ch_.send(MSG_FILE_CANCEL, b.data(), (uint32_t)b.size());
                } else if (t.id == id && t.state == Transfer::Failed && t.resumable) {   // discard: don't resume it
                    t.state = Transfer::Cancelled;
                    t.error.clear();
                    dropToken = hex(t.token, 16);
                }
        } else if (auto it = in_.find(id); it != in_.end() && (it->second.t.state == Transfer::Active || it->second.t.state == Transfer::Paused)) {
            finish_in(it->second, Transfer::Cancelled, L"");
            ch_.send(MSG_FILE_CANCEL, b.data(), (uint32_t)b.size());
        }
    }
    if (!dropToken.empty()) outbox_drop(dropToken);   // cancelled on purpose: don't resume it later
    cv_.notify_all();
    notify();
}

void FileEngine::finish_in(In& in, Transfer::State st, const std::wstring& err, bool keepPart) {   // mx_ held
    if (in.file != INVALID_HANDLE_VALUE) CloseHandle(in.file);
    in.file = INVALID_HANDLE_VALUE;
    delete (Hasher*)in.hash;
    in.hash = nullptr;
    in.t.state = st;
    in.t.error = err;
    if (st == Transfer::Done && !in.part.empty()) {
        // Complete and verified: move it out of .partial under a name that's free.
        std::wstring dest;
        HANDLE f = create_unique(downloads_dir(), in.t.name, dest);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (f != INVALID_HANDLE_VALUE && MoveFileExW(in.part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            in.t.path = dest;
            DeleteFileW(in.info.c_str());
        } else {
            if (f != INVALID_HANDLE_VALUE) DeleteFileW(dest.c_str());
            in.t.state = Transfer::Failed;
            in.t.error = L"Couldn't move the file into Downloads";
        }
        return;
    }
    if (st == Transfer::Done) return;
    if (keepPart && !in.part.empty()) return;   // interrupted: kept for the next session
    if (!in.part.empty()) {
        DeleteFileW(in.part.c_str());
        DeleteFileW(in.info.c_str());
    } else if (!in.t.path.empty()) {
        DeleteFileW(in.t.path.c_str());   // no half files left behind
    }
}

bool FileEngine::handle(uint8_t type, const std::vector<uint8_t>& p) {
    if (type == MSG_CAPS) {
        if (p.size() < 4) return true;
        {
            std::lock_guard<std::mutex> lk(mx_);
            peerCaps_ = get32(p);
            capsKnown_ = true;
            if ((peerCaps_ & CAP_FILE_RESUME) && !stop_) resume_pending();
        }
        cv_.notify_all();
        notify();
        return true;
    }
    if ((type < MSG_FILE_OFFER || type > MSG_FILE_CANCEL) && type != MSG_FILE_OFFER2 && type != MSG_FILE_PAUSE &&
        type != MSG_FILE_RESUME)
        return false;
    AsUser user;   // agent: files are created as the signed-in person, in their Downloads
    const uint32_t id = get32(p);
    std::vector<uint8_t> reply;
    put32(reply, id);
    std::unique_lock<std::mutex> lk(mx_);
    if (stop_) return true;
    switch (type) {
    case MSG_FILE_OFFER:
    case MSG_FILE_OFFER2: {
        const bool resumable = type == MSG_FILE_OFFER2;
        const size_t head = resumable ? 28 : 12;
        if (p.size() < head) return true;
        if (auto it = in_.find(id); it != in_.end() && it->second.t.state == Transfer::Active) return true;   // already arriving
        uint64_t size;
        memcpy(&size, p.data() + 4, 8);
        Transfer t;
        t.id = id;
        t.name = safe_name(widen(std::string(p.begin() + head, p.end())));
        t.size = size;
        t.resumable = resumable;
        if (resumable) memcpy(t.token, p.data() + 12, 16);
        lk.unlock();
        // Permission + license, and any re-hashing of a partial file, without the lock.
        std::wstring refuse;
        {
            AsSelf self;   // the checks need the agent's own rights
            refuse = allowed_ ? allowed_() : L"";
        }
        In in;
        uint64_t offset = 0;
        if (refuse.empty() && resumable) {
            const std::wstring dir = partial_dir();
            if (dir.empty()) refuse = L"No Downloads folder on the other computer";
            else {
                const std::wstring base = dir + L"\\" + hex(t.token, 16);
                in.part = base + L".part";
                in.info = base + L".info";
                in.file = CreateFileW(in.part.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (in.file == INVALID_HANDLE_VALUE) refuse = L"The other computer couldn't save the file";
            }
            if (refuse.empty()) {
                auto* h = new Hasher();
                in.hash = h;
                LARGE_INTEGER have{};
                GetFileSizeEx(in.file, &have);
                offset = (uint64_t)have.QuadPart;
                // What's already here counts only if it fits and still reads back; else start over.
                if (offset > size || (offset && !hash_prefix(in.file, offset, *h))) {
                    delete h;
                    in.hash = h = new Hasher();
                    offset = 0;
                }
                LARGE_INTEGER pos;
                pos.QuadPart = (LONGLONG)offset;
                SetFilePointerEx(in.file, pos, nullptr, FILE_BEGIN);
                SetEndOfFile(in.file);
                write_all(in.info, std::to_string(size) + "\n" + narrow(t.name) + "\n" + peer_ + "\n");
                t.done = offset;
                t.resumed = offset > 0;
            }
        } else if (refuse.empty()) {
            std::wstring dir = downloads_dir();
            if (dir.empty()) refuse = L"No Downloads folder on the other computer";
            else {
                in.file = create_unique(dir, t.name, t.path);
                if (in.file == INVALID_HANDLE_VALUE) refuse = L"The other computer couldn't save the file";
                else in.hash = new Hasher();
            }
        }
        lk.lock();
        if (stop_) {
            if (in.file != INVALID_HANDLE_VALUE) CloseHandle(in.file);
            delete (Hasher*)in.hash;
            return true;
        }
        if (!refuse.empty()) {
            if (in.file != INVALID_HANDLE_VALUE) CloseHandle(in.file);
            in.file = INVALID_HANDLE_VALUE;
            delete (Hasher*)in.hash;
            in.hash = nullptr;
            std::string r = narrow(refuse);
            reply.insert(reply.end(), r.begin(), r.end());
            t.state = Transfer::Failed;
            t.error = refuse;
            in.t = t;
            in_[id] = in;
            ch_.send(MSG_FILE_REJECT, reply.data(), (uint32_t)reply.size());
        } else {
            t.state = Transfer::Active;
            in.t = t;
            in_[id] = in;
            if (resumable) put64(reply, offset);
            ch_.send(MSG_FILE_ACCEPT, reply.data(), (uint32_t)reply.size());
        }
        break;
    }
    case MSG_FILE_DATA: {
        auto it = in_.find(id);
        if (it == in_.end() || it->second.t.state != Transfer::Active) return true;
        In& in = it->second;
        const size_t n = p.size() - 4;
        DWORD w = 0;
        if (in.t.done + n > in.t.size || !WriteFile(in.file, p.data() + 4, (DWORD)n, &w, nullptr) || w != n) {
            finish_in(in, Transfer::Failed, in.t.done + n > in.t.size ? L"More data than announced" : L"Couldn't write the file");
            ch_.send(MSG_FILE_CANCEL, reply.data(), (uint32_t)reply.size());
            break;
        }
        ((Hasher*)in.hash)->add(p.data() + 4, n);
        in.t.done += n;
        break;
    }
    case MSG_FILE_END: {
        auto it = in_.find(id);
        if (it == in_.end() || it->second.t.state != Transfer::Active || p.size() != 36) return true;
        In& in = it->second;
        uint8_t digest[32];
        bool ok = in.t.done == in.t.size && ((Hasher*)in.hash)->done(digest) && !memcmp(digest, p.data() + 4, 32);
        finish_in(in, ok ? Transfer::Done : Transfer::Failed, ok ? L"" : L"The file arrived damaged and was removed");
        Transfer copy = in.t;
        lk.unlock();
        if (event_) event_(copy);
        lk.lock();
        break;
    }
    case MSG_FILE_ACCEPT:
    case MSG_FILE_REJECT:
        answers_[id] = type == MSG_FILE_ACCEPT ? 1 : -1;
        if (type == MSG_FILE_ACCEPT && p.size() >= 12) { uint64_t off; memcpy(&off, p.data() + 4, 8); offsets_[id] = off; }
        if (type == MSG_FILE_REJECT) rejectWhy_[id] = widen(std::string(p.begin() + std::min<size_t>(4, p.size()), p.end()));
        cv_.notify_all();
        break;
    case MSG_FILE_PAUSE: {
        const bool theirOutgoing = p.size() >= 5 && p[4] == 0;   // they paused the file they send us
        if (theirOutgoing) {
            if (auto it = in_.find(id); it != in_.end() && it->second.t.state == Transfer::Active && !it->second.part.empty())
                finish_in(it->second, Transfer::Paused, L"", true);   // keep what arrived
        } else {
            for (Transfer& t : out_)
                if (t.id == id && (t.state == Transfer::Queued || t.state == Transfer::Waiting || t.state == Transfer::Active))
                    t.state = Transfer::Paused;   // the worker stops sending it
        }
        cv_.notify_all();
        break;
    }
    case MSG_FILE_RESUME:   // the receiver wants a paused file to continue: offer it again
        for (Transfer& t : out_)
            if (t.id == id && t.state == Transfer::Paused) { t.state = Transfer::Queued; t.resumed = true; }
        cv_.notify_all();
        break;
    case MSG_FILE_CANCEL:
        if (auto it = in_.find(id); it != in_.end() && (it->second.t.state == Transfer::Active || it->second.t.state == Transfer::Paused))
            finish_in(it->second, Transfer::Cancelled, L"Cancelled by the other computer");
        for (Transfer& t : out_)
            if (t.id == id && (t.state == Transfer::Waiting || t.state == Transfer::Active || t.state == Transfer::Paused)) {
                t.state = Transfer::Cancelled;
                t.error = L"Cancelled by the other computer";
                std::thread([tok = hex(t.token, 16)] { outbox_drop(tok); }).detach();
            }
        cv_.notify_all();
        break;
    }
    lk.unlock();
    notify();
    return true;
}

void FileEngine::worker() {
    std::vector<uint8_t> buf(4 + FILE_CHUNK);
    for (;;) {
        Transfer* t = nullptr;
        bool resumable = false;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait(lk, [&] {
                if (stop_) return true;
                for (Transfer& x : out_) if (x.state == Transfer::Queued) return true;
                return false;
            });
            if (stop_) return;
            // The peer says at once whether it can resume; older versions never do.
            cv_.wait_for(lk, std::chrono::seconds(3), [&] { return stop_ || capsKnown_; });
            if (stop_) return;
            for (Transfer& x : out_) if (x.state == Transfer::Queued) { t = &x; break; }
            if (!t) continue;
            t->state = Transfer::Waiting;
            t->resumable = resumable = (peerCaps_ & CAP_FILE_RESUME) != 0;
        }
        notify();
        AsUser user;   // agent: read the file with the signed-in person's rights
        const uint32_t id = t->id;
        const std::wstring path = t->path;
        const std::wstring token = hex(t->token, 16);
        auto set_state = [&](Transfer::State st, const std::wstring& err) {
            Transfer copy;
            {
                std::lock_guard<std::mutex> lk(mx_);
                if (t->state != Transfer::Cancelled && t->state != Transfer::Paused) { t->state = st; t->error = err; }
                copy = *t;
            }
            notify();
            if (event_ && st == Transfer::Failed) event_(copy);
        };

        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) { outbox_drop(token); set_state(Transfer::Failed, L"Couldn't open the file"); continue; }
        if (resumable) outbox_put(entry_for(*t, peer_));   // remembered until it has arrived
        {   // a paused file offered again: forget the previous offer's answer
            std::lock_guard<std::mutex> lk(mx_);
            answers_.erase(id);
            offsets_.erase(id);
            rejectWhy_.erase(id);
        }

        // Offer, then wait for the other side to accept (or refuse).
        std::vector<uint8_t> offer;
        put32(offer, id);
        put64(offer, t->size);
        if (resumable) offer.insert(offer.end(), t->token, t->token + 16);
        std::string name = narrow(t->name);
        offer.insert(offer.end(), name.begin(), name.end());
        int answer = 0;
        uint64_t offset = 0;
        if (ch_.send(resumable ? MSG_FILE_OFFER2 : MSG_FILE_OFFER, offer.data(), (uint32_t)offer.size())) {
            std::unique_lock<std::mutex> lk(mx_);
            // Generous: re-checking a large partial file on the other side takes a moment.
            cv_.wait_for(lk, std::chrono::seconds(120),
                         [&] { return stop_ || answers_.count(id) || t->state == Transfer::Cancelled || t->state == Transfer::Paused; });
            answer = answers_.count(id) ? answers_[id] : 0;
            if (offsets_.count(id)) offset = offsets_[id];
        }
        if (answer != 1) {
            CloseHandle(f);
            std::wstring why;
            { std::lock_guard<std::mutex> lk(mx_); why = rejectWhy_[id]; }
            if (why.empty()) why = L"Refused by the other computer";
            // A resumable file stays remembered (e.g. file transfer is off there for now): it's offered
            // again on the next connection, until it arrives or you discard it with x.
            if (answer == -1 && resumable) why += L" \u00B7 tried again next time (x discards it)";
            if (answer == 0 && resumable) set_state(Transfer::Failed, interrupted(*t, true));
            else set_state(Transfer::Failed, answer == -1 ? why : L"No answer from the other computer");
            continue;
        }

        auto hs = std::make_unique<Hasher>();
        bool ok = true;
        if (offset > t->size || (offset > 0 && !hash_prefix(f, offset, *hs))) { offset = 0; hs = std::make_unique<Hasher>(); }
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)offset;
        SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (t->state != Transfer::Cancelled && t->state != Transfer::Paused) t->state = Transfer::Active;
            t->done = offset;
            if (offset > 0) t->resumed = true;
        }
        notify();

        ULONGLONG lastNotify = 0;
        for (;;) {
            { std::lock_guard<std::mutex> lk(mx_); if (stop_ || t->state == Transfer::Cancelled || t->state == Transfer::Paused) { ok = false; break; } }
            DWORD n = 0;
            memcpy(buf.data(), &id, 4);
            if (!ReadFile(f, buf.data() + 4, FILE_CHUNK, &n, nullptr)) { ok = false; set_state(Transfer::Failed, L"Couldn't read the file"); break; }
            if (n == 0) break;
            hs->add(buf.data() + 4, n);
            if (!ch_.send(MSG_FILE_DATA, buf.data(), 4 + n)) {
                ok = false;
                set_state(Transfer::Failed, resumable ? interrupted(*t, true) : L"The connection dropped");
                break;
            }
            { std::lock_guard<std::mutex> lk(mx_); t->done += n; }
            if (GetTickCount64() - lastNotify > 150) { lastNotify = GetTickCount64(); notify(); }
            Sleep(0);   // let screen updates in between
        }
        CloseHandle(f);
        if (!ok) { notify(); continue; }
        std::vector<uint8_t> end;
        put32(end, id);
        uint8_t digest[32];
        hs->done(digest);
        end.insert(end.end(), digest, digest + 32);
        ch_.send(MSG_FILE_END, end.data(), (uint32_t)end.size());
        outbox_drop(token);
        set_state(Transfer::Done, L"");
        if (event_) { Transfer copy; { std::lock_guard<std::mutex> lk(mx_); copy = *t; } event_(copy); }
    }
}

bool FileEngine::can_pause() {
    std::lock_guard<std::mutex> lk(mx_);
    return (peerCaps_ & CAP_FILE_PAUSE) != 0;
}

void FileEngine::pause(uint32_t id, bool outgoing) {
    AsUser user;
    std::vector<uint8_t> b;
    put32(b, id);
    b.push_back(outgoing ? 0 : 1);   // 0: the file you receive from me; 1: the file you send me
    bool tell = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (!(peerCaps_ & CAP_FILE_PAUSE)) return;
        if (outgoing) {
            for (Transfer& t : out_)
                if (t.id == id && (t.state == Transfer::Queued || (t.state == Transfer::Active && t.resumable))) {
                    tell = t.state == Transfer::Active;   // a queued file hasn't been offered yet
                    t.state = Transfer::Paused;
                }
        } else if (auto it = in_.find(id); it != in_.end() && it->second.t.state == Transfer::Active && !it->second.part.empty()) {
            finish_in(it->second, Transfer::Paused, L"", true);   // keep what arrived
            tell = true;
        }
    }
    if (tell) ch_.send(MSG_FILE_PAUSE, b.data(), (uint32_t)b.size());
    cv_.notify_all();
    notify();
}

void FileEngine::resume(uint32_t id, bool outgoing) {
    std::vector<uint8_t> b;
    put32(b, id);
    bool ask = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (outgoing) {
            for (Transfer& t : out_)
                if (t.id == id && t.state == Transfer::Paused) { t.state = Transfer::Queued; t.resumed = true; }
        } else if (auto it = in_.find(id); it != in_.end() && it->second.t.state == Transfer::Paused) {
            it->second.t.state = Transfer::Waiting;   // "Resuming…" until the sender offers it again
            ask = true;
        }
    }
    if (ask) ch_.send(MSG_FILE_RESUME, b.data(), (uint32_t)b.size());
    cv_.notify_all();
    notify();
}

void FileEngine::cancel_all() {
    std::vector<std::pair<uint32_t, bool>> live;
    {
        std::lock_guard<std::mutex> lk(mx_);
        for (Transfer& t : out_)
            if (t.state == Transfer::Queued || t.state == Transfer::Waiting || t.state == Transfer::Active) live.push_back({ t.id, true });
        for (auto& [id, in] : in_)
            if (in.t.state == Transfer::Active) live.push_back({ id, false });
    }
    for (auto& [id, out] : live) cancel(id, out);
}
