#pragma once
// A line-based, two-way named-pipe link between the app (signed-in user) and the agent
// (SYSTEM, started by the service). Overlapped I/O, so one thread can wait for lines while
// others write. Fields are tab-separated; tabs, newlines and backslashes inside fields are
// escaped, so any text (file names, messages) travels safely.
#include "core/common.h"

class PipeConn {
public:
    explicit PipeConn(HANDLE h = INVALID_HANDLE_VALUE) : h_(h) {
        rd_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wr_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~PipeConn() {
        close();
        CloseHandle(rd_);
        CloseHandle(wr_);
    }
    PipeConn(const PipeConn&) = delete;
    PipeConn& operator=(const PipeConn&) = delete;

    HANDLE handle() const { return h_; }
    bool open() const { return h_ != INVALID_HANDLE_VALUE; }
    void close() {
        std::lock_guard<std::mutex> lk(wmx_);
        if (h_ != INVALID_HANDLE_VALUE) { CancelIoEx(h_, nullptr); CloseHandle(h_); }
        h_ = INVALID_HANDLE_VALUE;
    }

    // Blocks until a whole line arrives; false when the other end went away.
    bool read(std::vector<std::string>& fields) {
        for (;;) {
            size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                fields = split(line);
                return true;
            }
            if (buf_.size() > (8u << 20)) return false;   // no sane line is this long
            char tmp[16384];
            OVERLAPPED ov{};
            ov.hEvent = rd_;
            ResetEvent(rd_);
            DWORD n = 0;
            HANDLE h = h_;
            if (h == INVALID_HANDLE_VALUE) return false;
            if (!ReadFile(h, tmp, sizeof(tmp), &n, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                if (!GetOverlappedResult(h, &ov, &n, TRUE)) return false;
            }
            if (n == 0) return false;
            buf_.append(tmp, n);
        }
    }

    bool write(const std::vector<std::string>& fields) {
        std::string line;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) line += '\t';
            line += esc(fields[i]);
        }
        line += '\n';
        std::lock_guard<std::mutex> lk(wmx_);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        OVERLAPPED ov{};
        ov.hEvent = wr_;
        ResetEvent(wr_);
        DWORD n = 0;
        if (!WriteFile(h_, line.data(), (DWORD)line.size(), &n, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (!GetOverlappedResult(h_, &ov, &n, TRUE)) return false;
        }
        return n == line.size();
    }

    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '\\') o += "\\\\";
            else if (c == '\t') o += "\\t";
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else o += c;
        }
        return o;
    }

private:
    static std::vector<std::string> split(const std::string& line) {
        std::vector<std::string> f(1);
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '\t') { f.emplace_back(); continue; }
            if (c == '\\' && i + 1 < line.size()) {
                char n = line[++i];
                f.back() += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
                continue;
            }
            f.back() += c;
        }
        return f;
    }

    HANDLE h_;
    HANDLE rd_, wr_;
    std::mutex wmx_;
    std::string buf_;
};

// \\.\pipe\RemoteDeskAgent-<session>: one agent per Windows session (the console's).
inline std::wstring agent_pipe_name(DWORD session) {
    return L"\\\\.\\pipe\\RemoteDeskAgent-" + std::to_wstring(session);
}
