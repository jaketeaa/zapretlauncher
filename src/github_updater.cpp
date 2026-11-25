#include "github_updater.h"
#include "installation.h"
#include "http_client.h"
#include "service_control.h"
#include "utils.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <fstream>
#include <regex>
#include <sstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

const wchar_t* kReleaseApi = L"https://api.github.com/repos/Flowseal/zapret-discord-youtube/releases/latest";

std::wstring LowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

bool EndsWithLower(const std::string& s, const std::string& suffix) {
    std::string a = util::ToLowerAscii(s);
    return a.size() >= suffix.size() && a.compare(a.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Файлы пользователя, которые не нужно перезаписывать при обновлении
bool ShouldKeepExisting(const fs::path& rel) {
    std::wstring r = LowerW(rel.generic_wstring());
    if (r.rfind(L"lists/", 0) == 0) {
        if (r.size() >= 9 && r.compare(r.size() - 9, 9, L"-user.txt") == 0) return true;
        if (r == L"lists/ipset-all.txt" || r == L"lists/ipset-all.txt.backup") return true;
    }
    if (r.rfind(L"utils/", 0) == 0 && r.size() >= 8 && r.compare(r.size() - 8, 8, L".enabled") == 0) return true;
    return false;
}

} // namespace

GithubUpdater::GithubUpdater(fs::path zapretDir, fs::path configPath)
    : zapret_(std::move(zapretDir)), configPath_(std::move(configPath)) {}

void GithubUpdater::SetDirectory(fs::path directory) {
    if (worker_.joinable()) worker_.join();
    zapret_ = std::move(directory);
    std::lock_guard<std::mutex> lock(mtx_);
    latestTag_.clear();
    status_.clear();
    updateStatus_ = -1;
    state_ = State::Idle;
}

std::string GithubUpdater::LatestTag() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return latestTag_;
}

GithubUpdater::~GithubUpdater() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

std::vector<std::string> GithubUpdater::MissingFiles() const {
    std::vector<std::string> r;
    std::error_code ec;
    if (!fs::exists(zapret_ / L"service.bat", ec)) r.push_back("service.bat");
    if (!fs::exists(zapret_ / L"bin" / L"winws.exe", ec)) r.push_back("bin\\winws.exe");
    if (!fs::exists(zapret_ / L"bin" / L"WinDivert64.sys", ec)) r.push_back("bin\\WinDivert64.sys");
    return r;
}

std::string GithubUpdater::LocalTag() const {
    std::string s;
    // Установки, распакованные вручную, не имеют файла .launcher_version.
    std::ifstream script(zapret_ / L"service.bat");
    static const std::regex version(R"re(^\s*set\s+"?LOCAL_VERSION=([^"\s]+)"?\s*$)re", std::regex::icase);
    for (unsigned line = 0; line < 32 && std::getline(script, s); ++line) {
        std::smatch match;
        if (std::regex_match(s, match, version)) return match[1].str();
    }
    std::ifstream marker(zapret_ / L".launcher_version");
    s.clear();
    if (marker) std::getline(marker, s);
    return util::TrimCopy(s);
}

std::string GithubUpdater::GetStatus() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}
void GithubUpdater::SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(mtx_);
    status_ = s;
}
void GithubUpdater::Fail(const std::string& s) {
    SetStatus(s);
    ++generation_;
    state_ = State::Failed;
}
void GithubUpdater::Done(const std::string& s) {
    SetStatus(s);
    ++generation_;
    state_ = State::Success;
}

void GithubUpdater::Start(Task task) {
    if (state_.load() == State::Working) return;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    task_ = task;
    progress_ = -1.0f;
    state_ = State::Working;
    SetStatus(task == Task::Check ? "Проверка обновлений..." : "Подготовка к загрузке...");
    worker_ = std::thread([this, task] { Run(task); });
}

