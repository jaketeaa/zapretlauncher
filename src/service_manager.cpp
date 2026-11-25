#include "service_manager.h"
#include "console_session.h"
#include "http_client.h"
#include "service_control.h"
#include "utils.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

const wchar_t* kIpsetUrl =
    L"https://raw.githubusercontent.com/Flowseal/zapret-discord-youtube/refs/heads/main/.service/ipset-service.txt";
const wchar_t* kHostsUrl =
    L"https://raw.githubusercontent.com/Flowseal/zapret-discord-youtube/refs/heads/main/.service/hosts";
const char* kIpsetNoneMarker = "203.0.113.113/32";

bool ReadBinary(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteBinary(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> r;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r') continue;
        if (c == '\n') { r.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) r.push_back(cur);
    return r;
}

void StripBom(std::string& s) {
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
}

// Определение режима ipset так же, как это делает service.bat
std::string IpsetModeOf(const fs::path& list) {
    std::error_code ec;
    if (!fs::exists(list, ec)) return "missing";
    std::string data;
    if (!ReadBinary(list, data)) return "missing";
    size_t lines = 0;
    for (char c : data) if (c == '\n') ++lines;
    if (!data.empty() && data.back() != '\n') ++lines;
    if (lines == 0) return "any";
    if (data.find(kIpsetNoneMarker) != std::string::npos) return "none";
    return "loaded";
}

void ParseGameFilter(const fs::path& file, ServiceStatus& st) {
    st.gameMode = "disabled";
    st.gameTcp = "1024-65535";
    st.gameUdp = "1024-65535";
    std::string data;
    if (!ReadBinary(file, data)) return;
    StripBom(data);
    std::string mode = "disabled", tcpC, udpC;
    for (const auto& raw : SplitLines(data)) {
        size_t eq = raw.find('=');
        std::string key = util::ToLowerAscii(util::TrimCopy(eq == std::string::npos ? raw : raw.substr(0, eq)));
        std::string val = eq == std::string::npos ? "" : util::TrimCopy(raw.substr(eq + 1));
        if (key == "mode") mode = util::ToLowerAscii(val);
        else if (key == "all") mode = "all";
        else if (key == "udp") { if (val.empty()) mode = "udp"; else udpC = val; }
        else if (key == "tcp") { if (val.empty()) mode = "tcp"; else tcpC = val; }
    }
    if (mode != "all" && mode != "tcp" && mode != "udp") mode = "disabled";
    st.gameMode = mode;
    if (!tcpC.empty()) st.gameTcp = tcpC;
    if (!udpC.empty()) st.gameUdp = udpC;
}

// Номер пункта меню service.bat по ключевому слову: строки вида "   1. Install Service"
int FindMenuNumber(const std::string& text, const std::string& keywordLower) {
    for (const auto& raw : SplitLines(text)) {
        std::string line = util::TrimCopy(raw);
        size_t i = 0;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
        if (i == 0 || i >= line.size() || line[i] != '.') continue;
        std::string label = util::ToLowerAscii(line.substr(i + 1));
        if (label.find(keywordLower) != std::string::npos) return std::atoi(line.substr(0, i).c_str());
    }
    return -1;
}

// Номер файла в списке "Pick one of the options"
int FindListNumber(const std::string& text, const std::string& batName) {
    std::string want = util::ToLowerAscii(batName);
    for (const auto& raw : SplitLines(text)) {
        std::string line = util::TrimCopy(raw);
        size_t i = 0;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
        if (i == 0 || i >= line.size() || line[i] != '.') continue;
        int num = std::atoi(line.substr(0, i).c_str());
        if (num <= 0) continue;
        std::string name = util::ToLowerAscii(util::TrimCopy(line.substr(i + 1)));
        if (name == want) return num;
    }
    return -1;
}

} // namespace

// =====================================================================

ServiceManager::ServiceManager(fs::path zapretDir, LogFn log) : zapret_(std::move(zapretDir)), log_(std::move(log)) {}

