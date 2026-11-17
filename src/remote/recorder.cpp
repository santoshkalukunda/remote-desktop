#include "remote/recorder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#endif

template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool Recorder::start(const std::wstring& path, int w, int h, int fps, std::wstring& err) {
    stop();
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { err = L"Windows' video encoder isn't available."; return false; }
    mfStarted_ = true;
    inW_ = w; inH_ = h; fps_ = fps;
    // Fit into 1920x1080 (what every H.264 encoder takes), even sizes only.
    double s = std::min(1.0, std::min(1920.0 / w, 1080.0 / h));
    outW_ = std::max(2, (int)(w * s) & ~1);
    outH_ = std::max(2, (int)(h * s) & ~1);

    IMFAttributes* attrs = nullptr;
    IMFMediaType *out = nullptr, *in = nullptr;
    MFCreateAttributes(&attrs, 1);
    if (attrs) attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs, &writer_);
    release(attrs);
    const UINT32 bitrate = (UINT32)std::clamp((int64_t)outW_ * outH_ * fps * 3 / 10, (int64_t)1500000, (int64_t)12000000);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(out, MF_MT_FRAME_SIZE, outW_, outH_);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(out, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = writer_->AddStream(out, &stream_);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(outW_ * 4));   // positive = top-down rows
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(in, MF_MT_FRAME_SIZE, outW_, outH_);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(in, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = writer_->SetInputMediaType(stream_, in, nullptr);
    if (SUCCEEDED(hr)) hr = writer_->BeginWriting();
    release(out);
    release(in);
    if (FAILED(hr)) {
        release(writer_);
        DeleteFileW(path.c_str());
        err = L"Couldn't start the video recording.";
        return false;
    }
    return true;
}

// Box-filter downscale (each output pixel averages the input pixels it covers).
static void downscale(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const int y0 = (int)((int64_t)y * sh / dh), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * sh / dh));
        for (int x = 0; x < dw; ++x) {
            const int x0 = (int)((int64_t)x * sw / dw), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * sw / dw));
            unsigned b = 0, g = 0, r = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = src + ((size_t)yy * sw + xx) * 4;
                    b += p[0]; g += p[1]; r += p[2]; ++n;
                }
            uint8_t* o = dst + ((size_t)y * dw + x) * 4;
            o[0] = (uint8_t)(b / n); o[1] = (uint8_t)(g / n); o[2] = (uint8_t)(r / n); o[3] = 255;
        }
    }
}

bool Recorder::add_frame(const uint8_t* bgra, int w, int h, int64_t t) {
    if (!writer_ || w != inW_ || h != inH_) return false;
    const uint8_t* src = bgra;
    if (outW_ != w || outH_ != h) {
        scaled_.resize((size_t)outW_ * outH_ * 4);
        downscale(bgra, w, h, scaled_.data(), outW_, outH_);
        src = scaled_.data();
    }
    const DWORD bytes = (DWORD)outW_ * outH_ * 4;
    IMFMediaBuffer* buf = nullptr;
    IMFSample* sample = nullptr;
    BYTE* dst = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
    if (SUCCEEDED(hr)) hr = buf->Lock(&dst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        if (src == bgra && outW_ == w) memcpy(dst, src, bytes);
        else for (int y = 0; y < outH_; ++y) memcpy(dst + (size_t)y * outW_ * 4, src + (size_t)y * (src == bgra ? w : outW_) * 4, (size_t)outW_ * 4);
        buf->Unlock();
        hr = buf->SetCurrentLength(bytes);
    }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(t);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(10000000LL / fps_);
    if (SUCCEEDED(hr)) hr = writer_->WriteSample(stream_, sample);
    release(sample);
    release(buf);
    return SUCCEEDED(hr);
}

void Recorder::stop() {
    if (writer_) {
        writer_->Finalize();   // writes the MP4 index; without it the file won't play
        release(writer_);
    }
    if (mfStarted_) { MFShutdown(); mfStarted_ = false; }
}
