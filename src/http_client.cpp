#include "http_client.h"
#include "app_version.h"
#include <winhttp.h>
#include <cwchar>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace http {

namespace {
const wchar_t* kUserAgent = L"ZapretLauncher/" LAUNCHER_AGENT_VERSION_W L" (Windows; WinHTTP)";

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x = nullptr) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
}

std::string ErrorText(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "не удалось найти сервер (проверьте интернет / DNS / VPN)";
        case ERROR_WINHTTP_TIMEOUT:           return "превышено время ожидания ответа";
        case ERROR_WINHTTP_CANNOT_CONNECT:    return "не удалось подключиться к серверу";
        case ERROR_WINHTTP_CONNECTION_ERROR:  return "соединение было прервано";
        case ERROR_WINHTTP_SECURE_FAILURE:    return "ошибка TLS/SSL (проверьте дату и время на компьютере)";
        default: return "ошибка WinHTTP " + std::to_string(code);
    }
}

static bool Request(const std::wstring& url, bool githubJson, const DataCb& onData, const LengthCb& onLength,
         const std::atomic<bool>& cancel, std::string& err, DWORD* probeStatus) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    wchar_t extra[2048]{};
    uc.lpszHostName = host;   uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;    uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        err = "некорректный URL";
        return false;
    }
    std::wstring hostStr(host, uc.dwHostNameLength);
    std::wstring fullPath = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    if (fullPath.empty()) fullPath = L"/";

    Handle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h)
        session.h = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { err = ErrorText(GetLastError()); return false; }
    if (probeStatus) WinHttpSetTimeouts(session.h, 3500, 3500, 3500, 5000);
    else WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);

    Handle conn(WinHttpConnect(session.h, hostStr.c_str(), uc.nPort, 0));
    if (!conn.h) { err = ErrorText(GetLastError()); return false; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    Handle req(WinHttpOpenRequest(conn.h, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req.h) { err = ErrorText(GetLastError()); return false; }

    DWORD policy = probeStatus ? WINHTTP_OPTION_REDIRECT_POLICY_NEVER
                               : WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    std::wstring hdr = std::wstring(L"User-Agent: ") + kUserAgent + L"\r\nCache-Control: no-cache";
    if (githubJson) hdr += L"\r\nAccept: application/vnd.github+json";
    WinHttpAddRequestHeaders(req.h, hdr.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        err = ErrorText(GetLastError());
        return false;
    }

    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        err = ErrorText(GetLastError());
        return false;
    }
    if (probeStatus) {
        *probeStatus = status;
        return true;
    }
    if (status != 200) {
        err = "сервер вернул HTTP " + std::to_string(status);
        if (status == 403 || status == 429) err += " (возможно, превышен лимит запросов GitHub, попробуйте позже)";
        return false;
    }

    wchar_t lenBuf[32]{};
    DWORD lenSize = sizeof(lenBuf);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX) && onLength)
        onLength(_wcstoui64(lenBuf, nullptr, 10));

    std::vector<char> buf(65536);
    for (;;) {
        if (cancel.load()) { err = "операция отменена"; return false; }
        DWORD read = 0;
        if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &read)) {
            err = ErrorText(GetLastError());
            return false;
        }
        if (read == 0) break;
        if (!onData(buf.data(), read)) { err = "ошибка записи на диск"; return false; }
    }
    return true;
}

bool Get(const std::wstring& url, bool githubJson, const DataCb& onData, const LengthCb& onLength,
         const std::atomic<bool>& cancel, std::string& err) {
    return Request(url, githubJson, onData, onLength, cancel, err, nullptr);
}

bool ProbeHttps(const std::wstring& url, DWORD& status, std::string& err) {
    static const std::atomic<bool> never{false};
    status = 0;
    return Request(url, false, nullptr, nullptr, never, err, &status);
}

bool GetString(const std::wstring& url, bool githubJson, std::string& body, std::string& err) {
    static const std::atomic<bool> never{false};
    body.clear();
    return Get(url, githubJson,
               [&](const char* d, DWORD n) { body.append(d, n); return body.size() < (64u * 1024u * 1024u); },
               nullptr, never, err);
}

} // namespace http
