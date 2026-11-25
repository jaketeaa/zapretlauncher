#include "ui.h"
#include "utils.h"
#include "installation.h"
#include "glass_renderer.h"
#include "imgui.h"
#include "imgui_stdlib.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <imgui_internal.h>
#include <algorithm>
#include <cfloat>

namespace fs = std::filesystem;

// =====================================================================
//  AppState
// =====================================================================
AppState::AppState()
    : baseDir(util::DocumentsDataDir()),
      config(baseDir),
      updater(config.ZapretDir(), config.ConfigPath()),
      svcMgr(config.ZapretDir(), [this](const std::string& line) { log.AppendLog(line); }) {}

void AppState::SetNotice(const std::string& msg, bool isError) {
    notice = msg;
    noticeError = isError;
}

void AppState::SaveConfig() {
    std::string err;
    if (!config.Save(err)) SetNotice(err, true);
}

void AppState::RefreshFiles() {
    managedInstallation = installation::IsManaged(config.ZapretDir(), config.ConfigPath());
    listFiles = config.ListLists();
    strategies = config.ListStrategies();
    missing = updater.MissingFiles();
    if (!updater.Installing()) localVersion = updater.LocalTag();
    svcStatus = ServiceManager::ReadStatus(config.ZapretDir());

    // Выбранный general*.bat должен существовать в списке
    std::string& sel = config.settings.selectedBat;
    if (std::find(strategies.begin(), strategies.end(), sel) == strategies.end()) {
        std::string want = svcStatus.strategy.empty() ? std::string() : svcStatus.strategy + ".bat";
        if (!want.empty() && std::find(strategies.begin(), strategies.end(), want) != strategies.end()) sel = want;
        else if (std::find(strategies.begin(), strategies.end(), "general.bat") != strategies.end()) sel = "general.bat";
        else sel = strategies.empty() ? std::string() : strategies.front();
    }
}

void AppState::Init() {
    std::string error;
    if (!config.Load(error)) SetNotice(error, true);
    updater.SetDirectory(config.ZapretDir());
    svcMgr.SetDirectory(config.ZapretDir());
    termKillOnLeave = config.settings.terminalKillOnLeave;
    runWithWindows = util::IsAutostartEnabled();
    RefreshFiles();
    updater.Start(GithubUpdater::Task::Check);
}

void AppState::Tick() {
    if (autostartJob.valid() && autostartJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto failure = autostartJob.get();
        if (!failure.empty()) {
            runWithWindows = !runWithWindows;
            SetNotice(failure, true);
        }
    }
    if (diagnosticJob.valid() && diagnosticJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            diagnosticReport = diagnosticJob.get();
        } catch (const std::exception& e) {
            SetNotice(std::string("Ошибка диагностики: ") + e.what(), true);
        }
    }
    if (Deleting()) {
        if (removalJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        std::string error;
        try {
            error = removalJob.get();
        } catch (const std::exception& e) {
            error = std::string("Удаление не завершено: ") + e.what();
        }
        if (!term.IsRunning()) termStarted = true;
        if (error.empty()) {
            term.ClearDisplay();
            selectedList.clear();
            listBuffer.clear();
            listDirty = false;
        }
        const auto directory = config.ZapretDir();
        updater.SetDirectory(directory);
        svcMgr.SetDirectory(directory);
        RefreshFiles();
        if (error.empty()) {
            std::string saveError;
            if (config.Save(saveError)) SetNotice("Zapret не установлен. Путь установки сохранён.", false);
            else SetNotice("Zapret удалён. " + saveError, true);
        } else SetNotice(error, true);
        log.AppendLog("[лаунчер] " + notice + " " + util::ToUtf8(directory.wstring()));
    }
    unsigned long long now = GetTickCount64();
    unsigned gen = updater.Generation() + svcMgr.Generation();
    if (gen != lastGen || now - lastRefresh > 2000) {
        lastGen = gen;
        lastRefresh = now;
        RefreshFiles();
    }
}

void AppState::ChooseDirectory(HWND owner) {
    if (Deleting() || updater.Busy() || svcMgr.Busy() || term.IsRunning()) return;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        SetNotice("Не удалось запустить диалог выбора папки.", true);
        return;
    }
    struct ComScope { ~ComScope() { CoUninitialize(); } } com;
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        SetNotice("Не удалось открыть диалог выбора папки.", true);
        return;
    }
    dialog->SetTitle(L"Выберите папку, в которой будет создана папка zapret");
    DWORD options = 0;
    dialog->GetOptions(&options);
    result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IShellItem* initial = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(config.ZapretDir().c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial);
        initial->Release();
    }
    if (SUCCEEDED(result)) result = dialog->Show(owner);
    IShellItem* selected = nullptr;
    PWSTR raw = nullptr;
    if (SUCCEEDED(result)) result = dialog->GetResult(&selected);
    if (SUCCEEDED(result)) result = selected->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    fs::path directory;
    if (SUCCEEDED(result)) directory = raw;
    CoTaskMemFree(raw);
    if (selected) selected->Release();
    dialog->Release();
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(result) || directory.empty()) {
        SetNotice("Не удалось получить путь выбранной папки.", true);
        return;
    }
    directory = installation::ResolveSelection(directory);
    if (listDirty) {
        SetNotice("Сохраните изменения списка доменов перед сменой папки.", true);
        return;
    }

    std::string error;
    if (!installation::Prepare(directory, config.ConfigPath(), error)) {
        SetNotice(error, true);
        return;
    }
    if (directory == config.ZapretDir()) { RefreshFiles(); return; }
    const Settings previous = config.settings;
    config.settings.zapretPath = util::ToUtf8(directory.wstring());
    config.settings.selectedBat.clear();
    if (!config.Save(error)) {
        config.settings = previous;
        SetNotice(error, true);
        return;
    }

    updater.SetDirectory(directory);
    svcMgr.SetDirectory(directory);
    selectedList.clear();
    listBuffer.clear();
    term.ClearDisplay();
    termStarted = false;
    RefreshFiles();
    updater.Start(GithubUpdater::Task::Check);
    SetNotice(svcStatus.installed && !svcStatus.usesDirectory
        ? "Папка изменена. Служба продолжает использовать прежние файлы до смены стратегии."
        : "Рабочая папка zapret изменена.", false);
}

void AppState::RemoveInstallation() {
    if (Deleting() || updater.Busy() || svcMgr.Busy() || !managedInstallation) return;
    const auto directory = config.ZapretDir();
    const auto settings = config.ConfigPath();
    try {
        removalJob = std::async(std::launch::async, [this, directory, settings] {
            return installation::Remove(directory, settings, [this] { term.Stop(); });
        });
        log.AppendLog("[лаунчер] Удаление zapret: " + util::ToUtf8(directory.wstring()));
    } catch (const std::system_error&) {
        SetNotice("Не удалось запустить фоновое удаление.", true);
    }
}

void AppState::StartTerminal() {
    if (Deleting()) return;
    termStarted = true;
    std::string error;
    if (!term.Start(ConsoleSession::MakeServiceBatCommand(), config.ZapretDir(), error))
        SetNotice(error, true);
}

