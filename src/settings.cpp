#include "settings.h"

#include "resource.h"
#include "util.h"

#include <windows.h>
#include <shellapi.h>
#include <ole2.h>
#include <shlobj.h>

#include <cwctype>
#include <cstring>
#include <string>
#include <vector>

namespace sf {
namespace {

HWND g_dlg = nullptr;
std::vector<std::wstring>* g_sites = nullptr;

void addUrl(HWND dlg, const std::wstring& url);
void addDroppedText(HWND dlg, const wchar_t* text);

// OLE drop target for text, so URLs dragged from a browser address bar work.
class TextDropTarget final : public IDropTarget {
public:
    explicit TextDropTarget(HWND hwnd) : m_hwnd(hwnd), m_ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP DragEnter(IDataObject*, DWORD, POINTL, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override { return S_OK; }

    STDMETHODIMP Drop(IDataObject* pData, DWORD, POINTL, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_NONE;
        FORMATETC fmt = {};
        fmt.cfFormat = CF_UNICODETEXT;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;

        STGMEDIUM med = {};
        if (SUCCEEDED(pData->GetData(&fmt, &med))) {
            if (med.hGlobal) {
                const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(med.hGlobal));
                if (text) {
                    addDroppedText(m_hwnd, text);
                    GlobalUnlock(med.hGlobal);
                    *pdwEffect = DROPEFFECT_COPY;
                }
            }
            ReleaseStgMedium(&med);
        }
        return S_OK;
    }

private:
    HWND m_hwnd;
    ULONG m_ref;
};

TextDropTarget* g_dropTarget = nullptr;

void addUrl(HWND dlg, const std::wstring& url) {
    std::wstring u = trim(url);
    if (u.empty()) return;
    for (const auto& s : *g_sites)
        if (s == u) return;
    g_sites->push_back(u);
    SendDlgItemMessageW(dlg, IDC_SITE_LIST, LB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM>(u.c_str()));
}

void addDroppedText(HWND dlg, const wchar_t* text) {
    std::wstring s(text);
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && iswspace(s[i])) ++i;
        size_t start = i;
        while (i < s.size() && !iswspace(s[i])) ++i;
        if (i > start) {
            std::wstring tok = s.substr(start, i - start);
            if (tok.find(L"://") != std::wstring::npos)
                addUrl(dlg, tok);
        }
    }
}

bool endsWithIgnoreCase(const std::wstring& s, const wchar_t* suffix) {
    size_t n = std::wcslen(suffix);
    if (s.size() < n) return false;
    std::wstring tail = s.substr(s.size() - n);
    for (auto& c : tail)
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return tail == suffix;
}

INT_PTR CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_dlg = hwnd;
            for (const auto& s : *g_sites)
                SendDlgItemMessageW(hwnd, IDC_SITE_LIST, LB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(s.c_str()));
            DragAcceptFiles(hwnd, TRUE);
            g_dropTarget = new TextDropTarget(hwnd);
            RegisterDragDrop(hwnd, g_dropTarget);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_ADD: {
                    wchar_t buf[2048] = {};
                    GetDlgItemTextW(hwnd, IDC_SITE_EDIT, buf, 2048);
                    addUrl(hwnd, buf);
                    SetDlgItemTextW(hwnd, IDC_SITE_EDIT, L"");
                    return TRUE;
                }
                case IDC_REMOVE: {
                    LRESULT sel =
                        SendDlgItemMessageW(hwnd, IDC_SITE_LIST, LB_GETCURSEL, 0, 0);
                    if (sel != LB_ERR) {
                        SendDlgItemMessageW(hwnd, IDC_SITE_LIST, LB_DELETESTRING,
                                            static_cast<WPARAM>(sel), 0);
                        g_sites->erase(g_sites->begin() + static_cast<size_t>(sel));
                    }
                    return TRUE;
                }
                case IDOK:
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH] = {};
                if (DragQueryFileW(hDrop, i, path, MAX_PATH)) {
                    std::wstring p(path);
                    if (endsWithIgnoreCase(p, L".url")) {
                        wchar_t url[2048] = {};
                        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"",
                                                 url, 2048, p.c_str());
                        if (url[0]) addUrl(hwnd, url);
                    } else {
                        addUrl(hwnd, p);
                    }
                }
            }
            DragFinish(hDrop);
            return TRUE;
        }

        case WM_DESTROY:
            if (g_dropTarget) {
                RevokeDragDrop(hwnd);
                g_dropTarget->Release();
                g_dropTarget = nullptr;
            }
            DragAcceptFiles(hwnd, FALSE);
            break;
    }
    return FALSE;
}

}  // namespace

int showSettingsDialog(HINSTANCE hInstance, HWND owner,
                       std::vector<std::wstring>& sites) {
    g_sites = &sites;
    INT_PTR res = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), owner,
                                  SettingsProc, 0);
    g_sites = nullptr;
    g_dlg = nullptr;
    return static_cast<int>(res);
}

}  // namespace sf