void GithubUpdater::Run(Task task) {
    try {
        if (task == Task::Check) DoCheck(); else DoInstall();
    } catch (const std::exception& e) {
        Fail(std::string("Непредвиденная ошибка: ") + e.what());
    } catch (...) {
        Fail("Непредвиденная ошибка при обновлении компонентов.");
    }
    if (state_.load() == State::Working) Fail("Операция завершилась без результата.");
}

// Последний релиз Flowseal: ищем прикреплённый .zip, затем .rar, затем zipball исходников
bool GithubUpdater::FetchRelease(Release& rel, std::string& err) {
    std::string body;
    if (!http::GetString(kReleaseApi, true, body, err)) return false;
    try {
        auto j = nlohmann::json::parse(body);
        rel.tag = j.value("tag_name", "");
        std::string zipUrl, zipName, rarUrl, rarName;
        if (j.contains("assets") && j["assets"].is_array()) {
            for (const auto& a : j["assets"]) {
                std::string name = a.value("name", "");
                std::string url = a.value("browser_download_url", "");
                if (url.empty()) continue;
                if (zipUrl.empty() && EndsWithLower(name, ".zip")) { zipUrl = url; zipName = name; }
                if (rarUrl.empty() && EndsWithLower(name, ".rar")) { rarUrl = url; rarName = name; }
            }
        }
        if (!zipUrl.empty())      { rel.downloadUrl = util::FromUtf8(zipUrl); rel.assetName = zipName; }
        else if (!rarUrl.empty()) { rel.downloadUrl = util::FromUtf8(rarUrl); rel.assetName = rarName; }
        else {
            std::string zb = j.value("zipball_url", "");
            if (zb.empty()) { err = "в релизе нет архива для скачивания"; return false; }
            rel.downloadUrl = util::FromUtf8(zb);
            rel.assetName = "source.zip";
        }
        if (rel.tag.empty()) rel.tag = j.value("name", "");
        return true;
    } catch (const std::exception&) {
        err = "не удалось разобрать ответ GitHub API";
        return false;
    }
}

void GithubUpdater::DoCheck() {
    Release rel;
    std::string err;
    updateStatus_ = -1;
    if (!FetchRelease(rel, err)) { Fail("Не удалось проверить обновления: " + err); return; }
    {
        std::lock_guard<std::mutex> lock(mtx_);
        latestTag_ = rel.tag;
    }
    const std::string local = LocalTag();
    if (!MissingFiles().empty() || local.empty()) {
        updateStatus_ = -1;
        Done("Последний релиз Flowseal: " + rel.tag + ". Установленная версия неизвестна.");
        return;
    }

    const auto parts = [](const std::string& tag) {
        std::vector<unsigned long> version;
        static const std::regex format(R"(v?[0-9]{1,9}(\.[0-9]{1,9})*)", std::regex::icase);
        if (!std::regex_match(tag, format)) return version;
        std::istringstream input(tag.substr(tag.front() == 'v' || tag.front() == 'V' ? 1 : 0));
        std::string part;
        while (std::getline(input, part, '.')) version.push_back(std::stoul(part));
        while (version.size() > 1 && version.back() == 0) version.pop_back();
        return version;
    };
    const auto installed = parts(local);
    const auto available = parts(rel.tag);
    if (local != rel.tag && (installed.empty() || available.empty())) {
        updateStatus_ = -1;
        Done("Установлено " + local + ". Последний релиз: " + rel.tag + ".");
        return;
    }
    const bool newer = installed < available;
    updateStatus_ = newer ? 1 : 0;
    Done(newer ? "Доступно обновление: " + rel.tag + " (установлено " + local + ")."
               : "Установленная версия: " + local + ".");
}

