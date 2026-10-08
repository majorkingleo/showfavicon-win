#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace sf {

// Modal settings dialog. Edits `sites` in place (add, edit and remove in the
// UI). Returns IDOK when confirmed, IDCANCEL otherwise.
int showSettingsDialog(HINSTANCE hInstance, HWND owner,
                       std::vector<std::wstring>& sites);

// True while the dialog is up. The modal loop still dispatches the thread's
// messages, so a second "show settings" request must not nest another dialog.
bool settingsDialogOpen();

// Brings the open dialog to the front. Does nothing when none is open.
void raiseSettingsDialog();

}  // namespace sf
