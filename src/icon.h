#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sf {

// A decoded image: 32-bit BGRA pixels, top-down, row-major.
struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
};

// Fetch the page, resolve icon candidates (declared <link rel=icon> first,
// /favicon.ico at the origin last), fetch and decode the icon. On success fills
// `out`.
bool fetchIconForSite(const std::wstring& siteUrl, RgbaImage& out);

// Decode a cached PNG file into `out`.
bool loadPngFile(const std::wstring& path, RgbaImage& out);

// In-place grayscale with scaled alpha (offline look).
void grayscale(RgbaImage& img, float alphaScale);

// Convert a decoded image into an HICON (caller owns; use DestroyIcon).
HICON imageToHicon(const RgbaImage& img);

// Write a decoded image to a PNG file (temp file + rename).
bool writePngFile(const RgbaImage& img, const std::wstring& path);

std::wstring iconCacheDir();  // %LOCALAPPDATA%\ShowFavicon\icons

// Set or clear the "last fetch failed" marker next to the cache file.
void setFailureFlag(const std::wstring& cacheFile, bool failed);

}  // namespace sf
