#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sf {

// Fetch a URL, following redirects manually. On success returns true with the
// body bytes and the final (post-redirect) URL.
bool fetch(const std::wstring& url, std::vector<std::uint8_t>& body,
           std::wstring& finalUrl);

}  // namespace sf
