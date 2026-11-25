#include "diagnostics.h"
#include "http_client.h"
#include "service_control.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <future>

namespace diagnostics {
namespace {

Result CheckSite(Service service) {
    DWORD status = 0;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const bool connected = http::ProbeHttps(service.url, status, error);
    const auto finished = std::chrono::steady_clock::now();
    const auto date = std::chrono::system_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(finished - start).count();
    const bool available = connected && status >= 200 && status < 400;
    return {service.name, available ? Level::Good : Level::Error,
            connected ? "TLS установлен · HTTP " + std::to_string(status) : error,
            available ? std::optional<long long>{elapsed} : std::nullopt, date, finished};
}

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return std::towlower(c); });
    return text;
}

void AddMatch(std::string& matches, const wchar_t* name) {
    if (!matches.empty()) matches += ", ";
    matches += util::ToUtf8(name);
}

Result CheckDoh() {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const auto powershell = util::fs::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    const auto command = L"\"" + powershell.wstring() + LR"ps(" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; if (!(Get-Command Get-DnsClientDohServerAddress -ErrorAction SilentlyContinue)) { exit 2 }; $connected = @(Get-NetIPInterface | Where-Object ConnectionState -eq Connected | Select-Object -ExpandProperty InterfaceIndex); $servers = @(Get-DnsClientServerAddress | Where-Object {$_.InterfaceIndex -in $connected} | Select-Object -ExpandProperty ServerAddresses); if (Get-DnsClientDohServerAddress | Where-Object {$_.AutoUpgrade -and $_.ServerAddress -in $servers}) { exit 3 }; exit 0")ps";
    std::string output;
    const int code = util::RunCapture(command, 10000, output);
    if (code == 3)
        return {"Secure DNS / DoH", Level::Good, "Для DNS активного интерфейса настроен DoH."};
    if (code == 0)
        return {"Secure DNS / DoH", Level::Warning, "Автоматический DoH Windows не обнаружен. Проверьте Secure DNS в браузере."};
    return {"Secure DNS / DoH", Level::Warning, "Не удалось проверить DoH Windows. Проверьте настройки DNS вручную."};
}

std::vector<Result> CheckSystem() {
    std::vector<Result> results;
    const bool bfe = svc::Query(L"BFE").state == SERVICE_RUNNING;
    results.push_back({"Base Filtering Engine", bfe ? Level::Good : Level::Error,
                       bfe ? "Служба работает." : "Запустите службу BFE через «Службы» Windows."});

    std::string vpn, filters, drivers;
    const bool ownDriverExpected = svc::Query(L"zapret").state == SERVICE_RUNNING;
    bool scanned = false;
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE)) {
        DWORD needed = 0, count = 0;
        constexpr DWORD types = SERVICE_WIN32 | SERVICE_DRIVER;
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, types, SERVICE_ACTIVE,
                              nullptr, 0, &needed, &count, nullptr, nullptr);
        std::vector<BYTE> entries(needed);
        scanned = needed && EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, types, SERVICE_ACTIVE,
            entries.data(), static_cast<DWORD>(entries.size()), &needed, &count, nullptr, nullptr);
        if (scanned) {
            const auto* services = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(entries.data());
            for (DWORD i = 0; i < count; ++i) {
                const auto& service = services[i];
                const auto name = Lower(std::wstring(service.lpServiceName) + L" " + service.lpDisplayName);
                auto has = [&](const wchar_t* word) { return name.find(word) != std::wstring::npos; };
                if (has(L"vpn") || has(L"wireguard") || has(L"wintun") || has(L"tailscale") ||
                    has(L"happservice") || has(L"tracsrvwrapper") || has(L"zerotier") || has(L"tap-windows"))
                    AddMatch(vpn, service.lpServiceName);
                if (has(L"killer") || has(L"smartbyte") || has(L"connectivity network") || has(L"adguard"))
                    AddMatch(filters, service.lpServiceName);
                if (has(L"goodbyedpi") || (has(L"windivert") && !ownDriverExpected))
                    AddMatch(drivers, service.lpServiceName);
            }
        }
        CloseServiceHandle(scm);
    }
    if (svc::ProcessRunning(L"goodbyedpi.exe")) {
        if (!drivers.empty()) drivers += ", ";
        drivers += "goodbyedpi.exe";
    }

    auto add = [&](const char* name, const std::string& found, const char* advice) {
        if (!found.empty()) results.push_back({name, Level::Warning, found + ". " + advice});
        else if (scanned) results.push_back({name, Level::Good, "Активные компоненты по известным именам не обнаружены."});
        else results.push_back({name, Level::Warning, "Не удалось получить список служб и драйверов Windows."});
    };
    add("VPN и туннели", vpn, "При сбоях временно отключите VPN и повторите проверку.");
    add("Killer и сетевые фильтры", filters, "При сбоях проверьте работу без этого сетевого фильтра.");
    add("GoodbyeDPI и WinDivert", drivers, "Если работает другой фильтр, завершите его и повторите проверку.");
    results.push_back(CheckDoh());
    return results;
}
}

Report Run(bool includeSystem) {
    std::vector<std::future<Result>> requests;
    for (auto service : kServices)
        requests.push_back(std::async(std::launch::async, CheckSite, service));

    Report report;
    if (includeSystem) report.system = CheckSystem();
    for (auto& request : requests)
        report.network.push_back(request.get());
    return report;
}
}
