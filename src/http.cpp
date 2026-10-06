#include "http.h"

#include "util.h"

#include <windows.h>
#include <winhttp.h>

namespace sf {
namespace {

const wchar_t kUserAgent[] =
    L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    L"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36\r\n";

}  // namespace

bool fetch(const std::wstring& initialUrl, std::vector<std::uint8_t>& body,
           std::wstring& finalUrl) {
    HINTERNET hSession =
        WinHttpOpen(L"ShowFavicon/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    std::wstring url = initialUrl;
    bool ok = false;
    const int kMaxRedirects = 10;

    for (int attempt = 0; attempt <= kMaxRedirects && !ok; ++attempt) {
        URL_COMPONENTS uc = {};
        uc.dwStructSize = sizeof(uc);
        wchar_t scheme[16] = {};
        wchar_t host[256] = {};
        wchar_t path[8192] = {};
        wchar_t extra[8192] = {};
        uc.lpszScheme = scheme;
        uc.dwSchemeLength = 16;
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 8192;
        uc.lpszExtraInfo = extra;
        uc.dwExtraInfoLength = 8192;

        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc))
            break;

        bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
        INTERNET_PORT port =
            uc.nPort ? uc.nPort
                     : (secure ? INTERNET_DEFAULT_HTTPS_PORT
                               : INTERNET_DEFAULT_HTTP_PORT);

        HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
        if (!hConnect) break;

        std::wstring object = path;
        object += extra;

        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest =
            WinHttpOpenRequest(hConnect, L"GET", object.c_str(), nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               flags);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            break;
        }

        WinHttpSetTimeouts(hRequest, 5000, 5000, 5000, 15000);
        WinHttpAddRequestHeaders(hRequest, kUserAgent, static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD);

        bool redirected = false;
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            WinHttpQueryHeaders(hRequest,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                WINHTTP_NO_HEADER_INDEX);

            if (status >= 300 && status <= 399) {
                wchar_t loc[4096] = {};
                DWORD locSize = sizeof(loc);
                if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                        WINHTTP_HEADER_NAME_BY_INDEX, loc, &locSize,
                                        WINHTTP_NO_HEADER_INDEX) &&
                    locSize >= sizeof(wchar_t)) {
                    std::wstring location(loc, locSize / sizeof(wchar_t));
                    url = resolveUrl(url, location);
                    redirected = true;
                }
            } else if (status >= 200 && status < 300) {
                body.clear();
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(hRequest, &available) &&
                       available > 0) {
                    size_t oldSize = body.size();
                    body.resize(oldSize + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, body.data() + oldSize, available,
                                         &read))
                        break;
                    body.resize(oldSize + read);
                    if (read == 0) break;
                }
                finalUrl = url;
                ok = true;
            }
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);

        if (!redirected && !ok)
            break;  // only redirects keep the loop going
    }

    WinHttpCloseHandle(hSession);
    return ok;
}

}  // namespace sf
