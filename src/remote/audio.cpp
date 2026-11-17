#include "remote/audio.h"
#include "core/crypto.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <future>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#endif

constexpr int SOUND_RATE = 32000, SOUND_CH = 2;   // the host's sound
constexpr int VOICE_RATE = 16000, VOICE_CH = 1;   // calls
constexpr int BLOCK_MS = 20;

// ---------------------------------------------------------------- IMA ADPCM
// Packet: u8 channels, u16 frames, then per channel { i16 predictor, u8 step index }, then 4-bit
// codes, frame by frame and channel by channel, two per byte (low nibble first).
namespace {
const int kIndex[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
const int kStep[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767 };

int decode_nibble(int code, int& pred, int& index) {
    const int step = kStep[index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    pred += (code & 8) ? -diff : diff;
    pred = std::clamp(pred, -32768, 32767);
    index = std::clamp(index + kIndex[code], 0, 88);
    return pred;
}

int encode_sample(int sample, int& pred, int& index) {
    const int step = kStep[index];
    int diff = sample - pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
    if (diff >= step >> 2) code |= 1;
    decode_nibble(code, pred, index);   // track exactly what the decoder will reconstruct
    return code;
}
}

static std::vector<uint8_t> adpcm_encode(const int16_t* pcm, int frames, int channels, std::vector<int>& st) {
    if ((int)st.size() != channels * 2) st.assign(channels * 2, 0);
    std::vector<uint8_t> out;
    out.push_back((uint8_t)channels);
    out.push_back((uint8_t)(frames & 0xFF));
    out.push_back((uint8_t)(frames >> 8));
    for (int c = 0; c < channels; ++c) {
        const int16_t p = (int16_t)st[c * 2];
        out.push_back((uint8_t)(p & 0xFF));
        out.push_back((uint8_t)((uint16_t)p >> 8));
        out.push_back((uint8_t)st[c * 2 + 1]);
    }
    bool high = false;
    for (int i = 0; i < frames * channels; ++i) {
        const int c = i % channels;
        const int code = encode_sample(pcm[i], st[c * 2], st[c * 2 + 1]);
        if (!high) out.push_back((uint8_t)code); else out.back() |= (uint8_t)(code << 4);
        high = !high;
    }
    return out;
}

static bool adpcm_decode(const uint8_t* p, size_t n, int& channels, std::vector<int16_t>& out) {
    if (n < 3) return false;
    channels = p[0];
    const int frames = p[1] | (p[2] << 8);
    if (channels < 1 || channels > 2 || frames > 4800 || n < 3 + (size_t)channels * 3 + ((size_t)frames * channels + 1) / 2) return false;
    int st[4];
    for (int c = 0; c < channels; ++c) {
        st[c * 2] = (int16_t)(p[3 + c * 3] | (p[4 + c * 3] << 8));
        st[c * 2 + 1] = std::min<int>(p[5 + c * 3], 88);
    }
    const uint8_t* d = p + 3 + channels * 3;
    out.resize((size_t)frames * channels);
    for (int i = 0; i < frames * channels; ++i) {
        const int c = i % channels;
        const int code = (i & 1) ? d[i / 2] >> 4 : d[i / 2] & 15;
        out[i] = (int16_t)decode_nibble(code, st[c * 2], st[c * 2 + 1]);
    }
    return true;
}

// ---------------------------------------------------------------- device formats
namespace {
struct Fmt { int rate = 0, ch = 0, bits = 0; bool isFloat = false; };

Fmt fmt_of(const WAVEFORMATEX* w) {
    Fmt f{ (int)w->nSamplesPerSec, w->nChannels, w->wBitsPerSample, w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT };
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const auto* x = (const WAVEFORMATEXTENSIBLE*)w;
        f.isFloat = x->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;   // the subformat GUID starts with the format tag
        if (x->Samples.wValidBitsPerSample) f.bits = w->wBitsPerSample;
    }
    return f;
}

float sample_at(const BYTE* p, const Fmt& f) {
    if (f.isFloat) return f.bits == 64 ? (float)*(const double*)p : *(const float*)p;
    switch (f.bits) {
    case 16: return *(const int16_t*)p / 32768.0f;
    case 24: return (float)((int32_t)((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) >> 8) / 8388608.0f;
    case 32: return *(const int32_t*)p / 2147483648.0f;
    default: return (p[0] - 128) / 128.0f;   // 8-bit
    }
}

void put_sample(BYTE* p, const Fmt& f, float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    if (f.isFloat) { if (f.bits == 64) *(double*)p = v; else *(float*)p = v; return; }
    switch (f.bits) {
    case 16: *(int16_t*)p = (int16_t)lrintf(v * 32767.0f); break;
    case 24: { const int32_t s = (int32_t)lrintf(v * 8388607.0f); p[0] = (BYTE)s; p[1] = (BYTE)(s >> 8); p[2] = (BYTE)(s >> 16); break; }
    case 32: *(int32_t*)p = (int32_t)lrint((double)v * 2147483647.0); break;
    default: p[0] = (BYTE)(lrintf(v * 127.0f) + 128); break;
    }
}

// Channel mapping: mono <- average of the first two, stereo <- the first two (or the one, twice).
void map_frame(const float* in, int inCh, float* out, int outCh) {
    if (outCh == 1) out[0] = inCh >= 2 ? (in[0] + in[1]) * 0.5f : in[0];
    else { out[0] = in[0]; out[1] = inCh >= 2 ? in[1] : in[0]; }
}

// Linear-interpolating resampler with state across calls (interleaved floats).
struct Resampler {
    int ch = 1;
    double step = 1, pos = 0;
    std::vector<float> prev;   // last frame of the previous call
    void reset(int inRate, int outRate, int channels) { ch = channels; step = (double)inRate / outRate; pos = 0; prev.assign(ch, 0.0f); }
    void run(const std::vector<float>& in, std::vector<float>& out) {
        const int n = (int)(in.size() / ch);
        auto at = [&](int i, int c) { return i < 0 ? prev[c] : in[(size_t)i * ch + c]; };
        // positions are relative to `prev` at -1
        while (pos < n - 1 + 1e-9 && n > 0) {
            const int i = (int)std::floor(pos);
            const float f = (float)(pos - i);
            for (int c = 0; c < ch; ++c) out.push_back(at(i - 1, c) + (at(i, c) - at(i - 1, c)) * f);
            pos += step;
        }
        if (n > 0) {
            for (int c = 0; c < ch; ++c) prev[c] = in[(size_t)(n - 1) * ch + c];
            pos -= n;
        }
    }
};

struct Com {   // COM for this thread
    Com() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~Com() { CoUninitialize(); }
};

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

IAudioClient* open_client(EDataFlow flow, ERole role, std::wstring& error) {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) {
        error = L"Windows audio isn't available";
        return nullptr;
    }
    if (FAILED(en->GetDefaultAudioEndpoint(flow, role, &dev)))
        error = flow == eCapture ? L"No microphone found" : L"No speakers found";
    else if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)))
        error = flow == eCapture ? L"The microphone couldn't be opened" : L"The speakers couldn't be opened";
    release(dev);
    release(en);
    return client;
}
}

