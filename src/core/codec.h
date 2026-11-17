#pragma once
#include "common.h"

struct IWICImagingFactory;

// JPEG encode/decode via Windows Imaging Component (no external libraries).
// COM must already be initialised on the calling thread.
class Jpeg {
public:
    Jpeg();
    ~Jpeg();
    bool ok() const { return f_ != nullptr; }
    // `bgra` points at the top-left pixel of the region; `stride` is in bytes.
    bool encode(const uint8_t* bgra, int stride, int w, int h, float quality,
                std::vector<uint8_t>& out);
    // Decodes to tightly packed BGRA.
    bool decode(const uint8_t* data, size_t n, int& w, int& h, std::vector<uint8_t>& bgra);

private:
    IWICImagingFactory* f_ = nullptr;
};

// Saves top-down BGRA pixels as a PNG file (initialises COM for the call itself).
bool save_png(const uint8_t* bgra, int w, int h, const std::wstring& path);
