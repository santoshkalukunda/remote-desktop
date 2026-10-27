#include "codec.h"
#include <wincodec.h>

#ifdef _MSC_VER
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

// Minimal COM smart pointer.
template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

Jpeg::Jpeg() {
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_IWICImagingFactory, (void**)&f_);
}

Jpeg::~Jpeg() {
    if (f_) f_->Release();
}

bool Jpeg::encode(const uint8_t* bgra, int stride, int w, int h, float quality,
                  std::vector<uint8_t>& out) {
    if (!f_) return false;
    Com<IStream> st;
    Com<IWICBitmapEncoder> enc;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> bag;
    Com<IWICBitmap> src;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &st))) return false;
    if (FAILED(f_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(st, WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(&frame, &bag))) return false;

    PROPBAG2 opt{};
    VARIANT v;
    VariantInit(&v);
    opt.pstrName = (LPOLESTR)L"ImageQuality";
    v.vt = VT_R4; v.fltVal = quality;
    bag->Write(1, &opt, &v);
    opt.pstrName = (LPOLESTR)L"JpegYCrCbSubsampling";   // 4:4:4 keeps text crisp
    v.vt = VT_UI1; v.bVal = 3;                          // WICJpegYCrCbSubsampling444
    bag->Write(1, &opt, &v);                            // ignored on old Windows

    if (FAILED(frame->Initialize(bag))) return false;
    if (FAILED(frame->SetSize(w, h))) return false;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&pf))) return false;

    UINT bufSize = (UINT)(stride * (h - 1) + w * 4);
    if (FAILED(f_->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, stride,
                                          bufSize, (BYTE*)bgra, &src))) return false;
    if (FAILED(frame->WriteSource(src, nullptr))) return false;   // converts to 24bpp
    if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;

    STATSTG stat{};
    HGLOBAL hg = nullptr;
    if (FAILED(st->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(st, &hg))) return false;
    size_t size = (size_t)stat.cbSize.QuadPart;
    const uint8_t* p = (const uint8_t*)GlobalLock(hg);
    if (!p) return false;
    out.assign(p, p + size);
    GlobalUnlock(hg);
    return true;
}

bool Jpeg::decode(const uint8_t* data, size_t n, int& w, int& h, std::vector<uint8_t>& bgra) {
    if (!f_) return false;
    Com<IWICStream> ws;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> conv;
    if (FAILED(f_->CreateStream(&ws))) return false;
    if (FAILED(ws->InitializeFromMemory((BYTE*)data, (DWORD)n))) return false;
    if (FAILED(f_->CreateDecoderFromStream(ws, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    if (FAILED(f_->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    UINT uw, uh;
    if (FAILED(conv->GetSize(&uw, &uh))) return false;
    w = (int)uw; h = (int)uh;
    bgra.resize((size_t)w * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
}
