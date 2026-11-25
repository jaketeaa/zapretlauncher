#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace util {
namespace fs = std::filesystem;

std::string  ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

fs::path ExePath();
fs::path DocumentsDataDir();

// Запускает процесс без окна и ждёт завершения.
// Возвращает код выхода, -1 если запуск не удался, -2 если истёк таймаут.
int RunHidden(const std::wstring& cmdLine, DWORD timeoutMs, const fs::path* workDir = nullptr);

// Автозапуск вместе с Windows (через Планировщик заданий с правами администратора)
bool IsAutostartEnabled();
bool SetAutostart(bool enable, std::string& error);

int RunCapture(const std::wstring& cmdLine, DWORD timeoutMs, std::string& outUtf8, const fs::path* workDir = nullptr);

// Строковые помощники
std::string TrimCopy(const std::string& s);
std::string ToLowerAscii(std::string s);
// Ключ "естественной" сортировки: регистр не важен, числа сравниваются как числа
std::string NaturalKey(const std::string& s);
}
