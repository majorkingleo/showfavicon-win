#include "settings.h"

#include "http.h"
#include "resource.h"
#include "util.h"

#include <CpputilsDebug.h>
#include <format.h>

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

// Row currently loaded into the URL field for editing, or -1 when the field is
// used to add a new entry. The Edit button and a double-click on a row set it.
int g_editIndex = -1;

void addUrl(HWND dlg, const std::wstring& url);
void addDroppedText(HWND dlg, const wchar_t* text);

// OLE drop target for text, so URLs dragged from a browser address bar work.
class TextDropTarget final : public IDropTarget {
public:
    explicit TextDropTarget(HWND hwnd) : m_hwnd(hwnd), m_ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
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
        if (r == 0) {
            delete this;
        }
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
    if (u.empty()) {
        return;
    }
    for (const auto& s : *g_sites) {
        if (s == u) {
            return;
        }
    }
    g_sites->push_back(u);
    SendDlgItemMessageW(dlg, IDC_SITE_LIST, LB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM>(u.c_str()));

    CPPDEBUG( Tools::wformat( L"settings: added %s", u ) );
}

void addDroppedText(HWND dlg, const wchar_t* text) {
    std::wstring s(text);
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && iswspace(s[i])) {
            ++i;
        }
        size_t start = i;
        while (i < s.size() && !iswspace(s[i])) {
            ++i;
        }
        if (i > start) {
            std::wstring tok = s.substr(start, i - start);
            if (tok.find(L"://") != std::wstring::npos) {
                addUrl(dlg, tok);
            }
        }
    }
}

// Load the selected row into the URL field and switch the Add button into its
// Save role. Shared by the Edit button and a double-click on a row.
void beginEdit(HWND dlg) {
    LRESULT sel = SendDlgItemMessageW(dlg, IDC_SITE_LIST, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        return;  // nothing selected
    }

    int len = static_cast<int>(
        SendDlgItemMessageW(dlg, IDC_SITE_LIST, LB_GETTEXTLEN, sel, 0));
    if (len <= 0) {
        return;
    }

    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    SendDlgItemMessageW(dlg, IDC_SITE_LIST, LB_GETTEXT, sel,
                        reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(len));

    g_editIndex = static_cast<int>(sel);
    SetDlgItemTextW(dlg, IDC_SITE_EDIT, text.c_str());
    SetDlgItemTextW(dlg, IDC_ADD, L"&Save");

    // Put the caret in the field with the whole URL selected, so typing
    // replaces it.
    HWND edit = GetDlgItem(dlg, IDC_SITE_EDIT);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);

    CPPDEBUG( Tools::wformat( L"settings: editing index %d (%s)",
                              g_editIndex, text ) );
}

// Leave edit mode: the field goes back to adding a new entry.
void endEdit(HWND dlg) {
    g_editIndex = -1;
    SetDlgItemTextW(dlg, IDC_SITE_EDIT, L"");
    SetDlgItemTextW(dlg, IDC_ADD, L"&Add");
}

// Add and Save test the URL before accepting it. Anything that is not an
// http/https URL with a host is rejected outright. A URL that merely cannot be
// reached is only warned about: a site that is down right now still has to stay
// configurable. Returns false only for input that must not be added.
bool testUrl(HWND dlg, const std::wstring& url) {
    sf::Url parsed;
    if (!sf::parseUrl(url, parsed) || parsed.host.empty()) {
        MessageBoxW(dlg, L"That is not an http:// or https:// URL with a host name.",
                    L"ShowFavicon", MB_ICONERROR);
        return false;
    }

    // The fetch blocks this thread for up to the WinHTTP timeouts, so show that
    // something is happening.
    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::vector<std::uint8_t> body;
    std::wstring finalUrl;
    const bool reachable = sf::fetch(url, body, finalUrl);

    SetCursor(previous);

    if (!reachable) {
        const std::wstring message =
            L"Could not reach\n" + url +
            L"\n\nThe entry is added anyway. It may work once the site is up "
            L"again - the icon stays grayed until then.";
        MessageBoxW(dlg, message.c_str(), L"ShowFavicon", MB_ICONWARNING);
    }

    return true;
}

// The Run entry is only touched on OK, so Cancel leaves it as it was.
void applyAutoStart(HWND dlg) {
    const bool wanted = IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED;
    if (wanted == autoStartEnabled()) {
        return;
    }
    if (!setAutoStart(wanted)) {
        CPPDEBUG( "settings: could not update the autostart entry" );
        MessageBoxW(dlg, L"ShowFavicon could not update the Windows autostart entry.",
                    L"ShowFavicon", MB_ICONWARNING);
        return;
    }
    CPPDEBUG( Tools::wformat( L"settings: autostart %s",
                              wanted ? L"enabled" : L"disabled" ) );
}

