#include "ui.h"
#include "utils.h"
#include <algorithm>

namespace {
template<class T>
bool Ready(std::future<T>& job) {
    return job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
bool Contains(const std::vector<std::string>& entries, const std::string& name) {
    return std::find(entries.begin(), entries.end(), name) != entries.end();
}
}

bool AppState::Favorite(const std::string& name) const {
    return Contains(config.settings.favoriteStrategies, name);
}

void AppState::ToggleFavorite(const std::string& name) {
    auto& favorites = config.settings.favoriteStrategies;
    if (Favorite(name)) favorites.erase(std::remove(favorites.begin(), favorites.end(), name), favorites.end());
    else if (Contains(strategies, name) && favorites.size() < 512) favorites.push_back(name);
    SaveConfig();
}

bool AppState::PickStrategy(const std::string& name) {
    if (!Contains(strategies, name)) return false;
    config.settings.selectedBat = name;
    auto& recent = config.settings.recentStrategies;
    recent.erase(std::remove(recent.begin(), recent.end(), name), recent.end());
    recent.insert(recent.begin(), name);
    if (recent.size() > 5) recent.resize(5);
    SaveConfig();
    return true;
}

bool AppState::OwnService() const {
    return svcStatus.installed && svcStatus.supported && svcStatus.usesDirectory;
}

bool AppState::CanControl() const {
    return !Restoring() && !Deleting() && !updater.Installing() && !svcMgr.Busy() && !term.IsRunning() &&
           !svcStatus.queryError && (svcStatus.state == SERVICE_RUNNING || svcStatus.state == SERVICE_STOPPED);
}

bool AppState::ApplyStrategy(const std::string& name) {
    if (Restoring() || Deleting() || updater.Installing() || svcMgr.Busy() || term.IsRunning() ||
        !svcStatus.serviceBatPresent || (svcStatus.installed && (!svcStatus.supported || !CanControl())) ||
        (svcStatus.queryError && svcStatus.queryError != ERROR_SERVICE_DOES_NOT_EXIST))
        return false;
    if (!PickStrategy(name)) return false;
    svcMgr.InstallStrategy(config.settings.selectedBat);
    return true;
}

void AppState::ToggleService() {
    if (OwnService() && CanControl()) svcMgr.SetRunning(svcStatus.state != SERVICE_RUNNING);
}

void AppState::StartDiagnostics(bool full) {
    if (diagnosticJob.valid()) return;
    try {
        diagnosticJob = std::async(std::launch::async, diagnostics::Run, full);
        const int interval = config.settings.monitorSeconds;
        const int delay = interval == 5 || interval == 30 || interval == 60 ? interval : 10;
        nextDiagnosis = beta::Clock::now() + std::chrono::seconds(delay);
    } catch (const std::exception&) {
        SetNotice("Не удалось начать диагностику.", true);
    }
}

void AppState::CheckLauncher() {
    if (launcherJob.valid()) return;
    try {
        launcherJob = std::async(std::launch::async, launcher_updates::Check, config.settings.updateChannel == "beta");
    } catch (const std::exception&) {
        SetNotice("Не удалось начать проверку обновлений лаунчера.", true);
    }
}

void AppState::UpdateUptime(beta::Clock::time_point now) {
    uptimeText = uptime.Text(now);
    nextUptime = uptime.NextChange(now);
}

void AppState::TickBeta() {
    const auto now = beta::Clock::now();
    if (now >= nextUptime) UpdateUptime(now);
    if (Ready(backupJob)) {
        try {
            auto result = backupJob.get();
            if (!result.error.empty()) SetNotice(result.error, true);
            else if (backupAction == BackupAction::Read) backupPreview = std::move(result.backup);
            else if (backupAction == BackupAction::Export)
                notifications.Add(beta::Kind::Success, "Резервная копия", "Резервная копия настроек создана.");
            else if (result.backup) {
                config.settings = std::move(result.backup->settings);
                termKillOnLeave = config.settings.terminalKillOnLeave;
                nextDiagnosis = beta::Clock::now();
                launcherRelease = {};
                lastLauncherCheck = {};
                selectedList.clear();
                listBuffer.clear();
                RefreshFiles();
                notifications.Add(beta::Kind::Success, "Резервная копия", "Настройки и списки восстановлены.");
            }
        } catch (const std::exception&) {
            SetNotice("Операция резервного копирования не завершена.", true);
        }
    }
    if (Ready(diagnosticJob)) {
        try {
            auto report = diagnosticJob.get();
            history.Add(report.network);
            diagnosticReport.network = std::move(report.network);
            std::string failed;
            for (const auto& row : diagnosticReport.network) {
                if (row.latencyMs) continue;
                if (!failed.empty()) failed += ", ";
                failed += row.name;
            }
            if (!failed.empty())
                notifications.Add(beta::Kind::Warning, "Нет HTTPS-ответа от сервисов", failed);
            if (!report.system.empty()) {
                diagnosticReport.system = std::move(report.system);
                const bool conflict = std::any_of(diagnosticReport.system.begin(), diagnosticReport.system.end(),
                    [](const auto& row) { return row.level != diagnostics::Level::Good; });
                notifications.Add(conflict ? beta::Kind::Warning : beta::Kind::Success,
                    "Диагностика завершена", conflict ? "Проверьте результаты системных проверок." : "Все системные проверки пройдены.");
            }
        } catch (const std::exception& e) {
            SetNotice(std::string("Ошибка диагностики: ") + e.what(), true);
        }
    }
    if (!Restoring() && config.settings.monitorOn && !diagnosticJob.valid() && beta::Clock::now() >= nextDiagnosis)
        StartDiagnostics(false);

    if (Ready(launcherJob)) {
        lastLauncherCheck = beta::WallClock::now();
        try {
            launcherRelease = launcherJob.get();
        } catch (const std::exception&) {
            launcherRelease = {};
            launcherRelease.error = "Проверка обновлений завершилась ошибкой.";
        }
        if (!launcherRelease.error.empty())
            notifications.Add(beta::Kind::Error, "Обновления лаунчера", launcherRelease.error);
        else if (launcherRelease.newer && launcherRelease.tag != snoozedTag)
            notifications.Add(beta::Kind::Info, "Доступно обновление лаунчера", launcherRelease.tag);
    }

    if (!snoozedTag.empty() && now >= remindAfter) {
        if (launcherRelease.newer && launcherRelease.tag == snoozedTag)
            notifications.Add(beta::Kind::Info, "Доступно обновление лаунчера", launcherRelease.tag);
        snoozedTag.clear();
    }
    const auto serviceState = svcMgr.GetState();
    if (serviceState != ServiceManager::State::Idle && serviceState != ServiceManager::State::Working &&
        svcMgr.Generation() != notifiedService) {
        notifiedService = svcMgr.Generation();
        const bool failed = serviceState == ServiceManager::State::Failed;
        notifications.Add(failed ? beta::Kind::Error : beta::Kind::Success, "Служба zapret", svcMgr.GetStatus());
    }
    const auto flowState = updater.GetState();
    if (flowState != GithubUpdater::State::Idle && flowState != GithubUpdater::State::Working &&
        updater.Generation() != notifiedFlowseal) {
        notifiedFlowseal = updater.Generation();
        const bool failed = flowState == GithubUpdater::State::Failed;
        notifications.Add(failed ? beta::Kind::Error : beta::Kind::Success, "Сборка Flowseal", updater.GetStatus());
    }
}

Settings AppState::BackupSettings() const {
    auto snapshot = config.settings;
    snapshot.backupAutostart = runWithWindows;
    if (!snapshot.filtersPending) {
        snapshot.gameFilter = svcStatus.gameMode;
        snapshot.ipsetFilter = svcStatus.ipsetMode == "missing" ? "none" : svcStatus.ipsetMode;
    }
    return snapshot;
}

void AppState::ChooseBackup(HWND owner, bool save) {
    if (backupJob.valid() || listDirty) return;
    std::string error;
    const auto path = settings_backup::PickPath(owner, save, error);
    if (!error.empty()) { SetNotice(error, true); return; }
    if (path.empty()) return;
    try {
        backupPreview.reset();
        backupAction = save ? BackupAction::Export : BackupAction::Read;
        auto snapshot = config;
        snapshot.settings = BackupSettings();
        backupJob = std::async(std::launch::async, [path, save, snapshot = std::move(snapshot)] {
            return save ? settings_backup::Export(snapshot, path) : settings_backup::Read(path);
        });
    } catch (const std::exception&) {
        SetNotice("Не удалось запустить операцию резервного копирования.", true);
    }
}

void AppState::RestoreBackup() {
    if (backupJob.valid() || !backupPreview || listDirty || autostartJob.valid() || Deleting() ||
        svcMgr.Busy() || updater.Busy() || term.IsRunning() || launcherJob.valid()) return;
    try {
        auto snapshot = config;
        snapshot.settings = BackupSettings();
        auto input = *backupPreview;
        backupAction = BackupAction::Restore;
        backupJob = std::async(std::launch::async, settings_backup::Restore, std::move(snapshot), std::move(input));
        backupPreview.reset();
    } catch (const std::exception&) {
        SetNotice("Не удалось запустить восстановление.", true);
    }
}