// ---------------------------------------------------------------- capture
bool AudioCapture::start(bool loopback, int rate, int channels, Sink sink, std::wstring& error) {
    stop();
    stop_ = false;
    auto ready = std::make_shared<std::promise<std::pair<bool, std::wstring>>>();
    auto fut = ready->get_future();
    thread_ = std::thread(&AudioCapture::run, this, loopback, rate, channels, std::move(sink),
                          [ready](bool ok, std::wstring e) { ready->set_value({ ok, e }); });
    auto [ok, e] = fut.get();
    if (!ok) { stop(); error = e; }
    return ok;
}

void AudioCapture::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::run(bool loopback, int rate, int channels, Sink sink, std::function<void(bool, std::wstring)> ready) {
    Com com;
    std::wstring err;
    IAudioClient* client = open_client(loopback ? eRender : eCapture, loopback ? eConsole : eCommunications, err);
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* mix = nullptr;
    if (client && SUCCEEDED(client->GetMixFormat(&mix)) &&
        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 2000000, 0, mix, nullptr)) &&
        SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap)) && SUCCEEDED(client->Start())) {
        ready(true, L"");
    } else {
        if (err.empty()) err = loopback ? L"This computer's sound can't be captured"
                                        : L"The microphone couldn't be opened (check Windows' microphone privacy settings)";
        if (mix) CoTaskMemFree(mix);
        release(cap);
        release(client);
        ready(false, err);
        return;
    }
    const Fmt f = fmt_of(mix);
    const int bytesPerSample = f.bits / 8, frameBytes = bytesPerSample * f.ch;
    Resampler rs;
    rs.reset(f.rate, rate, channels);
    std::vector<float> mapped, resampled;
    std::vector<int16_t> block;
    const size_t blockSamples = (size_t)rate * BLOCK_MS / 1000 * channels;
    std::vector<float> in(f.ch);
    while (!stop_) {
        Sleep(10);
        UINT32 packet = 0;
        while (!stop_ && SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            mapped.clear();
            float out[2];
            for (UINT32 i = 0; i < frames; ++i) {
                for (int c = 0; c < f.ch; ++c)
                    in[c] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : sample_at(data + (size_t)i * frameBytes + (size_t)c * bytesPerSample, f);
                map_frame(in.data(), f.ch, out, channels);
                mapped.insert(mapped.end(), out, out + channels);
            }
            cap->ReleaseBuffer(frames);
            rs.run(mapped, resampled);
            for (float v : resampled) block.push_back((int16_t)lrintf(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
            resampled.clear();
            while (block.size() >= blockSamples) {
                sink(block.data(), (int)(blockSamples / channels));
                block.erase(block.begin(), block.begin() + blockSamples);
            }
        }
    }
    client->Stop();
    CoTaskMemFree(mix);
    release(cap);
    release(client);
}

// ---------------------------------------------------------------- playback
bool AudioPlayer::start(int rate, int channels, std::wstring& error) {
    stop();
    {
        std::lock_guard<std::mutex> lk(mx_);
        inRate_ = rate;
        inCh_ = channels;
        queue_.clear();
        primed_ = false;
    }
    stop_ = false;
    auto ready = std::make_shared<std::promise<std::pair<bool, std::wstring>>>();
    auto fut = ready->get_future();
    thread_ = std::thread(&AudioPlayer::run, this, [ready](bool ok, std::wstring e) { ready->set_value({ ok, e }); });
    auto [ok, e] = fut.get();
    if (!ok) { stop(); error = e; }
    return ok;
}

void AudioPlayer::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void AudioPlayer::push(const int16_t* pcm, int frames) {
    std::lock_guard<std::mutex> lk(mx_);
    if (!outRate_ || !outCh_) return;
    // to the device's rate and channels (resampler state lives in pos_ / carry_)
    std::vector<float> mapped;
    mapped.reserve((size_t)frames * outCh_);
    float in[2], out[8] = {};
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < inCh_; ++c) in[c] = pcm[i * inCh_ + c] / 32768.0f;
        if (outCh_ == 1) out[0] = inCh_ == 2 ? (in[0] + in[1]) * 0.5f : in[0];
        else for (int c = 0; c < outCh_; ++c) out[c] = c < 2 ? (inCh_ == 2 ? in[c] : in[0]) : 0.0f;   // speakers 3+ silent
        mapped.insert(mapped.end(), out, out + outCh_);
    }
    Resampler rs;
    rs.ch = outCh_;
    rs.step = (double)inRate_ / outRate_;
    rs.pos = pos_;
    rs.prev = carry_.size() == (size_t)outCh_ ? carry_ : std::vector<float>(outCh_, 0.0f);
    std::vector<float> res;
    rs.run(mapped, res);
    pos_ = rs.pos;
    carry_ = rs.prev;
    queue_.insert(queue_.end(), res.begin(), res.end());
    // Keep latency low: if far behind (a burst after a hiccup), drop the oldest audio.
    const size_t maxSamples = (size_t)outRate_ * outCh_ * 300 / 1000;
    if (queue_.size() > maxSamples) queue_.erase(queue_.begin(), queue_.begin() + (queue_.size() - maxSamples / 2));
}

