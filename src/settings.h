#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace sf {

// Modal settings dialog. Edits `sites` in place (add/remove in the UI).
// Returns IDOK when confirmed, IDCANCEL otherwise.
int showSettingsDialog(HINSTANCE hInstance, HWND owner,
                       std::vector<std::wstring>& sites);

}  // namespace sf
