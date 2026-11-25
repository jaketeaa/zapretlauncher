#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <future>
#include "diagnostics.h"

#include "config_manager.h"
#include "launcher_log.h"
#include "github_updater.h"
#include "service_manager.h"
#include "console_session.h"

struct ImFont;
class GlassRenderer;

// Сообщение главному окну: полный выход из программы
constexpr UINT WM_APP_EXIT = WM_APP + 2;

struct AppState {
    std::filesystem::path baseDir;      // ВАЖНО: объявлен первым (используется при инициализации ниже)
    ConfigManager config;
    LauncherLog log;
    GithubUpdater updater;
    ServiceManager svcMgr;
    ConsoleSession term;
    std::future<std::string> removalJob;
    bool managedInstallation = false;
    bool Deleting() const { return removalJob.valid(); }

    // Сообщение пользователю
    std::string notice;
    bool noticeError = false;

    // Кэш файлов и состояния
    std::vector<std::string> listFiles, missing, strategies;
    ServiceStatus svcStatus;
    std::string localVersion;
    unsigned lastGen = 0;
    unsigned long long lastRefresh = 0;

    std::future<diagnostics::Report> diagnosticJob;
    diagnostics::Report diagnosticReport;

    // Редактор списков
    std::string selectedList, listBuffer, newListName;
    bool listDirty = false;

    // Главная: предпочтительный режим Game Filter при включении
    std::string gameModePref = "all";

    // Настройки
    bool runWithWindows = false;
    std::future<std::string> autostartJob;

    // Логи
    bool logOpen = false;
    bool logAutoScroll = true;
    std::vector<std::string> logCache;
    size_t logCacheCounter = (size_t)-1;
    float logWidth = 0;
    float logLayoutScale = 0;

    // Терминал
    bool termStarted = false;
    bool termAutoScroll = true;
    bool termKillOnLeave = true;
    bool termTabWasActive = false;
    std::string termInput;
    std::vector<std::string> termLines;
    size_t termCounter = (size_t)-1;

    AppState();
    void Init();
    void Tick();                 // вызывать каждый кадр (даже когда окно скрыто)

    void ChooseDirectory(HWND owner);
    void RemoveInstallation();
    void StartTerminal();

    void SetNotice(const std::string& msg, bool isError);
    void SaveConfig();
    void RefreshFiles();
};

namespace ui {
void SetDpiScale(float scale);
void SetFonts(ImFont* title, ImFont* heading, ImFont* button, ImFont* caption, ImFont* mono);
void SetGlassRenderer(const GlassRenderer* renderer);
void SetLauncherIcon(unsigned long long texture);
void ApplyTheme(float dpiScale);
void Render(AppState& s, HWND mainWindow);

// Размеры кастомной шапки в пикселях (нужны для WM_NCHITTEST)
float TitleBarHeightPx();
float CaptionButtonsWidthPx();
}
