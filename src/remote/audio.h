#pragma once
// Sound and calls during a session, over the session's encrypted channel (no other app needed).
//
//   SoundSender   host:   its speakers' output (WASAPI loopback) -> MSG_AUDIO, 32 kHz stereo,
//                         while the viewer asked for it (MSG_AUDIO_CTL) and PERM_AUDIO allows it
//   SoundReceiver viewer: MSG_AUDIO -> its speakers
//   CallLink      both:   MSG_CALL ring / accept / decline / hang up; during the call each side's
//                         microphone -> MSG_VOICE (16 kHz mono) -> the other side's speakers
//
// Audio is cut into 20 ms pieces and compressed 4:1 with IMA ADPCM (simple, no licence, a few
// lines of code). Each piece carries its own decoder start state, so a lost piece costs 20 ms
// of sound and nothing more. Playback keeps a small buffer (about 80 ms) to ride out jitter.
#include "core/common.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>

class SecureChannel;

// Captures the default microphone (communications role) or, with `loopback`, what the default
// speakers play; delivers 20 ms blocks of 16-bit PCM at `rate` / `channels` on its own thread.
class AudioCapture {
public:
    using Sink = std::function<void(const int16_t* pcm, int frames)>;
    ~AudioCapture() { stop(); }
    bool start(bool loopback, int rate, int channels, Sink sink, std::wstring& error);
    void stop();
    bool running() const { return thread_.joinable(); }
private:
    void run(bool loopback, int rate, int channels, Sink sink, std::function<void(bool, std::wstring)> ready);
    std::thread thread_;
    std::atomic<bool> stop_{ false };
};

// Plays 16-bit PCM (`rate` / `channels`) on the default speakers.
class AudioPlayer {
public:
    ~AudioPlayer() { stop(); }
    bool start(int rate, int channels, std::wstring& error);
    void push(const int16_t* pcm, int frames);
    void stop();
    bool running() const { return thread_.joinable(); }
private:
    void run(std::function<void(bool, std::wstring)> ready);
    std::thread thread_;
    std::atomic<bool> stop_{ false };
    std::mutex mx_;
    std::deque<float> queue_;          // interleaved, at the device's rate and channel count
    int inRate_ = 0, inCh_ = 0, outRate_ = 0, outCh_ = 0;
    double pos_ = 0;                   // resampler position
    std::vector<float> carry_;         // last input frame, for interpolation across pushes
    bool primed_ = false;              // enough buffered to start (again) after running dry
};

// Host: sends its sound while the viewer listens and the permission allows it.
class SoundSender {
public:
    SoundSender(SecureChannel& ch, std::function<void(const std::wstring&)> note) : ch_(ch), note_(std::move(note)) {}
    void set(bool on);   // the viewer's MSG_AUDIO_CTL, or the permission was revoked (false)
    bool on() const { return cap_.running(); }
private:
    SecureChannel& ch_;
    std::function<void(const std::wstring&)> note_;
    AudioCapture cap_;
    std::vector<int> state_;
};

// Viewer: plays the host's sound.
class SoundReceiver {
public:
    void handle(const std::vector<uint8_t>& p);
    void stop() { player_.stop(); }
private:
    AudioPlayer player_;
    bool failed_ = false;
};

class CallLink {
public:
    enum State : uint8_t { Idle = 0, Calling = 1, Ringing = 2, Active = 3 };   // Ringing: the other side calls us
    // `changed` runs on any thread whenever the state or mute changes; `note` shows a short message.
    CallLink(SecureChannel& ch, std::function<void()> changed, std::function<void(const std::wstring&)> note);
    ~CallLink();
    void call();                 // ring the other side
    void answer(bool accept);    // to a ring
    void hang_up();
    void set_muted(bool m);
    State state() const { return state_; }
    bool muted() const { return muted_; }
    bool handle(uint8_t type, const std::vector<uint8_t>& p);   // MSG_CALL / MSG_VOICE; true if it was one
    void end();                  // session over: stop everything quietly
private:
    void set_state(State s);
    void begin_media();
    void end_media();
    void send(CallAction a);
    SecureChannel& ch_;
    std::function<void()> changed_;
    std::function<void(const std::wstring&)> note_;
    std::atomic<State> state_{ Idle };
    std::atomic<bool> muted_{ false };
    std::mutex mediaMx_;
    AudioCapture mic_;
    AudioPlayer speaker_;
    std::vector<int> micState_;
    std::thread timer_;          // gives up ringing after 45 s
    std::condition_variable timerCv_;
    std::mutex timerMx_;
    bool quit_ = false;
    int ringGen_ = 0;
};

// Call states for the main window / registry (host side), and for the viewer toolbar.
const wchar_t* call_state_text(uint8_t state, bool muted);
