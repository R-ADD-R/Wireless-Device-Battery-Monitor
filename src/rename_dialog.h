#pragma once
#include <windows.h>
#include <string>

// Native, modal Win32 name editor. Returns false if the user cancels.
// No shell or external process is used.
bool PromptDeviceName(HWND owner, const std::wstring& currentName, std::wstring& result);

// Edits a percentage (0 to 100). Only changes threshold when the user saves.
bool PromptBatteryThreshold(HWND owner, int& threshold);