ServiceManager::~ServiceManager() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void ServiceManager::SetDirectory(fs::path directory) {
    if (worker_.joinable()) worker_.join();
    zapret_ = std::move(directory);
    state_ = State::Idle;
    SetStatus({});
}

std::string ServiceManager::GetStatus() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}
void ServiceManager::SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(mtx_);
    status_ = s;
}
void ServiceManager::Done(const std::string& s) {
    SetStatus(s);
    Log(s);
    ++generation_;
    state_ = State::Success;
}
void ServiceManager::Fail(const std::string& s) {
    SetStatus(s);
    Log("ОШИБКА: " + s);
    ++generation_;
    state_ = State::Failed;
}
void ServiceManager::Log(const std::string& s) { if (log_) log_("[лаунчер] " + s); }

void ServiceManager::LogOutput(const std::string& text) {
    if (!log_) return;
    for (const auto& raw : SplitLines(text)) {
        std::string line = util::TrimCopy(raw);
        if (!line.empty()) log_(line);
    }
}

void ServiceManager::Launch(const std::string& title, std::function<void()> job) {
    if (state_.load() == State::Working) return;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    state_ = State::Working;
    SetStatus(title);
    worker_ = std::thread([this, job]() {
        try {
            job();
        } catch (const std::exception& e) {
            Fail(std::string("Непредвиденная ошибка: ") + e.what());
        } catch (...) {
            Fail("Непредвиденная ошибка.");
        }
        if (state_.load() == State::Working) Fail("Операция завершилась без результата.");
    });
}

// ---------------------------------------------------------------------
//  Статус (быстрый, без запуска внешних программ)
// ---------------------------------------------------------------------
ServiceStatus ServiceManager::ReadStatus(const fs::path& z) {
    ServiceStatus st;
    std::error_code ec;
    st.serviceBatPresent = fs::exists(z / L"service.bat", ec);
    st.winwsPresent = fs::exists(z / L"bin" / L"winws.exe", ec);
    st.driverPresent = fs::exists(z / L"bin" / L"WinDivert64.sys", ec);

    const auto snapshot = svc::QueryZapret(z);
    st.installed = snapshot.service.installed;
    st.state = snapshot.service.state;
    st.queryError = snapshot.service.error;
    st.stateText = svc::StateText(snapshot.service);
    st.usesDirectory = snapshot.sameDirectory;
    st.executable = snapshot.executable;
    st.supported = snapshot.supported;
    st.winwsRunning = snapshot.processRunning;
    st.processCreated = snapshot.processCreated;
    if (st.installed) st.strategy = svc::ReadStrategyName();

    ParseGameFilter(z / L"utils" / L"game_filter.enabled", st);

    fs::path list = z / L"lists" / L"ipset-all.txt";
    st.ipsetMode = IpsetModeOf(list);
    fs::path backup = list;
    backup += L".backup";
    st.ipsetBackup = fs::exists(backup, ec);
    return st;
}

// ---------------------------------------------------------------------
//  Работа через service.bat
// ---------------------------------------------------------------------
bool ServiceManager::OpenMenu(ConsoleSession& cs, size_t& pos, std::string& menuText, std::string& err) {
    fs::path bat = zapret_ / L"service.bat";
    std::error_code ec;
    if (!fs::exists(bat, ec)) {
        err = "service.bat не найден в папке zapret/. Скачайте сборку Flowseal.";
        return false;
    }
    if (!cs.Start(ConsoleSession::MakeServiceBatCommand(), zapret_, err)) return false;
    pos = 0;
    if (!cs.WaitFor("Select option (0-", pos, 60000, &cancel_)) {
        std::string tail = util::TrimCopy(cs.FeedRange(0, cs.FeedEnd()));
        if (tail.size() > 400) tail = tail.substr(tail.size() - 400);
        err = "service.bat не показал меню." + (tail.empty() ? std::string() : " Вывод: " + tail);
        return false;
    }
    menuText = cs.FeedRange(0, pos);
    return true;
}