void GithubUpdater::DoInstall() {
    std::string preparation;
    if (!installation::Prepare(zapret_, configPath_, preparation)) { Fail(preparation); return; }
    std::error_code ec;
    fs::path tmp = zapret_ / (L".launcher_update_" + std::to_wstring(GetCurrentProcessId()) +
                              L"_" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directories(tmp, ec)) {
        Fail("Не удалось создать временную папку для загрузки.");
        return;
    }
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{tmp};

    // 1. Информация о релизе
    SetStatus("Получение информации о последнем релизе Flowseal...");
    Release rel;
    std::string err;
    if (!FetchRelease(rel, err)) { fs::remove_all(tmp, ec); Fail("Не удалось получить релиз с GitHub: " + err); return; }
    if (cancel_) { Fail("Отменено."); return; }

    bool isRar = EndsWithLower(rel.assetName, ".rar");
    fs::path archive = tmp / (isRar ? L"release.rar" : L"release.zip");

    // 2. Скачивание
    {
        std::ofstream out(archive, std::ios::binary | std::ios::trunc);
        if (!out) { fs::remove_all(tmp, ec); Fail("Не удалось создать временный файл: " + util::ToUtf8(archive.wstring())); return; }
        unsigned long long total = 0, got = 0;
        bool ok = http::Get(rel.downloadUrl, false,
            [&](const char* d, DWORD n) {
                out.write(d, n);
                got += n;
                char b[160];
                if (total) {
                    progress_ = (float)((double)got / (double)total);
                    std::snprintf(b, sizeof(b), "Скачивание %s: %.1f из %.1f МБ", rel.tag.c_str(), got / 1048576.0, total / 1048576.0);
                } else {
                    progress_ = -1.0f;
                    std::snprintf(b, sizeof(b), "Скачивание %s: %.1f МБ", rel.tag.c_str(), got / 1048576.0);
                }
                SetStatus(b);
                return (bool)out;
            },
            [&](unsigned long long t) { total = t; }, cancel_, err);
        out.close();
        if (!ok) { fs::remove_all(tmp, ec); Fail("Ошибка скачивания с GitHub: " + err); return; }
    }
    progress_ = -1.0f;

    // 3. Распаковка (tar.exe -> PowerShell для zip -> 7-Zip для rar)
    SetStatus("Распаковка архива...");
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    fs::path sysDir(sys);
    fs::path outDir = tmp / L"extracted";
    fs::create_directories(outDir, ec);

    int rc = util::RunHidden(L"\"" + (sysDir / L"tar.exe").wstring() + L"\" -xf \"" + archive.wstring() +
                             L"\" -C \"" + outDir.wstring() + L"\"", 600000);
    if (rc != 0 && !isRar) {
        SetStatus("Распаковка (PowerShell)...");
        fs::remove_all(outDir, ec);
        fs::create_directories(outDir, ec);
        auto esc = [](std::wstring s) {
            size_t p = 0;
            while ((p = s.find(L'\'', p)) != std::wstring::npos) { s.insert(p, 1, L'\''); p += 2; }
            return s;
        };
        std::wstring ps = L"\"" + (sysDir / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe").wstring() +
                          L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
                          esc(archive.wstring()) + L"' -DestinationPath '" + esc(outDir.wstring()) + L"' -Force\"";
        rc = util::RunHidden(ps, 900000);
    }
    if (rc != 0 && isRar) {
        const wchar_t* cands[] = {L"C:\\Program Files\\7-Zip\\7z.exe", L"C:\\Program Files (x86)\\7-Zip\\7z.exe"};
        for (const wchar_t* c : cands) {
            if (fs::exists(c, ec)) {
                fs::remove_all(outDir, ec);
                fs::create_directories(outDir, ec);
                rc = util::RunHidden(L"\"" + std::wstring(c) + L"\" x -y \"-o" + outDir.wstring() + L"\" \"" + archive.wstring() + L"\"", 900000);
                break;
            }
        }
    }
    if (rc != 0) {
        fs::remove_all(tmp, ec);
        Fail(isRar ? "Не удалось распаковать .rar (нужен 7-Zip или tar.exe с поддержкой RAR). Код " + std::to_string(rc) + "."
                   : "Не удалось распаковать архив (tar.exe и PowerShell завершились с ошибкой, код " + std::to_string(rc) + ").");
        return;
    }
    if (cancel_) { fs::remove_all(tmp, ec); Fail("Отменено."); return; }

    // 4. Корень сборки = самая "мелкая" папка, где лежит service.bat
    fs::path root;
    size_t bestDepth = (size_t)-1;
    for (fs::recursive_directory_iterator it(outDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        if (_wcsicmp(it->path().filename().c_str(), L"service.bat") != 0) continue;
        size_t depth = (size_t)std::distance(it->path().begin(), it->path().end());
        if (depth < bestDepth) { bestDepth = depth; root = it->path().parent_path(); }
    }
    if (root.empty()) {
        fs::remove_all(tmp, ec);
        Fail("В архиве релиза не найден service.bat. Структура репозитория Flowseal могла измениться.");
        return;
    }

    // 5. Останавливаем службу/WinDivert, чтобы можно было заменить файлы в bin/
    SetStatus("Подготовка к копированию файлов...");
    bool restartService = false;
    const svc::Info si = svc::Query(L"zapret");
    const bool serviceHere = si.installed && svc::IsInDirectory(zapret_);
    if (serviceHere && si.state != SERVICE_STOPPED) {
        SetStatus("Остановка службы zapret на время обновления...");
        if (!svc::Stop(L"zapret", 20000)) {
            Fail("Не удалось остановить службу zapret перед обновлением.");
            return;
        }
        restartService = si.state == SERVICE_RUNNING;
    }
    struct RestoreService {
        bool required;
        ~RestoreService() { if (required) svc::Start(L"zapret", 20000); }
    } restore{restartService};
    if (serviceHere) util::RunHidden(L"sc.exe stop WinDivert", 10000);
    if ((serviceHere || !si.installed) && svc::ProcessRunning(L"winws.exe")) {
        Fail("Запущен winws.exe. Завершите его работу и повторите.");
        return;
    }

    // 6. Копирование с сохранением структуры
    SetStatus("Копирование файлов в zapret/...");
    fs::create_directories(zapret_, ec);
    int copied = 0, kept = 0;
    std::string failed;
    int failCount = 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        fs::path rel = fs::relative(it->path(), root, e2);
        if (e2 || rel.empty()) continue;
        fs::path dest = zapret_ / rel;
        if (it->is_directory(e2)) { fs::create_directories(dest, e2); continue; }
        if (!it->is_regular_file(e2)) continue;
        std::error_code e3;
        if (fs::exists(dest, e3) && ShouldKeepExisting(rel)) { ++kept; continue; }
        fs::create_directories(dest.parent_path(), e3);
        std::error_code cec;
        fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, cec);
        if (cec) {
            ++failCount;
            if (failCount <= 5) failed += util::ToUtf8(rel.wstring()) + " (код " + std::to_string(cec.value()) + "); ";
        } else {
            ++copied;
        }
    }
    if (ec) ++failCount;

    if (failCount > 0) {
        Fail("Не удалось скопировать файлов: " + std::to_string(failCount) + ". " + failed + "Закройте программы, использующие эти файлы, и повторите.");
        return;
    }
    auto missing = MissingFiles();
    if (!missing.empty()) {
        std::string m;
        for (auto& x : missing) m += x + " ";
        Fail("После копирования всё ещё не хватает файлов: " + m);
        return;
    }

    std::ofstream version(zapret_ / L".launcher_version", std::ios::trunc);
    version << rel.tag;
    version.close();
    if (!version) {
        Fail("Файлы обновлены, но не удалось записать версию релиза.");
        return;
    }
    if (restore.required) {
        restore.required = false;
        if (!svc::Start(L"zapret", 20000)) {
            updateStatus_ = 0;
            Fail("Файлы обновлены. Не удалось запустить службу zapret.");
            return;
        }
    }
    updateStatus_ = 0;
    progress_ = 1.0f;
    Done("Готово: Flowseal " + rel.tag + ", скопировано файлов - " + std::to_string(copied) +
         (kept ? ", сохранено ваших файлов - " + std::to_string(kept) : std::string()) + ".");
}