// =====================================================================
//  Тема и вспомогательные функции
// =====================================================================
namespace {

constexpr float kTitleBarH = 52.0f;
constexpr float kNavH = 56.0f;
constexpr float kCaptionButtonsWidth = 101.0f;
constexpr auto kNoScroll = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

float g_scale = 1.0f;
ImFont* g_fontTitle = nullptr;
ImFont* g_fontHeading = nullptr;
ImFont* g_fontMono = nullptr;
ImFont* g_fontButton = nullptr;
ImFont* g_fontCaption = nullptr;
const GlassRenderer* g_glass = nullptr;
ImTextureID g_launcherIcon = 0;

float S(float v) { return v * g_scale; }
ImVec4 C(int r, int g, int b, float a = 1.0f) { return {r / 255.f, g / 255.f, b / 255.f, a}; }

const ImVec4 kBackground = C(12, 12, 14);
const ImVec4 kTitleBg = C(16, 16, 18);
const ImVec4 kCard = C(23, 23, 25);
const ImVec4 kField = C(30, 30, 33);
const ImVec4 kButton = C(36, 36, 39);
const ImVec4 kHover = C(48, 48, 52);
const ImVec4 kDivider = C(41, 41, 45);
const ImVec4 kWhite = C(255, 255, 255);
const ImVec4 kText = C(242, 242, 244);
const ImVec4 kMuted = C(161, 161, 170);
const ImVec4 kInactive = C(113, 113, 122);
const ImVec4 kGreen = C(48, 209, 88);
const ImVec4 kRed = C(255, 69, 58);
const ImVec4 kAccent = C(255, 176, 32);
const ImVec4 kAccentL = C(255, 195, 77);
const ImVec4 kAccentPressed = C(217, 139, 18);
const ImVec4 kAmberMuted = C(56, 40, 16);
const ImVec4 kWarn = kAccentL;

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

ImU32 Ink(const ImVec4& color) { return ImGui::GetColorU32(color); }

float EaseOut(float t) {
    const float rest = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - rest * rest * rest;
}

struct Motion {
    float from;
    float to;
    float value;
    float elapsed;
    int frame;
};

std::unordered_map<ImGuiID, Motion> g_motion;

float Animate(ImGuiID id, float target, float initial, float seconds = 0.13f) {
    const int frame = ImGui::GetFrameCount();
    auto& motion = g_motion.try_emplace(id, Motion{initial, target, initial, 0, -1}).first->second;
    if (motion.to != target) {
        motion.from = motion.value;
        motion.to = target;
        motion.elapsed = 0;
    }
    if (motion.frame != frame) {
        motion.elapsed += ImGui::GetIO().DeltaTime;
        motion.value = motion.from + (motion.to - motion.from) * EaseOut(motion.elapsed / seconds);
        motion.frame = frame;
    }
    return motion.value;
}

struct Interaction { float hover, press, disabled; };

Interaction InteractionFor(ImGuiID id) {
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    return {Animate(id ^ 0x18aaU, ImGui::IsItemHovered() ? 1.0f : 0.0f, 0),
            Animate(id ^ 0xab19U, ImGui::IsItemActive() ? 1.0f : 0.0f, 0, 0.10f),
            Animate(id ^ 0x761fU, disabled ? 1.0f : 0.0f, disabled ? 1.0f : 0.0f)};
}

ImVec4 WithAlpha(ImVec4 color, float alpha) {
    color.w = alpha;
    return color;
}

void Glass(ImDrawList* draw, ImVec2 a, ImVec2 b, float radius, ImVec4 tint,
           float hover, float warmth = 0, float glow = 0) {
    if (!g_glass || !g_glass->DrawGlass(draw, a, b, radius, g_scale, Ink(tint), hover, warmth, glow))
        draw->AddRectFilled(a, b, Ink(tint), radius);
}

struct TooltipState {
    std::string hovered, displayed;
    float delay = 0, alpha = 0;
    ImVec2 anchor{};
} g_tooltip;

void Tooltip(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        g_tooltip.hovered = text;
}

void DrawTooltip() {
    static std::string previous;
    auto& tip = g_tooltip;
    const float dt = ImGui::GetIO().DeltaTime;
    if (tip.hovered != previous) tip.delay = 0;
    if (!tip.hovered.empty()) tip.delay += dt;
    previous = tip.hovered;
    const bool ready = tip.delay >= 0.33f && !tip.hovered.empty();
    if (ready && (tip.alpha <= 0 || tip.displayed == tip.hovered)) {
        tip.displayed = tip.hovered;
        tip.anchor = ImGui::GetMousePos();
    }
    const bool active = ready && tip.displayed == tip.hovered;
    tip.alpha = std::clamp(tip.alpha + (active ? dt : -dt) / 0.15f, 0.f, 1.f);
    tip.hovered.clear();
    if (tip.alpha <= 0) return;

    const auto* vp = ImGui::GetMainViewport();
    const float pad = S(13), gap = S(14), margin = S(8);
    const float maxWrap = std::max(1.f, std::min(S(370), vp->WorkSize.x - 2 * (pad + margin)));
    const float wrap = std::min(maxWrap, ImGui::CalcTextSize(tip.displayed.c_str(), nullptr, false, maxWrap).x + 1);
    const float width = wrap + 2 * pad;
    const float top = vp->WorkPos.y + margin;
    const float bottom = vp->WorkPos.y + vp->WorkSize.y - margin;
    const float right = vp->WorkPos.x + vp->WorkSize.x - margin;
    const float below = bottom - tip.anchor.y - gap, above = tip.anchor.y - gap - top;
    const float desired = ImGui::CalcTextSize(tip.displayed.c_str(), nullptr, false, wrap).y + 2 * pad;
    const bool under = desired <= below || below >= above;
    const float height = std::min(desired, under ? below : above);
    if (height <= 2 * pad) return;
    const ImVec2 pos(std::clamp(tip.anchor.x + S(4), vp->WorkPos.x + margin, right - width),
                     under ? tip.anchor.y + gap : tip.anchor.y - gap - height);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize({width, height});
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, EaseOut(tip.alpha));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {pad, pad});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(11));
    const auto flags = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##launcher_tooltip", nullptr, flags)) {
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(vp->WorkPos, {right + margin, bottom + margin}, false);
        Glass(draw, pos, {pos.x + width, pos.y + height}, S(11), C(32, 32, 35), 0.1f, 1);
        draw->PopClipRect();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextUnformatted(tip.displayed.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void Label(const char* text) {
    if (g_fontCaption) ImGui::PushFont(g_fontCaption);
    ImGui::TextColored(kMuted, "%s", text);
    if (g_fontCaption) ImGui::PopFont();
}

void Section(const char* text) {
    if (g_fontHeading) ImGui::PushFont(g_fontHeading);
    ImGui::TextUnformatted(text);
    if (g_fontHeading) ImGui::PopFont();
}

void BeginCard(const char* id, ImVec2 size, ImGuiWindowFlags flags = kNoScroll) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(16)});
    ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding, flags);
    ImGui::PopStyleVar();
}

void WrappedColored(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void StatusDot(const ImVec4& color, const char* text) {
    const auto p = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled({p.x + S(4), p.y + height / 2}, S(4), Ink(color));
    ImGui::Dummy({S(13), height});
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(text);
}

enum class Action { Neutral, Primary, Danger, Quiet };

bool Button(const char* label, ImVec2 size = {}, Action action = Action::Neutral) {
    if (g_fontButton) ImGui::PushFont(g_fontButton);
    const auto text = ImGui::CalcTextSize(label);
    const auto padding = ImGui::GetStyle().FramePadding;
    if (size.x == 0) size.x = text.x + 2 * padding.x;
    if (size.x < 0) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y == 0) size.y = text.y + 2 * padding.y;
    const auto id = ImGui::GetID(label);
    const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    const auto motion = InteractionFor(id);
    const auto p = ImGui::GetItemRectMin();
    const auto q = ImGui::GetItemRectMax();
    const ImVec2 inset(size.x * motion.press * 0.012f, size.y * motion.press * 0.012f);
    const ImVec2 a(p.x + inset.x, p.y + inset.y), b(q.x - inset.x, q.y - inset.y);
    const bool primary = action == Action::Primary;
    const auto base = primary ? kAccent : action == Action::Danger ? C(59, 38, 37) : action == Action::Quiet ? kTitleBg : kButton;
    auto fill = Mix(Mix(base, primary ? kAccentL : kHover, motion.hover), primary ? kAccentPressed : kBackground, motion.press * 0.68f);
    fill = Mix(fill, kCard, motion.disabled * 0.65f);
    auto* draw = ImGui::GetWindowDrawList();
    if (primary) Glass(draw, a, b, S(12), fill, motion.hover, true);
    else draw->AddRectFilled(a, b, Ink(fill), S(12));
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) draw->AddRect(a, b, Ink(kAccentL), S(12), 0, S(1.5f));
    const auto ink = primary ? C(33, 29, 12) : action == Action::Danger ? C(255, 158, 152) : kText;
    draw->AddText({p.x + (size.x - text.x) / 2, p.y + (size.y - text.y) / 2}, Ink(Mix(ink, kMuted, motion.disabled)), label);
    if (g_fontButton) ImGui::PopFont();
    return pressed;
}

