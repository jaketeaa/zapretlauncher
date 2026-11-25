#pragma once
#include <windows.h>
#include <string>
#include <filesystem>

namespace svc {

struct Info {
    bool installed = false;
    DWORD state = 0;
    DWORD pid = 0;
    DWORD error = 0;
};

Info        Query(const wchar_t* name);
struct ZapretInfo {
    Info service;
    std::filesystem::path executable;
    bool supported = false;
    bool sameDirectory = false;
    bool processRunning = false;
    unsigned long long processCreated = 0;
};
ZapretInfo QueryZapret(const std::filesystem::path& directory);
bool RemoveZapretService(const std::filesystem::path& expected, std::string& error);
std::string StateText(const Info& info);
bool        Stop(const wchar_t* name, DWORD timeoutMs);
bool        Start(const wchar_t* name, DWORD timeoutMs);
bool        ProcessRunning(const wchar_t* exeName, DWORD pid = 0);
// Имя стратегии, которое service.bat записывает в реестр при установке службы
std::string ReadStrategyName();
bool IsInDirectory(const std::filesystem::path& directory);
bool RemoveInstallation(const std::filesystem::path& directory, std::string& error);

}
