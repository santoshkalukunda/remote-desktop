#pragma once
// File transfer over a session's encrypted channel, the same code on both sides.
//
//   sender                         receiver
//   MSG_FILE_OFFER2(id,size,token,name) ->    (allowed? -> Downloads\RemoteDesk\.partial\<token>.part)
//                                <- MSG_FILE_ACCEPT(id, offset)  / MSG_FILE_REJECT(id, reason)
//   MSG_FILE_DATA(id, 64 KB) ... ->           (from `offset`: what the receiver already had)
//   MSG_FILE_END(id, sha256)     ->           (hash of the whole file checked, then moved into place)
//   MSG_FILE_CANCEL(id) either way stops it (and throws the partial file away).
//
// Resuming: if the connection drops, or either PC restarts, the receiver keeps the partial
// file and the sender remembers the file (outbox.ini, by token). The next session between
// the same two computers continues every unfinished file from where it stopped.
// Peers without CAP_FILE_RESUME (older versions) get the plain MSG_FILE_OFFER, all or nothing.
#include "core/common.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <thread>

class SecureChannel;

struct Transfer {
    uint32_t id = 0;
    bool outgoing = false;
    std::wstring name, path;        // display name; local file
    uint64_t size = 0, done = 0;
    enum State { Queued, Waiting, Active, Done, Failed, Cancelled, Paused } state = Queued;
    std::wstring error;
    bool resumable = false;         // continues in a later session if interrupted
    bool resumed = false;           // continuing an earlier, interrupted transfer
    uint8_t token[16] = {};
    int percent() const { return size ? (int)(done * 100 / size) : (state == Done ? 100 : 0); }
};

class FileEngine {
public:
    // `peerId` is the other computer (resumes are matched to it); `allowed` is asked for every
    // offer in either direction (permission + license); `changed` runs (on any thread) whenever
    // the list or a progress changes; `event` reports finished, failed and interrupted files.
    FileEngine(SecureChannel& ch, std::string peerId, std::function<std::wstring()> allowed,
               std::function<void()> changed, std::function<void(const Transfer&)> event);
    ~FileEngine();

    void send_files(const std::vector<std::wstring>& paths);
    bool handle(uint8_t type, const std::vector<uint8_t>& p);   // rx thread; true if it was a file message
    void cancel(uint32_t id, bool outgoing);
    // Pause / Resume one file, either direction (both sides need CAP_FILE_PAUSE; resumable files only).
    // Pausing keeps what has arrived; resuming offers it again and it continues from there.
    void pause(uint32_t id, bool outgoing);
    void resume(uint32_t id, bool outgoing);
    bool can_pause();   // the other side understands it
    std::vector<Transfer> list();
    std::wstring status();                                      // "Sending report.pdf · 45%", or ""
    uint32_t peer_caps() { std::lock_guard<std::mutex> lk(mx_); return peerCaps_; }   // what the other side supports (MSG_CAPS)
    void stop();                                                // session ended
    void cancel_all();                                          // file transfer was switched off

    static std::wstring downloads_dir();   // Downloads\RemoteDesk (created)
    static int pending_for(const std::string& peer);   // unfinished files this PC is sending to `peer`

private:
    struct In {
        Transfer t;
        HANDLE file = INVALID_HANDLE_VALUE;
        void* hash = nullptr;
        std::wstring part, info;   // resumable: partial file + its description
    };
    void worker();
    void notify() { if (changed_) changed_(); }
    void finish_in(In& in, Transfer::State st, const std::wstring& err, bool keepPart = false);
    void resume_pending();   // the peer can resume: queue what an earlier session left unfinished

    SecureChannel& ch_;
    std::string peer_;
    std::function<std::wstring()> allowed_;
    std::function<void()> changed_;
    std::function<void(const Transfer&)> event_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::deque<Transfer> out_;          // outgoing, in order (finished ones stay for the list)
    std::map<uint32_t, In> in_;         // incoming, by the peer's id
    std::map<uint32_t, int> answers_;   // outgoing id -> 1 accepted, -1 rejected
    std::map<uint32_t, uint64_t> offsets_;   // outgoing id -> bytes the receiver already has
    std::map<uint32_t, std::wstring> rejectWhy_;
    uint32_t nextId_ = 1;
    uint32_t peerCaps_ = 0;
    bool capsKnown_ = false;
    bool stop_ = false;
    std::thread thread_;
};
