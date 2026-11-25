#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>

namespace http {

using DataCb   = std::function<bool(const char*, DWORD)>;
using LengthCb = std::function<void(unsigned long long)>;

std::string ErrorText(DWORD code);
bool ProbeHttps(const std::wstring& url, DWORD& status, std::string& err);

// GET с редиректами, User-Agent и потоковой передачей данных в onData.
bool Get(const std::wstring& url, bool githubJson, const DataCb& onData, const LengthCb& onLength,
         const std::atomic<bool>& cancel, std::string& err);

// Удобная обёртка: скачать всё в строку.
bool GetString(const std::wstring& url, bool githubJson, std::string& body, std::string& err);

}
