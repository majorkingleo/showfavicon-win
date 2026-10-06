#include "icon.h"

#include "http.h"
#include "util.h"

#include <objbase.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>

// nanosvg is header-only; the implementations live in this translation unit.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

namespace sf {
namespace {

IWICImagingFactory* wicFactory() {
    static IWICImagingFactory* factory = nullptr;
    if (!factory) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IWICImagingFactory,
                         reinterpret_cast<void**>(&factory));
    }
    return factory;
}

bool looksLikeSvg(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return false;
    size_t n = std::min<size_t>(bytes.size(), 1024);
    std::string lower;
    lower.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        char c = static_cast<char>(bytes[i]);
        lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    }
    return lower.find("<svg") != std::string::npos ||
           lower.find("<?xml") != std::string::npos ||
           lower.find("<!doctype svg") != std::string::npos;
}

bool wicDecode(const std::vector<std::uint8_t>& bytes, RgbaImage& out) {
    IWICImagingFactory* factory = wicFactory();
    if (!factory) return false;

    IStream* stream =
        SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
    if (!stream) return false;

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = factory->CreateDecoderFromStream(stream, nullptr,
                                                  WICDecodeMetadataCacheOnLoad,
                                                  &decoder);
    if (FAILED(hr)) {
        stream->Release();
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        decoder->Release();
        stream->Release();
        return false;
    }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) {
        frame->Release();
        decoder->Release();
        stream->Release();
        return false;
    }

    IWICBitmapSource* src = nullptr;
    hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame, &src);
    frame->Release();
    if (FAILED(hr)) {
        decoder->Release();
        stream->Release();
        return false;
    }

    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.pixels.resize(static_cast<size_t>(w) * h);
    hr = src->CopyPixels(nullptr, w * 4,
                         static_cast<UINT>(out.pixels.size() * sizeof(std::uint32_t)),
                         reinterpret_cast<BYTE*>(out.pixels.data()));

    src->Release();
    decoder->Release();
    stream->Release();
    return SUCCEEDED(hr);
}

bool svgDecode(const std::vector<std::uint8_t>& bytes, RgbaImage& out) {
    if (bytes.empty()) return false;
    std::string data(bytes.begin(), bytes.end());

    NSVGimage* image = nsvgParse(const_cast<char*>(data.c_str()), "px", 96.0f);
    if (!image) return false;

    const int size = 32;
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return false;
    }

    std::vector<unsigned char> buf(static_cast<size_t>(size) * size * 4, 0);
    nsvgRasterize(rast, image, 0, 0, 1.0f, buf.data(), size, size, size * 4);

    out.width = size;
    out.height = size;
    out.pixels.resize(static_cast<size_t>(size) * size);
    for (int i = 0; i < size * size; ++i) {
        unsigned char r = buf[i * 4 + 0];
        unsigned char g = buf[i * 4 + 1];
        unsigned char b = buf[i * 4 + 2];
        unsigned char a = buf[i * 4 + 3];
        out.pixels[i] = (static_cast<std::uint32_t>(a) << 24) |
                        (static_cast<std::uint32_t>(r) << 16) |
                        (static_cast<std::uint32_t>(g) << 8) |
                        static_cast<std::uint32_t>(b);
    }

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    return true;
}

bool decodeImage(const std::vector<std::uint8_t>& bytes, RgbaImage& out) {
    if (looksLikeSvg(bytes)) return svgDecode(bytes, out);
    if (wicDecode(bytes, out)) return true;
    return svgDecode(bytes, out);  // in case WIC missed an SVG it cannot handle
}

bool ensureDir(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    size_t pos = path.find(L'\\');
    while (pos != std::wstring::npos) {
        std::wstring cur = path.substr(0, pos);
        if (!cur.empty()) CreateDirectoryW(cur.c_str(), nullptr);
        pos = path.find(L'\\', pos + 1);
    }
    CreateDirectoryW(path.c_str(), nullptr);
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

std::wstring iconCacheDir() {
    return appDataDir() + L"\\icons";
}

HICON imageToHicon(const RgbaImage& img) {
    if (img.width <= 0 || img.height <= 0 || img.pixels.empty()) return nullptr;

    HDC hdc = GetDC(nullptr);

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = img.width;
    bi.bV5Height = -img.height;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP hbmColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmColor && bits)
        std::memcpy(bits, img.pixels.data(),
                    img.pixels.size() * sizeof(std::uint32_t));

    HBITMAP hbmMask = CreateBitmap(img.width, img.height, 1, 1, nullptr);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbmColor;
    HICON hIcon = CreateIconIndirect(&ii);

    if (hbmColor) DeleteObject(hbmColor);
    if (hbmMask) DeleteObject(hbmMask);
    ReleaseDC(nullptr, hdc);
    return hIcon;
}