bool Toggle(const char* label, bool value) {
    const auto text = ImGui::CalcTextSize(label);
    const float width = S(43), height = S(24);
    const auto id = ImGui::GetID(label);
    const bool pressed = ImGui::InvisibleButton(label, {width + S(9) + text.x, S(30)}, ImGuiButtonFlags_EnableNav);
    const auto min = ImGui::GetItemRectMin();
    const ImVec2 p(min.x, min.y + S(3)), q(p.x + width, p.y + height);
    const auto motion = InteractionFor(id);
    const float state = Animate(id, (value != pressed) ? 1.0f : 0.0f, value ? 1.0f : 0.0f, 0.22f);
    auto* draw = ImGui::GetWindowDrawList();
    const auto fill = Mix(Mix(C(70, 70, 70), kAccent, state), kWhite, motion.hover * 0.10f);
    Glass(draw, p, q, height / 2, fill, motion.hover, state > 0.5f);
    const float x = p.x + height / 2 + (width - height) * state;
    draw->AddCircleFilled({x, p.y + height / 2 + S(1)}, S(8), Ink(C(0, 0, 0, 0.22f)));
    draw->AddCircleFilled({x, p.y + height / 2}, S(8 - motion.press), Ink(kText));
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) draw->AddRect(p, q, Ink(kAccentL), height / 2);
    draw->AddText({q.x + S(9), min.y + (S(30) - text.y) / 2}, Ink(kText), label);
    return pressed;
}

int Segments(const char* id, const char* const* labels, int count, int selected, ImVec2 size, bool navigation = false) {
    ImGui::PushID(id);
    if (g_fontButton) ImGui::PushFont(g_fontButton);
    const auto p = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    Glass(draw, p, {p.x + size.x, p.y + size.y}, size.y / 2, kField, 0);
    float total = 0;
    float widths[5]{};
    for (int i = 0; i < count; ++i) {
        widths[i] = navigation ? ImGui::CalcTextSize(labels[i]).x + S(43) : 1.0f;
        total += widths[i];
    }
    const float unit = size.x / total;
    float starts[5]{};
    for (int i = 1; i < count; ++i) starts[i] = starts[i - 1] + widths[i - 1] * unit;

    // The lens lives in one position between tabs. Both offsets are in logical pixels,
    // so an in-flight animation remains valid if the display DPI changes.
    const int index = std::max(0, selected);
    const float targetX = starts[index] / g_scale;
    const float targetW = widths[index] * unit / g_scale;
    const float x = S(Animate(ImGui::GetID("x"), targetX, targetX, 0.24f));
    const float width = S(Animate(ImGui::GetID("width"), targetW, targetW, 0.24f));
    const float visibility = Animate(ImGui::GetID("visible"), selected >= 0 ? 1.f : 0.f, selected >= 0 ? 1.f : 0.f);
    if (visibility > 0.01f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * visibility);
        Glass(draw, {p.x + x + S(3), p.y + S(3)}, {p.x + x + width - S(3), p.y + size.y - S(3)},
              size.y / 2 - S(3), kAmberMuted, 0.28f, 0.40f, navigation ? 0.065f : 0.035f);
        ImGui::PopStyleVar();
    }

    int pressed = -1;
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine(0, 0);
        const auto tab = ImGui::GetID(labels[i]);
        const float w = widths[i] * unit;
        if (ImGui::InvisibleButton(labels[i], {w, size.y}, ImGuiButtonFlags_EnableNav)) pressed = i;
        const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const auto motion = InteractionFor(tab);
        const float selectedText = Animate(tab, i == selected ? 1.f : 0.f, i == selected ? 1.f : 0.f, 0.20f);
        if (motion.hover > 0.01f)
            draw->AddRectFilled({a.x + S(4), a.y + S(4)}, {b.x - S(4), b.y - S(4)},
                                Ink(C(255, 255, 255, motion.hover * (0.05f + motion.press * 0.04f))), size.y / 2 - S(4));
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
            draw->AddRect(a, b, Ink(kAccentL), size.y / 2);
        const auto text = ImGui::CalcTextSize(labels[i]);
        draw->AddText({a.x + (w - text.x) / 2, a.y + (size.y - text.y) / 2 - (navigation ? S(1) : 0)},
                       Ink(Mix(kMuted, kAccentL, selectedText)), labels[i]);
    }
    if (g_fontButton) ImGui::PopFont();
    ImGui::PopID();
    return pressed;
}

bool ComboStr(const char* id, std::string& value, const std::vector<std::string>& items,
              bool allowEmpty = false, const char* emptyLabel = "(нет)") {
    bool changed = false;
    const char* preview = value.empty() ? emptyLabel : value.c_str();
    const auto key = ImGui::GetID(id);
    const auto point = ImGui::GetCursorScreenPos();
    const bool over = ImGui::IsMouseHoveringRect(point, {point.x + ImGui::CalcItemWidth(), point.y + ImGui::GetFrameHeight()});
    const float hover = Animate(key ^ 0xff246U, over ? 1.0f : 0.0f, 0, 0.13f);
    const bool opened = ImGui::IsPopupOpen(ImHashStr("##ComboPopup", 0, key), ImGuiPopupFlags_None);
    const float lens = Animate(key ^ 0xf136U, opened ? 1.f : 0.f, 0, 0.16f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Mix(kButton, kHover, hover));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Mix(kButton, kHover, hover));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Mix(kField, kAmberMuted, lens * 0.44f));
    if (ImGui::BeginCombo(id, preview)) {
        if (allowEmpty) {
            if (ImGui::Selectable(emptyLabel, value.empty())) { value.clear(); changed = true; }
        }
        for (const auto& it : items) {
            bool sel = (it == value);
            if (ImGui::Selectable(it.c_str(), sel)) { value = it; changed = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(3);
    return changed;
}

std::string TrimStr(const std::string& s) { return util::TrimCopy(s); }

// ---------- Прогресс/статус загрузчика ----------
void DrawUpdaterStatus(AppState& s) {
    using St = GithubUpdater::State;
    St st = s.updater.GetState();
    std::string status = s.updater.GetStatus();
    if (st == St::Working) {
        float p = s.updater.GetProgress();
        if (p >= 0.0f) ImGui::ProgressBar(p, ImVec2(-FLT_MIN, S(20)));
        else ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-FLT_MIN, S(20)), "...");
        WrappedColored(kText, status);
    } else if (st == St::Failed) {
        WrappedColored(kRed, status);
    } else if (st == St::Success) {
        WrappedColored(kGreen, status);
    }
}

bool FilesBusy(const AppState& s) {
    return s.Deleting() || s.updater.Busy() || s.svcMgr.Busy() || s.term.IsRunning();
}

bool DrawUpdateBanner(AppState& s) {
    if (s.updater.UpdateStatus() != 1 || s.updater.Busy()) return false;
    auto tag = s.updater.LatestTag();
    if (tag.empty()) return false;
    if (tag.front() != 'v' && tag.front() != 'V') tag.insert(0, "v");
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kAccentL, "Доступна новая версия zapret (%s)!", tag.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(FilesBusy(s));
    if (Button("Обновить сейчас", {}, Action::Primary)) s.updater.Start(GithubUpdater::Task::Install);
    ImGui::EndDisabled();
    return true;
}

