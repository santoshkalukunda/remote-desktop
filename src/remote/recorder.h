#pragma once
// Records the remote picture to an MP4 (H.264) with Windows' Media Foundation encoder.
// All calls must come from one thread (the viewer's recording thread).
#include "core/common.h"

struct IMFSinkWriter;

class Recorder {
public:
    ~Recorder() { stop(); }
    // `w` x `h` is the picture size; the video is scaled down to fit 1920x1080 if larger.
    bool start(const std::wstring& path, int w, int h, int fps, std::wstring& err);
    bool add_frame(const uint8_t* bgra, int w, int h, int64_t time100ns);   // top-down BGRA, size as in start()
    void stop();                                                            // finishes the file
    bool active() const { return writer_ != nullptr; }

private:
    IMFSinkWriter* writer_ = nullptr;
    unsigned long stream_ = 0;
    int inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0, fps_ = 15;
    bool mfStarted_ = false;
    std::vector<uint8_t> scaled_;
};