// После выбора пункта: подтверждаем "Нажмите любую клавишу" (определяем по хвосту ". . .", он есть на
// любом языке Windows), на возвращённом меню отправляем 0 и ждём выхода. Возвращает конец вывода действия.
size_t ServiceManager::FinishToMenuAndExit(ConsoleSession& cs, size_t& pos) {
    const size_t npos = std::string::npos;
    size_t actionEnd = npos;
    ULONGLONG deadline = GetTickCount64() + 240000;
    ULONGLONG lastNudge = GetTickCount64();

    while (!cancel_.load() && GetTickCount64() < deadline) {
        size_t pPause = npos, pMenu = npos;
        bool hasPause = cs.Find(". . .", pos, pPause);
        bool hasMenu = cs.Find("Select option (0-", pos, pMenu);

        if (hasPause && (!hasMenu || pPause < pMenu)) {
            if (actionEnd == npos) actionEnd = pPause;
            cs.Send("\n");
            pos = pPause + 5;
            lastNudge = GetTickCount64();
            continue;
        }
        if (hasMenu) {
            if (actionEnd == npos) actionEnd = pMenu;
            pos = pMenu + 17;
            cs.SendLine("0");
            cs.WaitExit(20000);
            break;
        }
        if (!cs.IsRunning()) break;

        // Запасной вариант: долгая тишина - вероятно, ждём нажатия клавиши
        ULONGLONG now = GetTickCount64();
        if (actionEnd == npos && now - cs.LastActivity() > 6000 && now - lastNudge > 6000) {
            cs.Send("\n");
            lastNudge = now;
        }
        Sleep(40);
    }
    if (actionEnd == npos) actionEnd = cs.FeedEnd();
    return actionEnd;
}

bool ServiceManager::InstallCore(const std::string& bat, std::string& msg) {
    if (bat.empty()) { msg = "Не выбрана стратегия."; return false; }
    std::error_code ec;
    if (!fs::exists(zapret_ / util::FromUtf8(bat), ec)) { msg = "Файл стратегии не найден: " + bat; return false; }

    SetStatus("Установка службы: " + bat);
    Log("Установка службы со стратегией: " + bat);

    ConsoleSession cs;
    size_t pos = 0;
    std::string menu, err;
    if (!OpenMenu(cs, pos, menu, err)) { msg = err; return false; }

    int installNo = FindMenuNumber(menu, "install service");
    if (installNo <= 0) installNo = 1;

    size_t listStart = pos;
    cs.SendLine(std::to_string(installNo));
    // Список файлов формируется через PowerShell, это может занять несколько секунд
    if (!cs.WaitFor("Input option (0-", pos, 120000, &cancel_)) {
        cs.Stop();
        msg = "service.bat не показал список стратегий.";
        return false;
    }
    std::string list = cs.FeedRange(listStart, pos);
    int idx = FindListNumber(list, bat);
    if (idx <= 0) {
        cs.SendLine("0");
        cs.WaitExit(10000);
        cs.Stop();
        msg = "service.bat не нашёл \"" + bat + "\" в своём списке стратегий.";
        return false;
    }

    size_t actionStart = pos;
    cs.SendLine(std::to_string(idx));
    size_t actionEnd = FinishToMenuAndExit(cs, pos);
    LogOutput(cs.FeedRange(actionStart, actionEnd));
    cs.Stop();

    const auto expected = fs::path(util::FromUtf8(bat)).stem().wstring();
    auto st = ReadStatus(zapret_);
    auto ready = [&] {
        return st.installed && st.state == SERVICE_RUNNING && st.usesDirectory &&
               _wcsicmp(util::FromUtf8(st.strategy).c_str(), expected.c_str()) == 0;
    };
    const auto deadline = GetTickCount64() + 4000;
    while (!ready() && !cancel_ && GetTickCount64() < deadline) {
        Sleep(150);
        st = ReadStatus(zapret_);
    }
    if (!ready()) {
        msg = "Не удалось подтвердить запуск выбранной стратегии. Служба — " + st.stateText +
              ". Стратегия — " + (st.strategy.empty() ? "не определена" : st.strategy) + ".";
        if (st.installed)
            msg += " Путь службы — " + (st.executable.empty() ? "не определён" : util::ToUtf8(st.executable.wstring())) + ".";
        return false;
    }
    msg = "Служба zapret установлена (стратегия: " + (st.strategy.empty() ? bat : st.strategy) + "), состояние: " + st.stateText + ".";
    return true;
}