void AudioPlayer::run(std::function<void(bool, std::wstring)> ready) {
    Com com;
    std::wstring err;
    IAudioClient* client = open_client(eRender, eConsole, err);
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mix = nullptr;
    UINT32 bufferFrames = 0;
    if (client && SUCCEEDED(client->GetMixFormat(&mix)) &&
        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr)) &&
        SUCCEEDED(client->GetBufferSize(&bufferFrames)) &&
        SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render)) && SUCCEEDED(client->Start())) {
        const Fmt f = fmt_of(mix);
        {
            std::lock_guard<std::mutex> lk(mx_);
            outRate_ = f.rate;
            outCh_ = f.ch;
        }
        ready(true, L"");
    } else {
        if (err.empty()) err = L"The speakers couldn't be opened";
        if (mix) CoTaskMemFree(mix);
        release(render);
        release(client);
        ready(false, err);
        return;
    }
    const Fmt f = fmt_of(mix);
    const int bytesPerSample = f.bits / 8;
    const size_t primeSamples = (size_t)f.rate * f.ch * 80 / 1000;   // start playing with ~80 ms in hand
    while (!stop_) {
        Sleep(10);
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 want = bufferFrames - padding;
        if (!want) continue;
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(want, &data))) continue;
        UINT32 written = 0;
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (!primed_ && queue_.size() >= primeSamples) primed_ = true;
            if (primed_) {
                const UINT32 have = (UINT32)(queue_.size() / f.ch);
                written = std::min(want, have);
                for (UINT32 i = 0; i < written; ++i)
                    for (int c = 0; c < f.ch; ++c) {
                        put_sample(data + ((size_t)i * f.ch + c) * bytesPerSample, f, queue_.front());
                        queue_.pop_front();
                    }
                if (written < want && padding == 0) primed_ = false;   // ran dry: buffer up again
            }
        }
        if (written) render->ReleaseBuffer(written, 0);
        else render->ReleaseBuffer(std::min<UINT32>(want, f.rate / 100), AUDCLNT_BUFFERFLAGS_SILENT);   // 10 ms of silence
    }
    client->Stop();
    CoTaskMemFree(mix);
    release(render);
    release(client);
}