// =====================================================================
//  Кастомная шапка окна
// =====================================================================
void DrawTitleBar(HWND hwnd, const ImVec2& pos, const ImVec2& size) {
    auto* draw = ImGui::GetWindowDrawList();
    const float height = S(kTitleBarH);
    draw->AddRectFilled(pos, {pos.x + size.x, pos.y + S(kTitleBarH + kNavH)}, Ink(WithAlpha(kTitleBg, 0.77f)));
    const char* title = "ZAPRET LAUNCHER";
    if (g_fontTitle) ImGui::PushFont(g_fontTitle);
    const auto text = ImGui::CalcTextSize(title);
    const float x = pos.x + (size.x - text.x) / 2;
    draw->AddText({x, pos.y + (height - text.y) / 2}, Ink(kText), title);
    if (g_fontTitle) ImGui::PopFont();

    auto caption = [&](const char* id, float right, bool close) {
        const ImVec2 p(pos.x + size.x - S(right), pos.y + S(7));
        ImGui::SetCursorScreenPos(p);
        const bool clicked = ImGui::InvisibleButton(id, {S(39), S(37)});
        const auto motion = InteractionFor(ImGui::GetID(id));
        draw->AddRectFilled(p, {p.x + S(39), p.y + S(37)},
                            Ink(C(255, 255, 255, motion.hover * (0.10f - motion.press * 0.04f))), S(12));
        const ImVec2 center(p.x + S(19.5f), p.y + S(18.5f + motion.press));
        const float d = S(5 - motion.press * 0.4f);
        draw->AddLine({center.x - d, center.y - (close ? d : 0)},
                      {center.x + d, center.y + (close ? d : 0)}, Ink(kText), S(1.8f));
        if (close) draw->AddLine({center.x - d, center.y + d}, {center.x + d, center.y - d}, Ink(kText), S(1.8f));
        Tooltip(close ? "Свернуть в трей" : "Свернуть в панель задач");
        return clicked;
    };
    if (caption("##caption_min", 93, false)) PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    if (caption("##caption_close", 49, true)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

bool ServiceBusy(const AppState& s) {
    return s.svcMgr.Busy() || s.term.IsRunning() || s.updater.Installing();
}

void DrawDashboard(AppState& s) {
    const auto& status = s.svcStatus;
    const bool localService = status.installed && status.usesDirectory;
    const bool running = status.state == SERVICE_RUNNING;
    const bool pending = status.state == SERVICE_START_PENDING || status.state == SERVICE_STOP_PENDING ||
                         status.queryError == ERROR_SERVICE_MARKED_FOR_DELETE;
    const bool busy = ServiceBusy(s);
    const bool canInstall = status.serviceBatPresent && !busy && !pending && !s.config.settings.selectedBat.empty();

    const bool working = pending || s.svcMgr.Busy() || s.updater.Installing();
    const float left = S(371);
    BeginCard("##status_card", {left, S(385)});
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    Label("СЛУЖБА ZAPRET");
    const float size = S(146);
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({origin.x + (contentWidth - size) / 2, origin.y + S(33)});

    const bool installFiles = !localService && !s.missing.empty();
    ImGui::BeginDisabled(installFiles ? FilesBusy(s) : busy || pending || (!localService && !canInstall));
    const auto powerId = ImGui::GetID("##power");
    const bool pressed = ImGui::InvisibleButton("##power", {size, size}, ImGuiButtonFlags_EnableNav);
    const auto p = ImGui::GetItemRectMin();
    const auto motion = InteractionFor(powerId);
    const float charge = Animate(powerId, running ? 1.f : 0.f, running ? 1.f : 0.f, 0.23f);
    const float pulse = working ? 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f) : 0;
    const ImVec2 center(p.x + size / 2, p.y + size / 2);
    const float radius = size * (0.5f - motion.press * 0.012f);
    auto* draw = ImGui::GetWindowDrawList();
    const float glow = charge * 0.38f + pulse * 0.14f;
    const auto surface = Mix(Mix(kButton, kAccent, charge), Mix(kHover, kAccentL, charge), motion.hover);
    const ImVec2 a(center.x - radius, center.y - radius), b(center.x + radius, center.y + radius);
    Glass(draw, a, b, radius, surface, motion.hover, charge, glow);
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
        draw->AddCircle(center, radius + S(3), Ink(kAccentL), 80, S(2));
    const auto glyph = Ink(Mix(kText, C(45, 39, 10), charge));
    if (working) {
        constexpr float pi = 3.14159265f;
        const float phase = static_cast<float>(ImGui::GetTime()) * 4.4f;
        draw->PathArcTo(center, S(17), phase, phase + 1.45f * pi, 30);
        draw->PathStroke(glyph, 0, S(3));
    } else {
        draw->PathArcTo({center.x, center.y + S(2)}, S(16), -0.78f, 3.92f, 32);
        draw->PathStroke(glyph, 0, S(3.3f));
        draw->AddLine({center.x, center.y - S(20)}, {center.x, center.y + S(1)}, glyph, S(3.3f));
    }
    Tooltip(localService && running ? "Остановить службу" : installFiles ? "Скачать Flowseal" :
            status.installed && !localService ? "Применить стратегию из выбранной папки" : "Запустить службу");
    ImGui::EndDisabled();

    if (pressed) {
        if (installFiles) s.updater.Start(GithubUpdater::Task::Install);
        else if (localService) s.svcMgr.SetRunning(!running);
        else s.svcMgr.InstallStrategy(s.config.settings.selectedBat);
    }

    ImGui::Spacing();
    const char* title = status.queryError == ERROR_SERVICE_MARKED_FOR_DELETE ? "Ожидает удаления" :
        working ? "Выполняется операция" : running && localService ? "Служба zapret работает" :
        running && !status.executable.empty() ? "Zapret из другой папки" : running ? "Служба работает" :
        status.queryError && status.queryError != ERROR_SERVICE_DOES_NOT_EXIST ? "Статус службы неизвестен" :
        status.installed ? "Служба остановлена" : installFiles ? "Zapret не установлен" : "Служба не установлена";
    if (g_fontTitle) ImGui::PushFont(g_fontTitle);
    ImGui::SetCursorPosX(std::max(S(16), (left - ImGui::CalcTextSize(title).x) / 2));
    ImGui::TextWrapped("%s", title);
    if (g_fontTitle) ImGui::PopFont();
    const std::string detail = s.updater.Installing() ? s.updater.GetStatus() :
        s.svcMgr.Busy() ? s.svcMgr.GetStatus() : installFiles && !status.installed ? "Скачать Flowseal"
        : std::string("Служба ") + status.stateText + "  ·  winws.exe " + (status.winwsRunning ? "запущен" : "запуск не подтверждён");
    if (g_fontCaption) ImGui::PushFont(g_fontCaption);
    const float detailWidth = ImGui::CalcTextSize(detail.c_str()).x;
    ImGui::SetCursorPosX(std::max(S(16), (left - detailWidth - S(14)) / 2));
    ImGui::PushTextWrapPos(left - S(16));
    StatusDot(working ? kWarn : Mix(kRed, kGreen, charge), detail.c_str());
    ImGui::PopTextWrapPos();
    if (g_fontCaption) ImGui::PopFont();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(16)});
    ImGui::BeginChild("##strategy_card", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding |
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize, kNoScroll);
    ImGui::PopStyleVar();
    Section("Стратегия");
    WrappedColored(kMuted, "Активная стратегия  ·  " + (status.strategy.empty() ? std::string("не определена") : status.strategy));
    ImGui::BeginDisabled(busy || pending || !status.serviceBatPresent);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ComboStr("##bat", s.config.settings.selectedBat, s.strategies, false, "Нет файлов general*.bat"))
        s.SaveConfig();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!canInstall);
    if (Button(status.installed ? "Сменить стратегию" : "Установить службу", {S(182), S(36)}))
        s.svcMgr.InstallStrategy(s.config.settings.selectedBat);
    ImGui::EndDisabled();
    ImGui::SameLine();
    static fs::path confirmedPath;
    static bool differentDirectory = false;
    ImGui::BeginDisabled(busy || pending || !status.supported);
    if (Button("Удалить сервис", {S(130), S(36)}, Action::Danger)) {
        confirmedPath = status.executable;
        differentDirectory = !status.usesDirectory;
        ImGui::OpenPopup("Удаление службы zapret");
    }
    Tooltip(status.supported ? "Остановить и удалить регистрацию службы Windows" :
            "Принадлежность службы сборке Flowseal не подтверждена");
    ImGui::EndDisabled();
    ImGui::SetNextWindowSize({S(520), 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Удаление службы zapret", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Служба будет остановлена, а её регистрация удалена. Файлы установки останутся на диске.");
        WrappedColored(kText, util::ToUtf8(confirmedPath.wstring()));
        if (differentDirectory) WrappedColored(kWarn, "Служба использует другую рабочую папку.");
        if (Button("Отмена")) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || pending);
        if (Button("Удалить сервис", {}, Action::Danger)) {
            s.svcMgr.RemoveService(confirmedPath);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (status.installed && !status.usesDirectory)
        WrappedColored(kWarn, status.executable.empty() ? "Путь службы не удалось прочитать." :
            "Служба использует другую папку. Смените стратегию, чтобы применить настройки из выбранной папки.");
    else if (s.term.IsRunning())
        Label("Для управления службой остановите сеанс терминала.");
    ImGui::EndChild();

    BeginCard("##filters_card", {0, S(204)});
    Section("Сетевые фильтры");
    const bool canFilter = !busy && !pending && status.serviceBatPresent && (!status.installed || status.usesDirectory);
    const bool gameOn = status.gameMode != "disabled";
    ImGui::BeginDisabled(!canFilter);
    if (Toggle("Game Filter", gameOn)) s.svcMgr.SetGameFilter(gameOn ? "disabled" : s.gameModePref);
    ImGui::SameLine();
    static const std::vector<std::string> modes{"TCP и UDP", "TCP", "UDP"};
    const auto mode = gameOn ? status.gameMode : s.gameModePref;
    std::string choice = mode == "tcp" ? modes[1] : mode == "udp" ? modes[2] : modes[0];
    ImGui::SetNextItemWidth(S(128));
    if (ComboStr("##game_mode", choice, modes)) {
        s.gameModePref = choice == modes[1] ? "tcp" : choice == modes[2] ? "udp" : "all";
        if (gameOn && mode != s.gameModePref) s.svcMgr.SetGameFilter(s.gameModePref);
    }
    ImGui::AlignTextToFramePadding();
    Label("IPSet Filter");
    ImGui::SameLine(0, S(20));
    static const char* labels[] = {"none", "loaded", "any"};
    int selected = -1;
    for (int i = 0; i < 3; ++i) if (status.ipsetMode == labels[i]) selected = i;
    const int clicked = Segments("##ipset_mode", labels, 3, selected, {S(244), S(32)});
    if (clicked >= 0 && clicked != selected) s.svcMgr.SetIpsetMode(labels[clicked]);
    if (Button("Обновить IPSet List", {S(165), S(34)})) s.svcMgr.UpdateIpset();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !status.serviceBatPresent);
    if (Button("Обновить Hosts File", {S(165), S(34)})) s.svcMgr.UpdateHosts();
    ImGui::EndDisabled();
    ImGui::TextColored(kMuted, "Порты Game Filter  ·  TCP %s  /  UDP %s",
                       status.gameTcp.c_str(), status.gameUdp.c_str());
    ImGui::EndChild();

    ImGui::EndGroup();
    using State = ServiceManager::State;
    const auto state = s.svcMgr.GetState();
    if (state != State::Idle) {
        const auto message = s.svcMgr.GetStatus();
        StatusDot(state == State::Failed ? kRed : state == State::Working ? kAccent : kGreen, message.c_str());
        Tooltip(message.c_str());
    } else if (s.updater.Busy() || s.updater.GetState() == GithubUpdater::State::Failed) {
        DrawUpdaterStatus(s);
    }
}

