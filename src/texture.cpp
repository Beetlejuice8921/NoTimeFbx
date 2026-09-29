// Texture decoding: WIC for common formats, a small built-in TGA reader, CPU mip generation.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>

#include "texture.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

const uint32_t kMaxSize = 4096;   // larger textures are downsampled via their mip chain

template <class T> struct ComPtr
{
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

bool decode_wic(IWICImagingFactory* f, IWICBitmapDecoder* decoder, uint32_t& w, uint32_t& h,
                std::vector<uint8_t>& rgba)
{
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT uw, uh;
    conv->GetSize(&uw, &uh);
    if (!uw || !uh) return false;
    w = uw;
    h = uh;
    rgba.resize((size_t)w * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
}

bool decode_wic_file(IWICImagingFactory* f, const std::wstring& path, uint32_t& w, uint32_t& h,
                     std::vector<uint8_t>& rgba)
{
    ComPtr<IWICBitmapDecoder> decoder;
    if (!f || FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    return decode_wic(f, decoder.p, w, h, rgba);
}

bool decode_wic_memory(IWICImagingFactory* f, const std::vector<uint8_t>& data, uint32_t& w, uint32_t& h,
                       std::vector<uint8_t>& rgba)
{
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (!f || FAILED(f->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory((BYTE*)data.data(), (DWORD)data.size())) ||
        FAILED(f->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    return decode_wic(f, decoder.p, w, h, rgba);
}

// TGA: uncompressed or RLE; 8-bit grayscale, 24/32-bit BGR(A). Covers what DCC tools export.
bool decode_tga(const uint8_t* d, size_t size, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba)
{
    if (size < 18) return false;
    uint8_t idLen = d[0], cmapType = d[1], type = d[2], bpp = d[16], desc = d[17];
    w = d[12] | (d[13] << 8);
    h = d[14] | (d[15] << 8);
    bool rle = type >= 9;
    uint8_t base = rle ? type - 8 : type;
    if (cmapType || (base != 2 && base != 3) || !w || !h) return false;
    uint32_t bytes = bpp / 8;
    if (!((base == 2 && (bytes == 3 || bytes == 4)) || (base == 3 && bytes == 1))) return false;

    const uint8_t* p = d + 18 + idLen;
    const uint8_t* end = d + size;
    size_t count = (size_t)w * h;
    rgba.resize(count * 4);
    auto put = [&](size_t i, const uint8_t* px) {
        uint8_t* o = &rgba[i * 4];
        if (bytes == 1) { o[0] = o[1] = o[2] = px[0]; o[3] = 255; }
        else { o[0] = px[2]; o[1] = px[1]; o[2] = px[0]; o[3] = bytes == 4 ? px[3] : 255; }
    };
    for (size_t i = 0; i < count;) {
        if (!rle) {
            if (p + bytes > end) return false;
            put(i++, p);
            p += bytes;
            continue;
        }
        if (p >= end) return false;
        uint8_t hdr = *p++;
        size_t n = (hdr & 0x7F) + 1;
        if (i + n > count) n = count - i;
        if (hdr & 0x80) {
            if (p + bytes > end) return false;
            for (size_t k = 0; k < n; ++k) put(i++, p);
            p += bytes;
        } else {
            if (p + n * bytes > end) return false;
            for (size_t k = 0; k < n; ++k, p += bytes) put(i++, p);
        }
    }
    // TGA rows are bottom-up unless bit 5 of the descriptor is set.
    if (!(desc & 0x20)) {
        size_t row = (size_t)w * 4;
        std::vector<uint8_t> tmp(row);
        for (uint32_t y = 0; y < h / 2; ++y) {
            uint8_t* a = &rgba[y * row];
            uint8_t* b = &rgba[(h - 1 - y) * row];
            memcpy(tmp.data(), a, row);
            memcpy(a, b, row);
            memcpy(b, tmp.data(), row);
        }
    }
    return true;
}

bool read_file(const std::wstring& path, std::vector<uint8_t>& data)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 31);
    if (ok) {
        data.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size();
    }
    CloseHandle(f);
    return ok;
}

bool has_ext(const std::wstring& path, const wchar_t* ext)
{
    size_t n = wcslen(ext);
    return path.size() >= n && _wcsicmp(path.c_str() + path.size() - n, ext) == 0;
}

bool is_tga(const std::vector<uint8_t>& d)
{
    // TGA has no magic number; accept it when WIC could not decode and the header looks sane.
    return d.size() >= 18 && d[1] == 0 && (d[2] == 2 || d[2] == 3 || d[2] == 10 || d[2] == 11);
}

// 2x2 box filter; odd edges reuse the last row/column.
void downsample(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dw, uint32_t dh)
{
    for (uint32_t y = 0; y < dh; ++y) {
        uint32_t y0 = y * 2, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (uint32_t x = 0; x < dw; ++x) {
            uint32_t x0 = x * 2, x1 = x0 + 1 < sw ? x0 + 1 : x0;
            const uint8_t* a = src + ((size_t)y0 * sw + x0) * 4;
            const uint8_t* b = src + ((size_t)y0 * sw + x1) * 4;
            const uint8_t* c = src + ((size_t)y1 * sw + x0) * 4;
            const uint8_t* d = src + ((size_t)y1 * sw + x1) * 4;
            uint8_t* o = dst + ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; ++k) o[k] = (uint8_t)((a[k] + b[k] + c[k] + d[k] + 2) / 4);
        }
    }
}

void build_mips(uint32_t w, uint32_t h, std::vector<uint8_t>& level0, Image& out)
{
    // Halve until within the size cap, then keep every level down to 1x1.
    std::vector<uint8_t> cur = std::move(level0), next;
    while (w > kMaxSize || h > kMaxSize) {
        uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        next.resize((size_t)nw * nh * 4);
        downsample(cur.data(), w, h, next.data(), nw, nh);
        cur.swap(next);
        w = nw;
        h = nh;
    }
    out.width = w;
    out.height = h;

    size_t total = 0;
    for (uint32_t mw = w, mh = h;; mw = mw > 1 ? mw / 2 : 1, mh = mh > 1 ? mh / 2 : 1) {
        total += (size_t)mw * mh * 4;
        if (mw == 1 && mh == 1) break;
    }
    out.pixels.resize(total);
    out.offsets.clear();
    memcpy(out.pixels.data(), cur.data(), cur.size());
    out.offsets.push_back(0);
    size_t off = cur.size();
    for (uint32_t mw = w, mh = h; mw > 1 || mh > 1;) {
        uint32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
        downsample(out.pixels.data() + out.offsets.back(), mw, mh, out.pixels.data() + off, nw, nh);
        out.offsets.push_back(off);
        off += (size_t)nw * nh * 4;
        mw = nw;
        mh = nh;
    }
}

} // namespace

bool load_image(const TextureSource& src, Image& out)
{
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));

    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool ok = false;

    if (!src.embedded.empty()) {
        ok = decode_wic_memory(f.p, src.embedded, w, h, rgba) ||
             (is_tga(src.embedded) && decode_tga(src.embedded.data(), src.embedded.size(), w, h, rgba));
    }
    for (size_t i = 0; !ok && i < src.paths.size(); ++i) {
        const std::wstring& path = src.paths[i];
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (has_ext(path, L".tga")) {
            std::vector<uint8_t> data;
            ok = read_file(path, data) && decode_tga(data.data(), data.size(), w, h, rgba);
        } else {
            ok = decode_wic_file(f.p, path, w, h, rgba);
        }
    }
    if (!ok) return false;

    // Cut-out detection: a noticeable share of fully transparent pixels and few semi-transparent
    // ones. Smooth alpha (glass) stays opaque rather than being punched out.
    size_t clear = 0, partial = 0, count = (size_t)w * h;
    for (size_t i = 0; i < count; ++i) {
        uint8_t a = rgba[i * 4 + 3];
        if (a < 16) ++clear;
        else if (a < 240) ++partial;
    }
    out.cutout = clear > count / 50 && partial < clear;

    build_mips(w, h, rgba, out);
    return true;
}