bool ServiceManager::RestartServiceIfRunning(std::string& note) {
    svc::Info i = svc::Query(L"zapret");
    if (!i.installed || i.state != SERVICE_RUNNING || !svc::IsInDirectory(zapret_)) { note.clear(); return true; }
    SetStatus("Перезапуск службы zapret...");
    bool ok = svc::Stop(L"zapret", 20000) && svc::Start(L"zapret", 20000);
    note = ok ? " Служба zapret перезапущена." : " Не удалось перезапустить службу zapret - запустите её вручную.";
    return ok;
}

// ---------------------------------------------------------------------
//  IPSet
// ---------------------------------------------------------------------
bool ServiceManager::UpdateIpsetCore(std::string& msg) {
    SetStatus("Скачивание ipset-all.txt...");
    std::string body, err;
    if (!http::GetString(kIpsetUrl, false, body, err)) { msg = "Не удалось скачать список IPSet: " + err; return false; }
    StripBom(body);
    if (util::TrimCopy(body).empty()) { msg = "Сервер вернул пустой список IPSet."; return false; }

    fs::path list = zapret_ / L"lists" / L"ipset-all.txt";
    fs::path tmp = list;
    tmp += L".tmp";
    if (!WriteBinary(tmp, body)) { msg = "Не удалось записать " + util::ToUtf8(tmp.wstring()); return false; }
    std::error_code ec;
    fs::rename(tmp, list, ec);
    if (ec) {
        fs::copy_file(tmp, list, fs::copy_options::overwrite_existing, ec);
        std::error_code ec2;
        fs::remove(tmp, ec2);
        if (ec) { msg = "Не удалось заменить ipset-all.txt (код " + std::to_string(ec.value()) + ")."; return false; }
    }
    msg = "Список IPSet обновлён.";
    return true;
}

bool ServiceManager::SetIpsetCore(const std::string& mode, std::string& msg) {
    fs::path list = zapret_ / L"lists" / L"ipset-all.txt";
    fs::path backup = list;
    backup += L".backup";
    std::string current = IpsetModeOf(list);
    if (current == mode) { msg = "IPSet уже в режиме \"" + mode + "\"."; return true; }
    std::error_code ec;

    if (mode == "none" || mode == "any") {
        if (current == "loaded") {
            fs::copy_file(list, backup, fs::copy_options::overwrite_existing, ec);
            if (ec) { msg = "Не удалось создать резервную копию ipset-all.txt.backup."; return false; }
        }
        std::string content = (mode == "none") ? std::string(kIpsetNoneMarker) + "\r\n" : std::string();
        if (!WriteBinary(list, content)) { msg = "Не удалось записать ipset-all.txt."; return false; }
        msg = "IPSet переключён в режим \"" + mode + "\".";
        return true;
    }
    if (mode == "loaded") {
        if (fs::exists(backup, ec)) {
            fs::copy_file(backup, list, fs::copy_options::overwrite_existing, ec);
            if (ec) { msg = "Не удалось восстановить список из резервной копии."; return false; }
            std::error_code ec2;
            fs::remove(backup, ec2);
            msg = "IPSet переключён в режим \"loaded\" (список восстановлен из резервной копии).";
            return true;
        }
        std::string m;
        if (!UpdateIpsetCore(m)) { msg = "Резервной копии нет, а скачать список не удалось. " + m; return false; }
        msg = "IPSet переключён в режим \"loaded\" (резервной копии не было - скачан свежий список).";
        return true;
    }
    msg = "Неизвестный режим IPSet: " + mode;
    return false;
}