// =====================================================================
//  Вкладка "Терминал"
// =====================================================================
void RebuildTermLines(AppState& s) {
    std::string text = s.term.DisplayText();
    const auto menu = text.rfind("ZAPRET SERVICE MANAGER v");
    if (menu != std::string::npos && text.find("Select option (0-", menu) != std::string::npos &&
        text.find('\n', text.find("Select option (0-", menu)) == std::string::npos) {
        auto line = text.rfind('\n', menu);
        text.erase(0, line == std::string::npos ? 0 : line + 1);
    }
    s.termLines.clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) { s.termLines.push_back(text.substr(start)); break; }
        s.termLines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
}

bool ContainsAny(const std::string& hay, std::initializer_list<const char*> needles) {
    std::string low = util::ToLowerAscii(hay);
    for (const char* n : needles) if (low.find(n) != std::string::npos) return true;
    return false;
}

void DrawTerminal(AppState& s) {
    bool ready = s.svcStatus.serviceBatPresent;
    bool running = s.term.IsRunning();

    // Автозапуск при открытии вкладки
    if (ready && !running && !s.termStarted && !s.svcMgr.Busy() && !s.updater.Installing()) {
        s.StartTerminal();
        running = s.term.IsRunning();
    }

    // ---- Верхняя строка ----
    ImGui::BeginDisabled(!ready || s.svcMgr.Busy() || s.updater.Installing());
    if (!running) {
        if (Button("Запустить service.bat", ImVec2(S(176), S(32)))) s.StartTerminal();
    } else {
        if (Button("Остановить", {S(176), S(32)}, Action::Danger)) s.term.Stop();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (Button("Очистить", ImVec2(S(110), S(32)))) s.term.ClearDisplay();
    ImGui::SameLine();
    if (Button("Копировать всё", ImVec2(S(150), S(32)))) ImGui::SetClipboardText(s.term.DisplayText().c_str());
    ImGui::SameLine();
    if (Toggle("Автопрокрутка", s.termAutoScroll)) s.termAutoScroll = !s.termAutoScroll;
    ImGui::SameLine();
    if (Toggle("Завершать при уходе", s.termKillOnLeave)) {
        s.termKillOnLeave = !s.termKillOnLeave;
        s.config.settings.terminalKillOnLeave = s.termKillOnLeave;
        s.SaveConfig();
    }

    if (!ready) {
        ImGui::TextColored(kWarn, "service.bat не найден. Скачайте сборку Flowseal на вкладке «Главная».");
    } else if (!running && s.termStarted) {
        ImGui::TextColored(kMuted, "Процесс завершён (код %lu). Нажмите «Запустить service.bat», чтобы открыть меню снова.",
                           (unsigned long)s.term.ExitCode());
    }

    // ---- Окно вывода ----
    size_t counter = s.term.DisplayCounter();
    bool changed = (counter != s.termCounter);
    if (changed) { RebuildTermLines(s); s.termCounter = counter; }

    const float bottomH = S(88);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kField);
    BeginCard("##term", {0, -bottomH}, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (g_fontMono) ImGui::PushFont(g_fontMono);
    {
        float lh = ImGui::GetTextLineHeightWithSpacing();
        ImGuiListClipper clip;
        clip.Begin((int)s.termLines.size(), lh);
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const std::string& line = s.termLines[i];
                ImVec4 col = kText;
                if (ContainsAny(line, {"error", "failed", "not found", "[x]", "invalid"})) col = C(240, 110, 110);
                else if (ContainsAny(line, {"success", "is running", "passed", "installed"})) col = C(90, 210, 130);
                else if (ContainsAny(line, {"zapret service manager", "::"})) col = kAccentL;
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopStyleColor();
            }
        }
        clip.End();
    }
    if (g_fontMono) ImGui::PopFont();
    ImGui::PopStyleVar();
    if (s.termAutoScroll && changed) ImGui::SetScrollY(ImGui::GetCursorPosY());
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- Быстрые кнопки ----
    auto send = [&](const std::string& t) { if (s.term.IsRunning()) s.term.SendLine(t); };
    ImGui::BeginDisabled(!running);
    const char* digits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "0"};
    for (int i = 0; i < 13; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        if (Button(digits[i], ImVec2(S(44), S(30)))) send(digits[i]);
        ImGui::PopID();
    }
    ImGui::SameLine();
    if (Button("Enter", ImVec2(S(80), S(30)))) send("");

    // ---- Поле ввода ----
    ImGui::SetNextItemWidth(-S(132));
    bool enter = ImGui::InputTextWithHint("##termin", "Введите текст и нажмите Enter", &s.termInput,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool clicked = Button("Отправить", ImVec2(S(120), S(30)));
    if (enter || clicked) {
        send(s.termInput);
        s.termInput.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::EndDisabled();
}

// =====================================================================
//  Вкладка «Диагностика»
// =====================================================================
void DrawDiagnostics(AppState& s) {
    const bool busy = s.diagnosticJob.valid();
    ImGui::BeginDisabled(busy);
    if (Button("Проверить сеть и систему", {S(218), S(36)}, Action::Primary))
        s.diagnosticJob = std::async(std::launch::async, diagnostics::Run);
    ImGui::EndDisabled();
    if (busy) {
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "Выполняется проверка...");
    }

    auto draw = [](const std::vector<diagnostics::Result>& results) {
        using diagnostics::Level;
        for (const auto& result : results) {
            const auto color = result.level == Level::Good ? kGreen : result.level == Level::Warning ? kWarn : kRed;
            StatusDot(color, result.name.c_str());
            WrappedColored(kMuted, result.detail);
        }
    };
    const float column = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    BeginCard("##network_card", {column, 0}, ImGuiWindowFlags_None);
    Section("Доступность сервисов");
    draw(s.diagnosticReport.network);
    ImGui::EndChild();
    ImGui::SameLine();
    BeginCard("##system_card", {0, 0}, ImGuiWindowFlags_None);
    Section("Службы, драйверы и DNS");
    draw(s.diagnosticReport.system);
    ImGui::EndChild();
}