bool writePngFile(const RgbaImage& img, const std::wstring& path) {
    IWICImagingFactory* factory = wicFactory();
    if (!factory || img.width <= 0 || img.height <= 0) return false;

    size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        ensureDir(path.substr(0, slash));

    std::wstring tmp = path + L".tmp";
    IStream* stream = nullptr;
    if (FAILED(SHCreateStreamOnFileW(tmp.c_str(), STGM_CREATE | STGM_WRITE, &stream)))
        return false;

    IWICBitmapEncoder* encoder = nullptr;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
        stream->Release();
        return false;
    }
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
        encoder->Release();
        stream->Release();
        return false;
    }

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) {
        encoder->Release();
        stream->Release();
        return false;
    }
    if (FAILED(frame->Initialize(props))) {
        frame->Release();
        if (props) props->Release();
        encoder->Release();
        stream->Release();
        return false;
    }

    frame->SetSize(static_cast<UINT>(img.width), static_cast<UINT>(img.height));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);

    std::vector<std::uint32_t> px = img.pixels;  // CreateBitmapFromMemory wants non-const
    IWICBitmap* bitmap = nullptr;
    if (SUCCEEDED(factory->CreateBitmapFromMemory(
            static_cast<UINT>(img.width), static_cast<UINT>(img.height),
            GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(img.width * 4),
            static_cast<UINT>(px.size() * sizeof(std::uint32_t)),
            reinterpret_cast<BYTE*>(px.data()), &bitmap))) {
        frame->WriteSource(bitmap, nullptr);
        bitmap->Release();
    }

    frame->Commit();
    frame->Release();
    if (props) props->Release();
    encoder->Commit();
    encoder->Release();
    stream->Release();

    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    return true;
}

bool loadPngFile(const std::wstring& path, RgbaImage& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD sizeHigh = 0;
    DWORD size = GetFileSize(h, &sizeHigh);
    if (size == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        CloseHandle(h);
        return false;
    }

    std::vector<std::uint8_t> bytes(size);
    DWORD read = 0;
    bool ok = ReadFile(h, bytes.data(), size, &read, nullptr) && read == size;
    CloseHandle(h);
    if (!ok) return false;
    return wicDecode(bytes, out);
}

void grayscale(RgbaImage& img, float alphaScale) {
    for (auto& p : img.pixels) {
        std::uint32_t b = p & 0xFFu;
        std::uint32_t g = (p >> 8) & 0xFFu;
        std::uint32_t r = (p >> 16) & 0xFFu;
        std::uint32_t a = (p >> 24) & 0xFFu;
        std::uint32_t l =
            static_cast<std::uint32_t>(0.299f * r + 0.587f * g + 0.114f * b);
        a = static_cast<std::uint32_t>(a * alphaScale);
        p = (a << 24) | (l << 16) | (l << 8) | l;
    }
}

void setFailureFlag(const std::wstring& cacheFile, bool failed) {
    std::wstring flag = cacheFile + L".failed";
    if (failed) {
        HANDLE h = CreateFileW(flag.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    } else {
        DeleteFileW(flag.c_str());
    }
}

bool fetchIconForSite(const std::wstring& siteUrl, RgbaImage& out) {
    std::vector<std::uint8_t> page;
    std::wstring pageUrl;
    if (!fetch(siteUrl, page, pageUrl)) return false;

    std::string html(page.begin(), page.end());

    std::vector<std::wstring> candidates;
    for (const auto& href : findIconLinkHrefs(html))
        candidates.push_back(resolveUrl(pageUrl, utf8ToWide(href)));

    Url u;
    if (parseUrl(pageUrl, u))
        candidates.push_back(originRoot(u) + L"/favicon.ico");

    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        std::vector<std::uint8_t> bytes;
        std::wstring finalCandidate;
        if (!fetch(candidate, bytes, finalCandidate)) continue;
        if (decodeImage(bytes, out)) return true;
    }
    return false;
}

bool fetchOrCachedIcon(const std::wstring& siteUrl, const std::wstring& cacheFile,
                       RgbaImage& out) {
    if (fetchIconForSite(siteUrl, out)) {
        writePngFile(out, cacheFile);
        setFailureFlag(cacheFile, false);
        return true;
    }
    if (loadPngFile(cacheFile, out)) {
        grayscale(out, 0.55f);
        setFailureFlag(cacheFile, true);
        return true;
    }
    setFailureFlag(cacheFile, true);
    return false;
}

}  // namespace sf