// ---------------------------------------------------------------------
//  hosts
// ---------------------------------------------------------------------
bool ServiceManager::UpdateHostsCore(std::string& msg) {
    SetStatus("Скачивание блока hosts...");
    std::string body, err;
    if (!http::GetString(kHostsUrl, false, body, err)) { msg = "Не удалось скачать hosts: " + err; return false; }
    StripBom(body);

    std::vector<std::string> block = SplitLines(body);
    while (!block.empty() && util::TrimCopy(block.back()).empty()) block.pop_back();
    while (!block.empty() && util::TrimCopy(block.front()).empty()) block.erase(block.begin());
    if (block.empty()) { msg = "Сервер вернул пустой блок hosts."; return false; }

    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    fs::path hostsPath = fs::path(sys) / L"drivers" / L"etc" / L"hosts";
    fs::path bakPath = hostsPath.parent_path() / L"hosts.bak";

    std::string current;
    std::error_code ec;
    if (fs::exists(hostsPath, ec) && !ReadBinary(hostsPath, current)) { msg = "Не удалось прочитать файл hosts."; return false; }
    StripBom(current);
    std::vector<std::string> cur = SplitLines(current);

    auto same = [](const std::string& a, const std::string& b) { return util::TrimCopy(a) == util::TrimCopy(b); };

    // 1. Блок уже присутствует целиком?
    if (cur.size() >= block.size()) {
        for (size_t i = 0; i + block.size() <= cur.size(); ++i) {
            bool all = true;
            for (size_t k = 0; k < block.size() && all; ++k) all = same(cur[i + k], block[k]);
            if (all) { msg = "Файл hosts уже содержит актуальный блок - изменения не требуются."; return true; }
        }
    }

    // 2. Резервная копия (обязательно)
    if (fs::exists(hostsPath, ec)) {
        fs::copy_file(hostsPath, bakPath, fs::copy_options::overwrite_existing, ec);
        if (ec) { msg = "Не удалось создать резервную копию hosts.bak (код " + std::to_string(ec.value()) + "). Файл hosts не изменён."; return false; }
    }

    // 3. Заменяем старый блок (по первой и последней строке) или добавляем в конец
    std::vector<std::string> result;
    size_t first = std::string::npos, last = std::string::npos;
    for (size_t i = 0; i < cur.size(); ++i)
        if (same(cur[i], block.front())) { first = i; break; }
    if (first != std::string::npos)
        for (size_t i = cur.size(); i-- > first;)
            if (same(cur[i], block.back())) { last = i; break; }

    bool replaced = (first != std::string::npos && last != std::string::npos);
    if (replaced) {
        result.assign(cur.begin(), cur.begin() + (std::ptrdiff_t)first);
        result.insert(result.end(), block.begin(), block.end());
        result.insert(result.end(), cur.begin() + (std::ptrdiff_t)last + 1, cur.end());
    } else {
        result = cur;
        while (!result.empty() && util::TrimCopy(result.back()).empty()) result.pop_back();
        if (!result.empty()) result.push_back("");
        result.insert(result.end(), block.begin(), block.end());
    }

    std::string out;
    for (const auto& l : result) { out += l; out += "\r\n"; }

    DWORD attrs = GetFileAttributesW(hostsPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(hostsPath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);

    if (!WriteBinary(hostsPath, out)) {
        std::error_code ec2;
        if (fs::exists(bakPath, ec2)) fs::copy_file(bakPath, hostsPath, fs::copy_options::overwrite_existing, ec2);
        msg = "Не удалось записать файл hosts (его может блокировать антивирус). Изменения откатены, копия: hosts.bak.";
        return false;
    }
    util::RunHidden(L"ipconfig.exe /flushdns", 15000);
    msg = std::string(replaced ? "Блок в файле hosts заменён на актуальный." : "Актуальный блок добавлен в конец файла hosts.") +
          " Резервная копия: hosts.bak. Кэш DNS очищен.";
    return true;
}

// ---------------------------------------------------------------------
//  Публичные операции
// ---------------------------------------------------------------------
void ServiceManager::InstallStrategy(const std::string& batName) {
    Launch("Установка службы...", [this, batName]() {
        std::string msg;
        if (InstallCore(batName, msg)) Done(msg); else Fail(msg);
    });
}

void ServiceManager::SetRunning(bool running) {
    Launch(running ? "Запуск службы zapret..." : "Остановка службы zapret...", [this, running] {
        const auto status = svc::QueryZapret(zapret_);
        if (!status.supported || !status.sameDirectory) {
            Fail("Не удалось подтвердить службу из выбранной папки.");
            return;
        }
        const bool ok = running ? svc::Start(L"zapret", 20000) : svc::Stop(L"zapret", 20000);
        if (ok) Done(running ? "Служба zapret запущена." : "Служба zapret остановлена.");
        else Fail(running ? "Не удалось запустить службу zapret." : "Не удалось остановить службу zapret.");
    });
}

void ServiceManager::Restart() {
    Launch("Перезапуск службы zapret...", [this] {
        const auto status = svc::QueryZapret(zapret_);
        if (!status.supported || !status.sameDirectory || status.service.state != SERVICE_RUNNING) {
            Fail("Не удалось подтвердить работающую службу из выбранной папки.");
            return;
        }
        if (svc::Stop(L"zapret", 20000) && svc::Start(L"zapret", 20000))
            Done("Служба zapret перезапущена.");
        else Fail("Не удалось перезапустить службу zapret.");
    });
}

void ServiceManager::RemoveService(fs::path expected) {
    Launch("Удаление службы zapret...", [this, expected = std::move(expected)] {
        std::string error;
        if (svc::RemoveZapretService(expected, error)) Done("Служба zapret удалена.");
        else Fail(error);
    });
}

void ServiceManager::UpdateIpset() {
    Launch("Обновление списка IPSet...", [this]() {
        std::string msg, note;
        if (!UpdateIpsetCore(msg)) { Fail(msg); return; }
        if (RestartServiceIfRunning(note)) Done(msg + note);
        else Fail(msg + note);
    });
}

void ServiceManager::UpdateHosts() {
    Launch("Обновление файла hosts...", [this]() {
        std::string msg;
        if (UpdateHostsCore(msg)) Done(msg); else Fail(msg);
    });
}

void ServiceManager::SetIpsetMode(const std::string& mode) {
    Launch("Переключение IPSet Filter...", [this, mode]() {
        std::string msg, note;
        if (!SetIpsetCore(mode, msg)) { Fail(msg); return; }
        if (RestartServiceIfRunning(note)) Done(msg + note);
        else Fail(msg + note);
    });
}

void ServiceManager::SetGameFilter(const std::string& mode) {
    Launch("Переключение Game Filter...", [this, mode]() {
        ServiceStatus st = ReadStatus(zapret_);
        std::string content = "mode=" + mode + "\r\ntcp=" + st.gameTcp + "\r\nudp=" + st.gameUdp + "\r\n";
        if (!WriteBinary(zapret_ / L"utils" / L"game_filter.enabled", content)) {
            Fail("Не удалось записать utils\\game_filter.enabled.");
            return;
        }
        std::string label = mode == "disabled" ? "выключен" : mode == "all" ? "включён (TCP и UDP)"
                          : mode == "tcp" ? "включён (TCP)" : "включён (UDP)";
        // Порты игр подставляются в аргументы службы при установке, поэтому стратегию нужно переустановить
        if (st.installed && !st.strategy.empty()) {
            std::string bat = st.strategy + ".bat";
            std::error_code ec;
            if (fs::exists(zapret_ / util::FromUtf8(bat), ec)) {
                Log("Game Filter " + label + ". Переустановка стратегии для применения порта...");
                std::string msg;
                if (InstallCore(bat, msg)) Done("Game Filter " + label + ". " + msg);
                else Fail("Game Filter " + label + ", но переустановить службу не удалось: " + msg);
                return;
            }
        }
        Done("Game Filter " + label + ". Установите стратегию заново, чтобы изменение вступило в силу.");
    });
}
