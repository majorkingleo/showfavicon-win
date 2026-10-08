#include "util.h"

#include <windows.h>
#include <shlobj.h>

// cpputils ships the UTF-8 conversions (src/cpputils, see src/tools_config.h).
// Utf8Util is strict, so the two wrappers below pair it with replace_invalid.
#include <utf8.h>
#include <utf8_util.h>

#include <algorithm>
#include <exception>

namespace sf {
namespace {

std::wstring asciiLower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) {
        if (c >= L'A' && c <= L'Z') {
            c += L'a' - L'A';
        }
    }
    return r;
}

std::string asciiLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    return r;
}

std::string htmlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") {
                    out += '&';
                } else if (ent == "lt") {
                    out += '<';
                } else if (ent == "gt") {
                    out += '>';
                } else if (ent == "quot") {
                    out += '"';
                } else if (ent == "apos" || ent == "#39") {
                    out += '\'';
                } else {
                    out.append(s, i, semi - i + 1);
                }
                i = semi + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

size_t ciFind(const std::string& hay, const std::string& needle, size_t from = 0) {
    std::string h = asciiLower(hay);
    std::string n = asciiLower(needle);
    return h.find(n, from);
}

bool ciContains(const std::string& s, const std::string& sub) {
    return ciFind(s, sub) != std::string::npos;
}

std::string getAttr(const std::string& tag, const std::string& name) {
    std::string lowerTag = asciiLower(tag);
    std::string needle = asciiLower(name) + "=";
    size_t p = lowerTag.find(needle);
    while (p != std::string::npos) {
        bool boundary = (p == 0) || (lowerTag[p - 1] == ' ') ||
                        (lowerTag[p - 1] == '\t') || (lowerTag[p - 1] == '\r') ||
                        (lowerTag[p - 1] == '\n') || (lowerTag[p - 1] == '/');
        if (boundary) {
            size_t q = p + needle.size();
            while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) {
                ++q;
            }
            if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                char quote = tag[q++];
                size_t end = tag.find(quote, q);
                if (end != std::string::npos) {
                    return htmlDecode(tag.substr(q, end - q));
                }
            } else {
                size_t end = q;
                while (end < tag.size() && tag[end] != ' ' && tag[end] != '\t' &&
                       tag[end] != '>' && tag[end] != '/') {
                    ++end;
                }
                return htmlDecode(tag.substr(q, end - q));
            }
        }
        p = lowerTag.find(needle, p + 1);
    }
    return {};
}

std::wstring normalizePath(const std::wstring& path) {
    std::vector<std::wstring> segs;
    std::wstring cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        wchar_t c = (i < path.size()) ? path[i] : L'/';
        if (c == L'/') {
            if (cur == L"..") {
                if (!segs.empty()) {
                    segs.pop_back();
                }
            } else if (!cur.empty() && cur != L".") {
                segs.push_back(cur);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }

    std::wstring out;
    for (const auto& s : segs) {
        out += L'/';
        out += s;
    }
    if (out.empty()) {
        out = L"/";
    }
    if (!path.empty() && path.back() == L'/' && out.back() != L'/') {
        out += L'/';
    }
    return out;
}

}  // namespace

bool parseUrl(const std::wstring& text, Url& out) {
    std::wstring t = text;
    size_t frag = t.find(L'#');
    if (frag != std::wstring::npos) {
        t = t.substr(0, frag);
    }

    size_t schemeEnd = t.find(L"://");
    if (schemeEnd == std::wstring::npos) {
        return false;
    }
    out.scheme = asciiLower(t.substr(0, schemeEnd));
    if (out.scheme != L"http" && out.scheme != L"https") {
        return false;
    }

    size_t authStart = schemeEnd + 3;
    size_t pathStart = t.find(L'/', authStart);
    size_t queryStart = t.find(L'?', authStart);

    size_t authEnd = t.size();
    if (pathStart != std::wstring::npos && pathStart < authEnd) {
        authEnd = pathStart;
    }
    if (queryStart != std::wstring::npos && queryStart < authEnd) {
        authEnd = queryStart;
    }

    std::wstring auth = t.substr(authStart, authEnd - authStart);
    size_t colon = auth.find(L':');
    if (colon != std::wstring::npos) {
        out.host = asciiLower(auth.substr(0, colon));
        out.port = auth.substr(colon + 1);
    } else {
        out.host = asciiLower(auth);
        out.port.clear();
    }

    out.path.clear();
    out.query.clear();
    if (pathStart != std::wstring::npos && pathStart < t.size()) {
        if (queryStart != std::wstring::npos && queryStart > pathStart) {
            out.path = t.substr(pathStart, queryStart - pathStart);
        } else {
            out.path = t.substr(pathStart);
        }
    }
    if (queryStart != std::wstring::npos && queryStart < t.size()) {
        out.query = t.substr(queryStart + 1);
    }

    return true;
}