// ---------------------------------------------------------------- the host's sound
void SoundSender::set(bool on) {
    if (!on) { cap_.stop(); return; }
    if (cap_.running()) return;
    std::wstring err;
    state_.clear();
    if (!cap_.start(true, SOUND_RATE, SOUND_CH, [this](const int16_t* pcm, int frames) {
            const std::vector<uint8_t> pkt = adpcm_encode(pcm, frames, SOUND_CH, state_);
            ch_.send(MSG_AUDIO, pkt.data(), (uint32_t)pkt.size());
        }, err))
        if (note_) note_(err);
}

void SoundReceiver::handle(const std::vector<uint8_t>& p) {
    if (failed_) return;
    int channels = 0;
    std::vector<int16_t> pcm;
    if (!adpcm_decode(p.data(), p.size(), channels, pcm) || channels != SOUND_CH) return;
    if (!player_.running()) {
        std::wstring err;
        if (!player_.start(SOUND_RATE, SOUND_CH, err)) { failed_ = true; return; }
    }
    player_.push(pcm.data(), (int)(pcm.size() / SOUND_CH));
}

// ---------------------------------------------------------------- calls
CallLink::CallLink(SecureChannel& ch, std::function<void()> changed, std::function<void(const std::wstring&)> note)
    : ch_(ch), changed_(std::move(changed)), note_(std::move(note)) {
    // Gives up on an unanswered ring after 45 s.
    timer_ = std::thread([this] {
        std::unique_lock<std::mutex> lk(timerMx_);
        for (;;) {
            const int gen = ringGen_;
            const bool ringing = state_ == Calling || state_ == Ringing;
            if (quit_) return;
            if (!ringing) { timerCv_.wait(lk); continue; }
            if (timerCv_.wait_for(lk, std::chrono::seconds(45), [&] { return quit_ || ringGen_ != gen; })) continue;
            lk.unlock();
            if (state_ == Calling) { send(CALL_HANGUP); set_state(Idle); if (note_) note_(L"No answer"); }
            else if (state_ == Ringing) set_state(Idle);
            lk.lock();
        }
    });
}