// =====================================================================
//  Вкладка "Списки доменов"
// =====================================================================
void LoadList(AppState& s, const std::string& name) {
    std::string text, err;
    if (s.config.ReadTextFile(s.config.ListsDir() / fs::path(util::FromUtf8(name)), text, err)) {
        s.selectedList = name;
        s.listBuffer = text;
        s.listDirty = false;
    } else {
        s.SetNotice(err, true);
    }
}

void SaveList(AppState& s) {
    std::string err;
    if (s.config.WriteTextFile(s.config.ListsDir() / fs::path(util::FromUtf8(s.selectedList)), s.listBuffer, err)) {
        s.listDirty = false;
        s.SetNotice("Файл \"" + s.selectedList + "\" сохранён.", false);
    } else {
        s.SetNotice(err, true);
    }
}

void DrawLists(AppState& s) {
    BeginCard("##files", {S(260), 0}, ImGuiWindowFlags_None);
    Section("Файлы списков");
    std::string toLoad;
    for (const auto& f : s.listFiles) {
        if (ImGui::Selectable(f.c_str(), f == s.selectedList)) toLoad = f;
    }
    if (!toLoad.empty() && toLoad != s.selectedList) LoadList(s, toLoad);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    Label("Новый файл");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##newlist", "my-list.txt", &s.newListName);
    if (Button("Создать", ImVec2(-FLT_MIN, S(32)))) {
        std::string n = TrimStr(s.newListName);
        if (n.empty() || n.find_first_of("\\/:*?\"<>|") != std::string::npos) {
            s.SetNotice("Некорректное имя файла.", true);
        } else {
            if (n.size() < 4 || n.compare(n.size() - 4, 4, ".txt") != 0) n += ".txt";
            fs::path p = s.config.ListsDir() / fs::path(util::FromUtf8(n));
            std::error_code ec;
            if (fs::exists(p, ec)) {
                s.SetNotice("Файл с таким именем уже существует.", true);
            } else {
                std::string err;
                if (s.config.WriteTextFile(p, "", err)) {
                    s.newListName.clear();
                    s.RefreshFiles();
                    LoadList(s, n);
                } else {
                    s.SetNotice(err, true);
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    BeginCard("##editor", {0, 0});
    if (s.selectedList.empty()) {
        ImGui::TextColored(kMuted, "Выберите файл слева, чтобы изменить его.");
    } else {
        ImGui::TextColored(kText, "%s%s", s.selectedList.c_str(), s.listDirty ? "  *(не сохранено)" : "");
        ImGui::TextWrapped("Один домен на строку. Файлы *-user.txt сохраняются при обновлении.");
        if (g_fontMono) ImGui::PushFont(g_fontMono);
        if (ImGui::InputTextMultiline("##text", &s.listBuffer, ImVec2(-FLT_MIN, -S(46)), ImGuiInputTextFlags_AllowTabInput))
            s.listDirty = true;
        if (g_fontMono) ImGui::PopFont();
        if (Button("Сохранить", ImVec2(S(160), S(36)))) SaveList(s);

    }
    ImGui::EndChild();
}

// =====================================================================
//  Вкладка "Настройки"
// =====================================================================
enum class SettingsIcon { Folder, Download, System, Repository };
constexpr wchar_t kLauncherRepository[] = L"https://github.com/jaketeaa/zapretlauncher";

void SettingsHeading(SettingsIcon icon, const char* title) {
    const auto p = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const auto ink = Ink(kMuted);
    const ImVec2 c(p.x + S(13), p.y + S(13));
    draw->AddRectFilled(p, {p.x + S(26), p.y + S(26)}, Ink(kButton), S(8));
    if (icon == SettingsIcon::Folder) {
        draw->AddRect({c.x - S(6), c.y - S(3)}, {c.x + S(6), c.y + S(5)}, ink, S(1.5f));
        draw->AddLine({c.x - S(5), c.y - S(5)}, {c.x, c.y - S(5)}, ink, S(1.5f));
    } else if (icon == SettingsIcon::Download) {
        draw->AddLine({c.x, c.y - S(6)}, {c.x, c.y + S(2)}, ink, S(1.5f));
        draw->AddLine({c.x - S(3), c.y}, {c.x, c.y + S(3)}, ink, S(1.5f));
        draw->AddLine({c.x, c.y + S(3)}, {c.x + S(3), c.y}, ink, S(1.5f));
        draw->AddLine({c.x - S(5), c.y + S(6)}, {c.x + S(5), c.y + S(6)}, ink, S(1.5f));
    } else if (icon == SettingsIcon::Repository) {
        // Cat silhouette on a GitHub-style round tile.
        const ImVec2 points[] = {{c.x - S(6), c.y + S(1)}, {c.x - S(6), c.y - S(6)},
            {c.x - S(2), c.y - S(4)}, {c.x + S(2), c.y - S(4)}, {c.x + S(6), c.y - S(6)},
            {c.x + S(6), c.y + S(1)}, {c.x + S(3), c.y + S(5)}, {c.x - S(3), c.y + S(5)}};
        draw->AddPolyline(points, IM_ARRAYSIZE(points), ink, ImDrawFlags_Closed, S(1.3f));
    } else {
        draw->AddRect({c.x - S(6), c.y - S(5)}, {c.x + S(6), c.y + S(3)}, ink, S(1.5f));
        draw->AddLine({c.x - S(3), c.y + S(6)}, {c.x + S(3), c.y + S(6)}, ink, S(1.5f));
    }
    ImGui::Dummy({S(26), S(26)});
    ImGui::SameLine(0, S(9));
    Section(title);
}

void DrawSettings(AppState& s, HWND hwnd) {
    BeginCard("##storage_card", {0, S(130)});
    SettingsHeading(SettingsIcon::Folder, "Расположение файлов zapret");
    Label("Папка сборки и списков доменов");
    std::string directory = util::ToUtf8(s.config.ZapretDir().wstring());
    ImGui::SetNextItemWidth(S(538));
    ImGui::InputText("##directory", &directory, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    ImGui::BeginDisabled(FilesBusy(s));
    if (Button("Изменить папку...")) s.ChooseDirectory(hwnd);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(s.Deleting());
    if (Button("Открыть в Проводнике")) {
        std::error_code error;
        const auto path = s.config.ZapretDir();
        fs::create_directories(path, error);
        if (error || reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            s.SetNotice("Не удалось открыть папку zapret.", true);
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(16)});
    ImGui::BeginChild("##release_card", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding |
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize, kNoScroll);
    ImGui::PopStyleVar();
    SettingsHeading(SettingsIcon::Download, "Сборка Flowseal");
    const auto& local = s.localVersion;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMuted, "Установленная версия  ·  %s", local.empty() ? "неизвестно" : local.c_str());
    ImGui::SameLine(S(370));
    ImGui::BeginDisabled(FilesBusy(s));
    if (Button("Проверить обновления")) s.updater.Start(GithubUpdater::Task::Check);
    ImGui::SameLine();
    if (Button(s.missing.empty() ? "Переустановить / обновить" : "Скачать компоненты", {}, Action::Primary))
        s.updater.Start(GithubUpdater::Task::Install);
    ImGui::EndDisabled();
    if (s.updater.GetState() != GithubUpdater::State::Idle) {
        const auto text = s.updater.GetStatus();
        WrappedColored(s.updater.GetState() == GithubUpdater::State::Failed ? kRed : kMuted, text);
        Tooltip(text.c_str());
        const float progress = s.updater.GetProgress();
        if (s.updater.Busy() && progress >= 0.0f)
            ImGui::ProgressBar(std::clamp(progress, 0.f, 1.f), {S(185), S(8)}, "");
    }
    ImGui::EndChild();

    const float column = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    BeginCard("##startup_card", {column, S(119)});
    SettingsHeading(SettingsIcon::System, "Запуск и выход");
    ImGui::BeginDisabled(s.autostartJob.valid());
    if (Toggle("Запускать вместе с Windows", s.runWithWindows)) {
        const bool enabled = !s.runWithWindows;
        try {
            s.autostartJob = std::async(std::launch::async, [enabled] {
                std::string error;
                try {
                    if (util::SetAutostart(enabled, error)) return std::string{};
                } catch (const std::exception& e) {
                    error = e.what();
                }
                return error.empty() ? std::string("Не удалось изменить параметры автозапуска.") : error;
            });
            s.runWithWindows = enabled;
        } catch (const std::system_error&) {
            s.SetNotice("Не удалось запустить изменение параметров автозапуска.", true);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (Button("Выход из программы", {S(171), S(34)}, Action::Danger))
        PostMessageW(hwnd, WM_APP_EXIT, 0, 0);
    ImGui::EndChild();

    ImGui::SameLine();
    BeginCard("##uninstall_card", {0, S(119)});
    Section("Управление установкой");
    ImGui::BeginDisabled(s.Deleting() || s.updater.Busy() || s.svcMgr.Busy() || !s.managedInstallation);
    if (Button("Удалить zapret", {S(194), S(32)}, Action::Danger)) ImGui::OpenPopup("Удаление zapret");
    ImGui::EndDisabled();
    if (s.Deleting()) {
        Label("Удаление zapret…");
        ImGui::SameLine();
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), {S(160), S(9)}, "");
    } else if (!s.managedInstallation) {
        Label(s.missing.empty() ? "У этой папки нет подтверждённого маркера установки." : "Zapret не установлен.");
    }

    ImGui::SetNextWindowSize({S(530), 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Удаление zapret", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Все службы и процессы zapret, относящиеся к данной установке, будут остановлены. Рабочая папка zapret и её содержимое будут удалены.");
        WrappedColored(kMuted, directory);
        if (Button("Отмена")) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (Button("Удалить zapret", {}, Action::Danger)) {
            s.RemoveInstallation();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    BeginCard("##about_card", {0, S(150)});
    SettingsHeading(SettingsIcon::Repository, "О проекте");
    ImGui::SameLine(ImGui::GetWindowWidth() - S(232));
    ImGui::BeginDisabled(!kLauncherRepository[0]);
    if (Button("Репозиторий проекта", {S(216), S(32)}))
        ShellExecuteW(hwnd, L"open", kLauncherRepository, nullptr, nullptr, SW_SHOWNORMAL);
    const auto buttonMin = ImGui::GetItemRectMin(), buttonMax = ImGui::GetItemRectMax();
    const ImVec2 arrow(buttonMax.x - S(15), (buttonMin.y + buttonMax.y) * 0.5f);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddLine({arrow.x - S(4), arrow.y + S(4)}, {arrow.x + S(3), arrow.y - S(3)}, Ink(kMuted), S(1.2f));
    draw->AddLine({arrow.x - S(2), arrow.y - S(3)}, {arrow.x + S(3), arrow.y - S(3)}, Ink(kMuted), S(1.2f));
    draw->AddLine({arrow.x + S(3), arrow.y - S(3)}, {arrow.x + S(3), arrow.y + S(2)}, Ink(kMuted), S(1.2f));
    Tooltip(kLauncherRepository[0] ? "Открыть репозиторий лаунчера в браузере" :
                                    "Публичная ссылка на репозиторий лаунчера не задана");
    ImGui::EndDisabled();
    ImGui::Image(g_launcherIcon, {S(56), S(56)});
    ImGui::SameLine(0, S(14));
    ImGui::BeginGroup();
    const auto spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {spacing.x, S(3)});
    Section("ZAPRET LAUNCHER");
    ImGui::TextColored(kAccentL, "%s", "Сделано JAKETEA");
    Label("Графический лаунчер для управления zapret.");
    Label("Использует zapret-discord-youtube от Flowseal.");
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    ImGui::EndChild();
}

// =====================================================================
//  Нижняя панель логов
// =====================================================================
void DrawLogPanel(AppState& s) {
    if (Button(s.logOpen ? "Скрыть логи" : "Показать логи", {S(120), S(32)}, Action::Quiet)) s.logOpen = !s.logOpen;
    ImGui::SameLine();
    if (Button("Очистить")) s.log.ClearLog();
    ImGui::SameLine();
    if (Button("Копировать логи")) {
        std::string text;
        for (const auto& line : s.log.GetLogSnapshot()) text += line + '\n';
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if (Toggle("Автопрокрутка", s.logAutoScroll)) s.logAutoScroll = !s.logAutoScroll;
    const float reveal = Animate(ImGui::GetID("##log_reveal"), s.logOpen ? 1.f : 0.f, 0, 0.16f);
    if (reveal <= 0.01f || ImGui::GetContentRegionAvail().y < S(26)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * reveal);
    size_t counter = s.log.LogCounter();
    bool changed = (counter != s.logCacheCounter);
    if (changed) {
        s.logCache = s.log.GetLogSnapshot();
        s.logCacheCounter = counter;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kField);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(12)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {S(12), S(5)});
    if (g_fontMono) ImGui::PushFont(g_fontMono);
    if (changed || s.logLayoutScale != g_scale) {
        s.logWidth = 0;
        for (const auto& line : s.logCache)
            s.logWidth = std::max(s.logWidth, ImGui::CalcTextSize(line.c_str()).x);
        s.logLayoutScale = g_scale;
    }
    ImGui::SetNextWindowContentSize({s.logWidth + S(16), 0});
    ImGui::BeginChild("##log", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    const auto panel = ImGui::GetWindowPos();
    const auto area = ImGui::GetWindowSize();
    ImGui::PushClipRect({panel.x + S(16), panel.y + S(12)},
                        {panel.x + area.x - S(16), panel.y + area.y - S(12)}, true);
    ImGuiListClipper clip;
    clip.Begin((int)s.logCache.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const std::string& line = s.logCache[i];
            bool own = line.rfind("[лаунчер]", 0) == 0;
            if (own) ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::TextUnformatted(line.c_str());
            if (own) ImGui::PopStyleColor();
        }
    }
    clip.End();
    ImGui::PopClipRect();
    if (g_fontMono) ImGui::PopFont();
    if (s.logAutoScroll && changed) ImGui::SetScrollY(ImGui::GetCursorPosY());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

} // namespace

// =====================================================================
//  Публичный интерфейс
// =====================================================================
void ui::SetDpiScale(float scale) { g_scale = scale; }
void ui::SetFonts(ImFont* title, ImFont* heading, ImFont* button, ImFont* caption, ImFont* mono) {
    g_fontTitle = title;
    g_fontHeading = heading;
    g_fontButton = button;
    g_fontCaption = caption;
    g_fontMono = mono;
}

void ui::SetGlassRenderer(const GlassRenderer* renderer) { g_glass = renderer; }
void ui::SetLauncherIcon(unsigned long long texture) { g_launcherIcon = texture; }


float ui::TitleBarHeightPx() { return S(kTitleBarH); }
float ui::CaptionButtonsWidthPx() { return S(kCaptionButtonsWidth); }

void ui::ApplyTheme(float scale) {
    g_scale = scale;
    auto& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.WindowPadding = {16, 16};
    style.FramePadding = {12, 8};
    style.ItemSpacing = {12, 10};
    style.ItemInnerSpacing = {8, 6};
    style.CellPadding = {6, 0};
    style.ScrollbarSize = 8;
    style.ChildRounding = 18;
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
    style.CurveTessellationTol = 0.12f;
    style.CircleTessellationMaxError = 0.15f;
    style.FrameRounding = 12;
    style.PopupRounding = 16;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 5;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.FrameBorderSize = 0;
    style.PopupBorderSize = 1;

    auto* color = style.Colors;
    color[ImGuiCol_Text] = kText;
    color[ImGuiCol_TextDisabled] = kInactive;
    color[ImGuiCol_WindowBg] = kBackground;
    color[ImGuiCol_ChildBg] = kCard;
    color[ImGuiCol_PopupBg] = kCard;
    color[ImGuiCol_Border] = kDivider;
    color[ImGuiCol_FrameBg] = kField;
    color[ImGuiCol_FrameBgHovered] = kButton;
    color[ImGuiCol_FrameBgActive] = kButton;
    color[ImGuiCol_Header] = kButton;
    color[ImGuiCol_HeaderHovered] = Mix(kButton, kWhite, 0.08f);
    color[ImGuiCol_Button] = kButton;
    color[ImGuiCol_ButtonHovered] = Mix(kButton, kWhite, 0.15f);
    color[ImGuiCol_ButtonActive] = kField;
    color[ImGuiCol_Separator] = kDivider;
    color[ImGuiCol_ScrollbarBg] = kField;
    color[ImGuiCol_ScrollbarGrab] = C(66, 66, 66);
    color[ImGuiCol_ScrollbarGrabHovered] = C(100, 100, 100);
    color[ImGuiCol_ScrollbarGrabActive] = kMuted;
    color[ImGuiCol_HeaderActive] = kAmberMuted;
    color[ImGuiCol_NavCursor] = kAccent;
    color[ImGuiCol_PlotHistogram] = color[ImGuiCol_SliderGrab] = kAccent;
    color[ImGuiCol_TextSelectedBg] = WithAlpha(kAccent, 0.27f);
    color[ImGuiCol_CheckMark] = kAccent;
    style.ScaleAllSizes(scale);
    style.ChildBorderSize = 1;
}

void ui::Render(AppState& s, HWND hwnd) {
    if (ImGui::GetFrameCount() % 120 == 0) {
        const int oldest = ImGui::GetFrameCount() - 180;
        for (auto it = g_motion.begin(); it != g_motion.end();) {
            if (it->second.frame < oldest) it = g_motion.erase(it);
            else ++it;
        }
    }

    auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    const auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoBringToFrontOnFocus | kNoScroll;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    const bool visible = ImGui::Begin("##root", nullptr, flags);
    ImGui::PopStyleVar();
    if (visible) {
        if (g_glass && g_glass->Backdrop())
            ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(g_glass->Backdrop()), viewport->Pos,
                {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y});
        DrawTitleBar(hwnd, viewport->Pos, viewport->Size);
        auto* draw = ImGui::GetWindowDrawList();
        const float navBottom = viewport->Pos.y + S(kTitleBarH + kNavH) - 1;
        draw->AddLine({viewport->Pos.x, navBottom}, {viewport->Pos.x + viewport->Size.x, navBottom}, Ink(kDivider));
        static const char* pages[] = {"Главная", "Терминал", "Диагностика", "Списки доменов", "Настройки"};
        static int page = 0;
        const float navWidth = S(698);
        ImGui::SetCursorPos({(viewport->Size.x - navWidth) / 2, S(kTitleBarH + 10)});
        const int next = Segments("##navigation", pages, 5, page, {navWidth, S(39)}, true);
        static float fadeTime = 0.15f;
        if (next >= 0 && next != page) {
            page = next;
            fadeTime = 0;
        } else fadeTime += ImGui::GetIO().DeltaTime;

        const bool terminal = page == 1;
        if (!terminal && s.termTabWasActive && !s.Deleting()) {
            if (s.termKillOnLeave && s.term.IsRunning()) s.term.Stop();
            s.termStarted = false;
        }
        s.termTabWasActive = terminal;

        const float top = S(kTitleBarH + kNavH + 18);
        const float sides = S(24);
        ImGui::SetCursorPos({sides, top});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, C(0, 0, 0, 0));
        ImGui::BeginChild("##body", {viewport->Size.x - sides * 2, viewport->Size.y - top - S(14)}, 0, kNoScroll);
        ImGui::PopStyleColor();

        const bool updateBanner = DrawUpdateBanner(s);
        if (!s.notice.empty()) {
            if (updateBanner) ImGui::SameLine(0, S(24));
            if (Button("Скрыть", {S(68), S(23)}, Action::Quiet)) s.notice.clear();
            ImGui::SameLine();
            const auto line = s.notice.substr(0, s.notice.find('\n'));
            const auto p = ImGui::GetCursorScreenPos();
            const ImVec2 area(ImGui::GetContentRegionAvail().x, S(23));
            auto* body = ImGui::GetWindowDrawList();
            body->PushClipRect(p, {p.x + area.x, p.y + area.y}, true);
            body->AddText({p.x, p.y + (area.y - ImGui::GetFontSize()) / 2}, Ink(s.noticeError ? kRed : kAccentL), line.c_str());
            body->PopClipRect();
            ImGui::Dummy(area);
            Tooltip(s.notice.c_str());
        }

        const float logSize = Animate(ImGui::GetID("##log_height"), s.logOpen ? 123.f : 45.f, s.logOpen ? 123.f : 45.f, 0.18f);
        const float footer = terminal ? 0 : -S(logSize);
        ImGui::PushID(page);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, C(0, 0, 0, 0));
        ImGui::BeginChild("##page", {0, footer}, 0,
                          page == 0 || page == 4 ? ImGuiWindowFlags_None : kNoScroll);
        ImGui::PopStyleColor();
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (0.35f + 0.65f * EaseOut(fadeTime / 0.15f)));
        if (s.Deleting() && (page == 0 || page == 1 || page == 3)) {
            Section("Удаление zapret…");
            ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), {S(260), S(10)}, "");
        } else switch (page) {
            case 0: DrawDashboard(s); break;
            case 1: DrawTerminal(s); break;
            case 2: DrawDiagnostics(s); break;
            case 3: DrawLists(s); break;
            case 4: DrawSettings(s, hwnd); break;
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopID();
        if (!terminal) DrawLogPanel(s);

        ImGui::EndChild();
        ImGui::GetForegroundDrawList()->AddRect({viewport->Pos.x + 0.5f, viewport->Pos.y + 0.5f},
            {viewport->Pos.x + viewport->Size.x - 0.5f, viewport->Pos.y + viewport->Size.y - 0.5f},
            Ink(C(255, 255, 255, 0.12f)), S(22));
    }
    ImGui::End();
    DrawTooltip();
}