std::wstring originRoot(const Url& u) {
    std::wstring r = u.scheme + L"://" + u.host;
    if (!u.port.empty()) {
        r += L":" + u.port;
    }
    return r;
}

std::wstring resolveUrl(const std::wstring& base, const std::wstring& ref) {
    std::wstring r = trim(ref);
    size_t frag = r.find(L'#');
    if (frag != std::wstring::npos) {
        r = r.substr(0, frag);
    }
    if (r.empty()) {
        return base;
    }

    if (r.find(L"://") != std::wstring::npos) {
        return r;
    }

    Url b;
    if (!parseUrl(base, b)) {
        return ref;
    }

    std::wstring root = originRoot(b);

    if (r.size() >= 2 && r[0] == L'/' && r[1] == L'/') {
        return b.scheme + L":" + r;
    }

    if (r[0] == L'/') {
        return root + normalizePath(r);
    }

    if (r[0] == L'?') {
        std::wstring p = b.path.empty() ? L"/" : b.path;
        return root + p + r;
    }

    std::wstring dir = L"/";
    size_t slash = b.path.find_last_of(L'/');
    if (slash != std::wstring::npos) {
        dir = b.path.substr(0, slash + 1);
    }
    return root + normalizePath(dir + r);
}

std::wstring hostFromUrl(const std::wstring& url) {
    Url u;
    if (parseUrl(url, u)) {
        return u.host;
    }
    return {};
}

std::wstring trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) {
        ++a;
    }
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' ||
                     s[b - 1] == L'\n')) {
        --b;
    }
    return s.substr(a, b - a);
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    // Utf8Util would throw utf8::invalid_utf8 here; replace_invalid hands it
    // valid UTF-8, with bad sequences becoming U+FFFD - what the
    // MultiByteToWideChar call this replaces used to produce.
    return Tools::Utf8Util::utf8toWString(utf8::replace_invalid(s));
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    try {
        return Tools::Utf8Util::wStringToUtf8(s);
    } catch (const std::exception&) {
        // An unpaired surrogate makes Utf8Util throw utf8::invalid_utf16. The
        // only caller writes sites.txt from the settings dialog, where an
        // escaping exception would unwind out of the window procedure, so
        // substitute the surrogates and keep a usable file.
        std::wstring clean = s;
        for (wchar_t& c : clean) {
            if (c >= 0xD800 && c <= 0xDFFF) {
                c = L'?';
            }
        }
        return Tools::Utf8Util::wStringToUtf8(clean);
    }
}

void nameCurrentThread(const wchar_t* name) {
    // Available since Windows 10 1607; on anything older the call just fails,
    // which costs nothing.
    SetThreadDescription(GetCurrentThread(), name);
}

std::vector<std::string> findIconLinkHrefs(const std::string& html) {
    std::vector<std::string> hrefs;
    size_t pos = 0;
    while (true) {
        size_t p = ciFind(html, "<link", pos);
        if (p == std::string::npos) {
            break;
        }
        size_t end = html.find('>', p);
        if (end == std::string::npos) {
            break;
        }
        std::string tag = html.substr(p, end - p + 1);
        std::string rel = getAttr(tag, "rel");
        std::string href = getAttr(tag, "href");
        if (!href.empty() && ciContains(rel, "icon")) {
            hrefs.push_back(href);
        }
        pos = end + 1;
    }
    return hrefs;
}

std::wstring appDataDir() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        return std::wstring(path) + L"\\ShowFavicon";
    }
    return L"ShowFavicon";
}

std::vector<std::wstring> loadSites() {
    std::vector<std::wstring> sites;
    std::wstring file = appDataDir() + L"\\sites.txt";

    std::string bytes;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return sites;
    }

    char buf[4096];
    DWORD read = 0;
    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
        bytes.append(buf, read);
    }
    CloseHandle(h);

    std::wstring text = utf8ToWide(bytes);
    size_t i = 0;
    while (i <= text.size()) {
        size_t nl = text.find(L'\n', i);
        if (nl == std::wstring::npos) {
            nl = text.size();
        }
        std::wstring line = trim(text.substr(i, nl - i));
        if (!line.empty() && line[0] != L'#') {
            sites.push_back(line);
        }
        if (nl == text.size()) {
            break;
        }
        i = nl + 1;
    }
    return sites;
}