CallLink::~CallLink() {
    end();
    {
        std::lock_guard<std::mutex> lk(timerMx_);
        quit_ = true;
    }
    timerCv_.notify_all();
    if (timer_.joinable()) timer_.join();
}

void CallLink::send(CallAction a) {
    const uint8_t b = a;
    ch_.send(MSG_CALL, &b, 1);
}

void CallLink::set_state(State s) {
    const State before = state_.exchange(s);
    if (s == Active && before != Active) begin_media();
    if (s != Active && before == Active) end_media();
    if (s == Idle) muted_ = false;
    {
        std::lock_guard<std::mutex> lk(timerMx_);
        ++ringGen_;
    }
    timerCv_.notify_all();
    if (before != s && changed_) changed_();
}

void CallLink::call() {
    if (state_ != Idle) return;
    send(CALL_RING);
    set_state(Calling);
}

void CallLink::answer(bool accept) {
    if (state_ != Ringing) return;
    send(accept ? CALL_ACCEPT : CALL_DECLINE);
    set_state(accept ? Active : Idle);
}

void CallLink::hang_up() {
    if (state_ == Idle) return;
    send(state_ == Ringing ? CALL_DECLINE : CALL_HANGUP);
    set_state(Idle);
}

void CallLink::set_muted(bool m) {
    if (muted_.exchange(m) != m && changed_) changed_();
}

void CallLink::end() {
    if (state_ != Idle) {
        end_media();
        state_ = Idle;
    }
}

void CallLink::begin_media() {
    std::lock_guard<std::mutex> lk(mediaMx_);
    std::wstring err;
    if (!speaker_.start(VOICE_RATE, VOICE_CH, err) && note_) note_(err);
    micState_.clear();
    if (!mic_.start(false, VOICE_RATE, VOICE_CH, [this](const int16_t* pcm, int frames) {
            if (muted_) return;   // muted: send nothing (the other side hears silence)
            const std::vector<uint8_t> pkt = adpcm_encode(pcm, frames, VOICE_CH, micState_);
            ch_.send(MSG_VOICE, pkt.data(), (uint32_t)pkt.size());
        }, err) && note_)
        note_(err + L" — you can still hear the other person");
}

void CallLink::end_media() {
    std::lock_guard<std::mutex> lk(mediaMx_);
    mic_.stop();
    speaker_.stop();
}

bool CallLink::handle(uint8_t type, const std::vector<uint8_t>& p) {
    if (type == MSG_VOICE) {
        int channels = 0;
        std::vector<int16_t> pcm;
        if (state_ == Active && adpcm_decode(p.data(), p.size(), channels, pcm) && channels == VOICE_CH)
            speaker_.push(pcm.data(), (int)pcm.size());
        return true;
    }
    if (type != MSG_CALL || p.size() != 1) return false;
    switch (p[0]) {
    case CALL_RING:
        if (state_ == Idle) set_state(Ringing);
        else if (state_ == Calling) { send(CALL_ACCEPT); set_state(Active); }   // both called at once
        break;
    case CALL_ACCEPT:
        if (state_ == Calling) set_state(Active);
        break;
    case CALL_DECLINE:
        if (state_ == Calling) { set_state(Idle); if (note_) note_(L"The call was declined"); }
        break;
    case CALL_HANGUP:
        if (state_ != Idle) { set_state(Idle); if (note_) note_(L"The call ended"); }
        break;
    }
    return true;
}

const wchar_t* call_state_text(uint8_t state, bool muted) {
    switch (state) {
    case CallLink::Calling: return L"Calling…";
    case CallLink::Ringing: return L"Incoming call";
    case CallLink::Active: return muted ? L"On a call (muted)" : L"On a call";
    default: return L"";
    }
}
