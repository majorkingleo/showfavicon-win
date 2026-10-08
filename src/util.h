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

// UTF-8 <-> wide, built on cpputils' Tools::Utf8Util (src/cpputils) rather than
// the Win32 code page calls. Utf8Util is strict - it throws utf8::invalid_utf8 /
// invalid_utf16 - but every string that goes through here comes from somewhere
// untrusted: the hand-editable sites.txt, href bytes scraped from a fetched page
// (converted on the worker thread, where an escaping exception would call
// std::terminate), a URL typed into the dialog. These two therefore never throw.
//
// Log messages do not go through here at all: CPPDEBUG takes wide strings
// directly, and the backends convert them with Tools::Utf8Util themselves.
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);  // sites.txt on disk is UTF-8

// Names the calling thread, so it is identifiable in the debugger and in
// Process Explorer.
void nameCurrentThread(const wchar_t* name);

// Href attributes of <link rel="...icon..."> tags, in document order.
std::vector<std::string> findIconLinkHrefs(const std::string& html);

std::wstring appDataDir();              // %LOCALAPPDATA%\ShowFavicon
std::vector<std::wstring> loadSites();  // one URL per line from sites.txt
void saveSites(const std::vector<std::wstring>& sites);  // writes sites.txt

// The per-user Run entry that starts ShowFavicon at logon
// (HKCU\Software\Microsoft\Windows\CurrentVersion\Run). setAutoStart returns
// false when the registry could not be updated.
bool autoStartEnabled();
bool setAutoStart(bool enable);

// Palette window position, remembered between runs in
// HKCU\Software\ShowFavicon. loadPalettePos returns false when nothing is
// stored yet. Both tolerate negative values: a secondary monitor may sit left
// of or above the primary one.
bool loadPalettePos(int& x, int& y);
void savePalettePos(int x, int y);

// Windows 11 parks new notification-area icons in the overflow flyout. The
// shell keeps that choice per icon and per executable under
// HKCU\Control Panel\NotifyIconSettings; writing IsPromoted there is what the
// taskbar settings page itself does, and a running shell applies it at once, so
// an icon already in the tray leaves the overflow without Explorer restarting.
// Returns how many entries had to be changed.
int promoteNotificationIcons();

}  // namespace sf