void saveSites(const std::vector<std::wstring>& sites) {
    std::wstring dir = appDataDir();
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
        }
    }

    std::string bytes;
    for (const auto& s : sites) {
        std::wstring line = trim(s);
        if (line.empty()) {
            continue;
        }
        bytes += wideToUtf8(line);
        bytes += "\r\n";
    }

    std::wstring file = dir + L"\\sites.txt";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                  nullptr);
        CloseHandle(h);
    }
}

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ShowFavicon";
constexpr wchar_t kSettingsKey[] = L"Software\\ShowFavicon";
constexpr wchar_t kPaletteXValue[] = L"PaletteX";
constexpr wchar_t kPaletteYValue[] = L"PaletteY";

// Where the shell remembers the notification-area choices, one subkey per icon.
constexpr wchar_t kNotifyIconKey[] = L"Control Panel\\NotifyIconSettings";
constexpr wchar_t kIconPathValue[] = L"ExecutablePath";
constexpr wchar_t kIconPromotedValue[] = L"IsPromoted";

std::wstring executablePath() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD cap = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    const DWORD n = GetModuleFileNameW(nullptr, buf, cap);
    if (n == 0 || n >= cap) {
        return {};
    }
    return std::wstring(buf, n);
}

}  // namespace

bool autoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD size = 0;
    const LONG res = RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, &size);
    RegCloseKey(key);

    return res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

bool setAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG res = ERROR_SUCCESS;

    if (enable) {
        const std::wstring exe = executablePath();
        if (exe.empty()) {
            RegCloseKey(key);
            return false;
        }
        // Quoted, so a path with spaces survives.
        const std::wstring quoted = L"\"" + exe + L"\"";
        res = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(quoted.c_str()),
                             static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        res = RegDeleteValueW(key, kRunValue);
        if (res == ERROR_FILE_NOT_FOUND) {
            res = ERROR_SUCCESS;  // already gone
        }
    }

    RegCloseKey(key);
    return res == ERROR_SUCCESS;
}

bool loadPalettePos(int& x, int& y) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    DWORD value = 0;

    const bool have_x =
        RegQueryValueExW(key, kPaletteXValue, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD;

    size = sizeof(DWORD);
    DWORD value_y = 0;
    const bool have_y = have_x &&
        RegQueryValueExW(key, kPaletteYValue, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value_y), &size) == ERROR_SUCCESS &&
        type == REG_DWORD;

    RegCloseKey(key);

    if (!have_y) {
        return false;
    }

    x = static_cast<int>(value);
    y = static_cast<int>(value_y);
    return true;
}

void savePalettePos(int x, int y) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    const DWORD value_x = static_cast<DWORD>(x);
    const DWORD value_y = static_cast<DWORD>(y);
    RegSetValueExW(key, kPaletteXValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value_x), sizeof(value_x));
    RegSetValueExW(key, kPaletteYValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value_y), sizeof(value_y));

    RegCloseKey(key);
}

int promoteNotificationIcons() {
    const std::wstring exe = executablePath();
    if (exe.empty()) {
        return 0;
    }

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyIconKey, 0,
                      KEY_ENUMERATE_SUB_KEYS, &root) != ERROR_SUCCESS) {
        return 0;
    }

    int changed = 0;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD nameLen = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
        if (RegEnumKeyExW(root, i, name, &nameLen, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS) {
            break;
        }

        HKEY item = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &item) !=
            ERROR_SUCCESS) {
            continue;
        }

        wchar_t path[2 * MAX_PATH] = {};
        DWORD size = sizeof(path);
        DWORD type = 0;
        const bool ours =
            RegQueryValueExW(item, kIconPathValue, nullptr, &type,
                             reinterpret_cast<BYTE*>(path),
                             &size) == ERROR_SUCCESS &&
            type == REG_SZ && lstrcmpiW(path, exe.c_str()) == 0;

        if (ours) {
            DWORD promoted = 0;
            DWORD valueSize = sizeof(promoted);
            const bool already =
                RegQueryValueExW(item, kIconPromotedValue, nullptr, &type,
                                 reinterpret_cast<BYTE*>(&promoted),
                                 &valueSize) == ERROR_SUCCESS &&
                type == REG_DWORD && promoted != 0;

            if (!already) {
                const DWORD one = 1;
                if (RegSetValueExW(item, kIconPromotedValue, 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&one),
                                   sizeof(one)) == ERROR_SUCCESS) {
                    ++changed;
                }
            }
        }

        RegCloseKey(item);
    }

    RegCloseKey(root);
    return changed;
}

}  // namespace sf
