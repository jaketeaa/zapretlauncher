#pragma once
#include <windows.h>

struct AppState;

namespace tray_panel {
bool Show(HWND launcher, AppState& app, const RECT& anchor);
void RefreshUptime();
void Dismiss();
void Shutdown();
}
