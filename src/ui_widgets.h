#pragma once
#include "ui.h"
#include "imgui.h"

namespace ui_detail {
enum class Action { Neutral, Primary, Danger, Quiet };
extern ImFont* g_fontMono;
extern ImFont* g_fontCaption;
extern ImFont* g_fontUptime;
extern ImTextureID g_diagnosticIcons[];
extern const ImVec4 kText, kMuted, kAccentL, kGreen, kRed, kWarn, kCard, kField, kDivider, kAccent;
float S(float value);
ImVec4 C(int r, int g, int b, float a = 1.0f);
ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t);
ImVec4 WithAlpha(ImVec4 color, float alpha);
ImU32 Ink(const ImVec4& color);
float Animate(ImGuiID id, float target, float initial, float seconds = .13f);
void Glass(ImDrawList* draw, ImVec2 a, ImVec2 b, float radius, ImVec4 tint, float hover,
           float warmth = 0, float glow = 0);
void Tooltip(const char* text);
void Label(const char* text);
void Section(const char* text);
void BeginCard(const char* id, ImVec2 size, ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
void WrappedColored(const ImVec4& color, const std::string& text);
void StatusDot(const ImVec4& color, const char* text);
bool Button(const char* label, ImVec2 size = {}, Action action = Action::Neutral);
bool Toggle(const char* label, bool value);
int Segments(const char* id, const char* const* labels, int count, int selected, ImVec2 size, bool navigation = false);

void DrawStrategyPicker(AppState& app);
void DrawUptime(AppState& app, float width);
void DrawMonitoringControls(AppState& app);
void DrawConnectionHistory(AppState& app);
void DrawLauncherUpdates(AppState& app, HWND owner);
void DrawBackupTools(AppState& app, HWND owner);
void DrawBetaNote();
void DrawNotificationBell(AppState& app, const ImVec2& origin);
void DrawToasts(AppState& app, float footerHeight);
}