// ShowFavicon owns no visible window, so a dialog it creates can end up behind
// whatever is in front. Attaching to the foreground thread first is what makes
// SetForegroundWindow take effect from a message handler.
void bringToFront(HWND hwnd) {
    HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread =
        foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD thisThread = GetCurrentThreadId();
    const bool attach = foregroundThread != 0 && foregroundThread != thisThread;

    if (attach) {
        AttachThreadInput(foregroundThread, thisThread, TRUE);
    }

    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);

    if (attach) {
        AttachThreadInput(foregroundThread, thisThread, FALSE);
    }
}

bool endsWithIgnoreCase(const std::wstring& s, const wchar_t* suffix) {
    size_t n = std::wcslen(suffix);
    if (s.size() < n) {
        return false;
    }
    std::wstring tail = s.substr(s.size() - n);
    for (auto& c : tail) {
        if (c >= L'A' && c <= L'Z') {
            c += L'a' - L'A';
        }
    }
    return tail == suffix;
}

INT_PTR CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_dlg = hwnd;
            g_editIndex = -1;
            CheckDlgButton(hwnd, IDC_AUTOSTART,
                           autoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
            for (const auto& s : *g_sites) {
                SendDlgItemMessageW(hwnd, IDC_SITE_LIST, LB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(s.c_str()));
            }
            DragAcceptFiles(hwnd, TRUE);
            g_dropTarget = new TextDropTarget(hwnd);
            RegisterDragDrop(hwnd, g_dropTarget);
            bringToFront(hwnd);
            return TRUE;
        }

        case WM_COMMAND:
            // A double-click on a row edits it, exactly like the Edit button.
            if (LOWORD(wParam) == IDC_SITE_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                beginEdit(hwnd);
                return TRUE;
            }
            switch (LOWORD(wParam)) {
                case IDC_ADD: {
                    wchar_t buf[2048] = {};
                    GetDlgItemTextW(hwnd, IDC_SITE_EDIT, buf, 2048);
                    std::wstring u = trim(buf);
                    if (u.empty()) {
                        return TRUE;
                    }
                    if (!testUrl(hwnd, u)) {
                        return TRUE;  // unusable URL: do not add it
                    }
                    if (g_editIndex >= 0 &&
                        static_cast<size_t>(g_editIndex) < g_sites->size()) {
                        // Save: replace the row that Edit / double-click loaded.
                        (*g_sites)[static_cast<size_t>(g_editIndex)] = u;
                        SendDlgItemMessageW(hwnd, IDC_SITE_LIST, LB_DELETESTRING,
                                            static_cast<WPARAM>(g_editIndex), 0);
                        SendDlgItemMessageW(hwnd, IDC_SITE_LIST, LB_INSERTSTRING,
                                            static_cast<WPARAM>(g_editIndex),
                                            reinterpret_cast<LPARAM>(u.c_str()));
                        SendDlgItemMessageW(hwnd, IDC_SITE_LIST, LB_SETCURSEL,
                                            static_cast<WPARAM>(g_editIndex), 0);
                        CPPDEBUG( Tools::wformat( L"settings: updated index %d to %s",
                                                  g_editIndex, u ) );
                        endEdit(hwnd);
                    } else {
                        addUrl(hwnd, buf);
                        SetDlgItemTextW(hwnd, IDC_SITE_EDIT, L"");
                    }
                    return TRUE;
                }
                case IDC_EDIT_SITE:
                    beginEdit(hwnd);
                    return TRUE;
                case IDC_REMOVE: {
                    LRESULT sel =
                        SendDlgItemMessageW(hwnd, IDC_SITE_LIST, LB_GETCURSEL, 0, 0);
                    if (sel != LB_ERR) {
                        CPPDEBUG( Tools::format( "settings: removed index %d",
                                                 static_cast<int>(sel) ) );
                        SendDlgItemMessageW(hwnd, IDC_SITE_LIST, LB_DELETESTRING,
                                            static_cast<WPARAM>(sel), 0);
                        g_sites->erase(g_sites->begin() + static_cast<size_t>(sel));
                        // Keep the edit target on the same URL, or leave edit
                        // mode when that very row was the one being edited.
                        if (g_editIndex == static_cast<int>(sel)) {
                            endEdit(hwnd);
                        } else if (g_editIndex > static_cast<int>(sel)) {
                            --g_editIndex;
                        }
                    }
                    return TRUE;
                }
                case IDOK:
                    applyAutoStart(hwnd);
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
                        if (url[0]) {
                            addUrl(hwnd, url);
                        }
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
    g_editIndex = -1;
    return static_cast<int>(res);
}

bool settingsDialogOpen() {
    return g_dlg != nullptr;
}

void raiseSettingsDialog() {
    if (g_dlg != nullptr) {
        bringToFront(g_dlg);
    }
}

}  // namespace sf
