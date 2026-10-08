#pragma once

#include <string>
#include <vector>

namespace sf {

// A minimal parsed URL (http/https only).
struct Url {
    std::wstring scheme;  // "http" or "https"
    std::wstring host;    // lowercase, without port
    std::wstring port;    // empty if default
    std::wstring path;    // "/..." or empty
    std::wstring query;   // without '?', or empty
};

bool parseUrl(const std::wstring& text, Url& out);
std::wstring resolveUrl(const std::wstring& base, const std::wstring& ref);
std::wstring originRoot(const Url& u);
std::wstring hostFromUrl(const std::wstring& url);

std::wstring trim(const std::wstring& s);
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);  // for log messages (always UTF-8)

// Href attributes of <link rel="...icon..."> tags, in document order.
std::vector<std::string> findIconLinkHrefs(const std::string& html);

std::wstring appDataDir();              // %LOCALAPPDATA%\ShowFavicon
std::vector<std::wstring> loadSites();  // one URL per line from sites.txt
void saveSites(const std::vector<std::wstring>& sites);  // writes sites.txt

}  // namespace sf
