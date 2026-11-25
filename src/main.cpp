#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <vector>
#include <cwchar>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include "glass_renderer.h"
#include "app_version.h"
#include "font_resources.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "ui.h"
#include "utils.h"

#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_APP_SHOW = WM_APP + 3;
enum { ID_TRAY_OPEN = 1001, ID_TRAY_EXIT = 1003 };
const wchar_t* kWndClass = L"ZapretLauncherWnd";

AppState* g_app = nullptr;
HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbarCreated = 0;
float g_scale = 1.0f;
bool g_imguiReady = false;
bool g_inRender = false;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
GlassRenderer g_glass;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_launcherIcon;
constexpr int kDiagnosticIconResources[] = {
    PNG_DIAGNOSTIC_DISCORD, PNG_DIAGNOSTIC_YOUTUBE, PNG_DIAGNOSTIC_CLOUDFLARE, PNG_DIAGNOSTIC_GITHUB
};
static_assert(std::size(kDiagnosticIconResources) == std::size(diagnostics::kServices));
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_diagnosticIcons[std::size(kDiagnosticIconResources)];

// ---------------- DirectX 11 ----------------
void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void CleanupRenderTarget() {
    if (g_ctx) g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &level, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &level, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

bool LoadPngTexture(HINSTANCE instance, int id, UINT size,
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view) {
    using Microsoft::WRL::ComPtr;
    const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return false;
    const DWORD length = SizeofResource(instance, resource);
    const auto loaded = LoadResource(instance, resource);
    auto* source = static_cast<BYTE*>(loaded ? LockResource(loaded) : nullptr);
    if (!source || !length) return false;

    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return false;
    struct Apartment {
        bool owned;
        ~Apartment() { if (owned) CoUninitialize(); }
    } com{SUCCEEDED(apartment)};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> rgba;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(source, length)) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), size, size, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&rgba)) ||
        FAILED(rgba->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    std::vector<BYTE> pixels(size * size * 4);
    if (FAILED(rgba->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return false;

    // Keep transparent texels close to the dark rim for straight-alpha mip filtering.
    if (id == PNG_ZAPRET_LAUNCHER) {
        for (size_t i = 0; i < pixels.size(); i += 4) {
            if (!pixels[i + 3]) { pixels[i] = 41; pixels[i + 1] = 39; pixels[i + 2] = 40; }
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = size;
    desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &texture)) ||
        FAILED(g_device->CreateShaderResourceView(texture.Get(), nullptr, &view))) return false;
    g_ctx->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), size * 4, 0);
    g_ctx->GenerateMips(view.Get());
    return true;
}

void CleanupDevice() {
    ui::SetLauncherIcon(0);
    g_launcherIcon.Reset();
    for (size_t i = 0; i < std::size(g_diagnosticIcons); ++i) {
        ui::SetDiagnosticIcon(i, 0);
        g_diagnosticIcons[i].Reset();
    }
    CleanupRenderTarget();
    g_glass = {};
    if (g_swap)   { g_swap->Release();   g_swap = nullptr; }
    if (g_ctx)    { g_ctx->Release();    g_ctx = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

void ResizeSwapChain(UINT w, UINT h) {
    if (!g_swap || w == 0 || h == 0) return;
    CleanupRenderTarget();
    g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

// Один кадр: вызывается из главного цикла и из WM_SIZE (плавное изменение размера окна)
void RenderFrame() {
    if (!g_app || !g_imguiReady || !g_device || !g_rtv || g_inRender) return;
    g_inRender = true;

    RECT client{};
    GetClientRect(g_hwnd, &client);
    const bool glass = g_glass.Prepare(g_device, g_ctx, client.right, client.bottom);
    ui::SetGlassRenderer(glass ? &g_glass : nullptr);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ui::Render(*g_app, g_hwnd);
    ImGui::Render();

    const float clear[4] = {12 / 255.f, 12 / 255.f, 14 / 255.f, 1.0f};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    HRESULT hr = g_swap->Present(1, 0);
    g_inRender = false;
    if (hr == DXGI_STATUS_OCCLUDED) Sleep(50);
}

// ---------------- Трей ----------------
HICON LauncherIcon(bool smallIcon, UINT dpi = GetDpiForSystem()) {
    const int width = GetSystemMetricsForDpi(smallIcon ? SM_CXSMICON : SM_CXICON, dpi);
    const int height = GetSystemMetricsForDpi(smallIcon ? SM_CYSMICON : SM_CYICON, dpi);
    struct IconCache {
        struct Entry { int width, height; HICON icon; };
        std::vector<Entry> entries;
        ~IconCache() { for (const auto& entry : entries) DestroyIcon(entry.icon); }
    };
    static IconCache cache;
    for (const auto& entry : cache.entries) {
        if (entry.width == width && entry.height == height) return entry.icon;
    }
    const auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_ZAPRET_LAUNCHER),
                                                   IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
    if (icon) cache.entries.push_back({width, height, icon});
    return icon;
}

void SetWindowIcons(HWND hwnd, UINT dpi) {
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LauncherIcon(false, dpi)));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LauncherIcon(true, dpi)));
}

void TrayAdd(HWND hwnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LauncherIcon(true);
    wcscpy_s(g_nid.szTip, L"Zapret Launcher - остановлен");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void TrayUpdateTip(bool running) {
    wcscpy_s(g_nid.szTip, running ? L"Zapret Launcher - активен" : L"Zapret Launcher - остановлен");
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void ShowMain() {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Открыть");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Выход");
    SetMenuDefaultItem(menu, ID_TRAY_OPEN, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------- Кастомная рамка окна ----------------
void ApplyWindowRegion(HWND hwnd, int width, int height) {
    if (width < 1 || height < 1) return;
    const int diameter = std::max(2, static_cast<int>(std::lround(44.0f * g_scale)));
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    if (region && !SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
}

void ApplyCustomFrame(HWND hwnd, int width, int height) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const auto preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
    const COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    // A region gives the borderless HWND the same 22-DIP radius on Windows 10 and 11.
    ApplyWindowRegion(hwnd, width, height);
}

void LoadFonts(HINSTANCE instance) {
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x20AC, 0x20AC,
                                    0x2116, 0x2116, 0x2190, 0x2193, 0x2500, 0x257F, 0};
    auto load = [&](WORD id, float size) -> ImFont* {
        const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
        const auto data = resource ? LoadResource(instance, resource) : nullptr;
        void* bytes = data ? LockResource(data) : nullptr;
        if (!bytes) return nullptr;
        ImFontConfig options;
        options.FontDataOwnedByAtlas = false;
        return io.Fonts->AddFontFromMemoryTTF(bytes, static_cast<int>(SizeofResource(instance, resource)),
                                              size * g_scale, &options, ranges);
    };
    io.FontDefault = load(FONT_REGULAR, 16.0f);
    if (!io.FontDefault) io.FontDefault = io.Fonts->AddFontDefault();
    ui::SetFonts(load(FONT_BOLD, 27.0f), load(FONT_SEMIBOLD, 19.0f), load(FONT_SEMIBOLD, 16.0f),
                 load(FONT_MEDIUM, 14.0f), load(FONT_MONO, 14.0f));
}

LRESULT HitTest(HWND hWnd, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hWnd, &pt);
    int titleH = (int)ui::TitleBarHeightPx();
    int btnsW = (int)ui::CaptionButtonsWidthPx();
    if (pt.y >= 0 && pt.y < titleH) {
        RECT client{};
        GetClientRect(hWnd, &client);
        return pt.x >= client.right - btnsW ? HTCLIENT : HTCAPTION;
    }
    return HTCLIENT;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    if (g_taskbarCreated && msg == g_taskbarCreated) {   // Explorer перезапущен
        TrayAdd(hWnd);
        return 0;
    }

    switch (msg) {
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST:
        return HitTest(hWnd, lParam);
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED) {
            ResizeSwapChain((UINT)LOWORD(lParam), (UINT)HIWORD(lParam));
            RenderFrame();
        }
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowIcons(hWnd, HIWORD(wParam));
        MONITORINFO monitor{sizeof(MONITORINFO)};
        g_scale = HIWORD(wParam) / 96.0f;
        if (GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &monitor)) {
            g_scale = std::min({g_scale, (monitor.rcWork.right - monitor.rcWork.left) / 1000.0f,
                               (monitor.rcWork.bottom - monitor.rcWork.top) / 740.0f});
        }
        if (g_imguiReady) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
            LoadFonts(GetModuleHandleW(nullptr));
        }
        if (g_imguiReady) ui::ApplyTheme(g_scale);
        else ui::SetDpiScale(g_scale);
        const int width = static_cast<int>(1000 * g_scale), height = static_cast<int>(740 * g_scale);
        ApplyWindowRegion(hWnd, width, height);
        SetWindowPos(hWnd, nullptr, suggested->left, suggested->top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NCLBUTTONDBLCLK:
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU || (wParam & 0xfff0) == SC_MAXIMIZE ||
            (wParam & 0xfff0) == SC_SIZE) return 0;
        break;
    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);                       // крестик = свернуть в трей
        return 0;
    case WM_APP_EXIT:
        DestroyWindow(hWnd);
        return 0;
    case WM_APP_SHOW:
        ShowMain();
        return 0;
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) ShowMain();
        else if (lParam == WM_RBUTTONUP) ShowTrayMenu(hWnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_OPEN:
            ShowMain();
            break;
        case ID_TRAY_EXIT:
            DestroyWindow(hWnd);
            break;
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Один экземпляр программы
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"ZapretLauncher_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kWndClass, nullptr);
        if (other) PostMessageW(other, WM_APP_SHOW, 0, 0);
        return 0;
    }

    bool startHidden = wcsstr(GetCommandLineW(), L"--tray") != nullptr;

    ImGui_ImplWin32_EnableDpiAwareness();
    g_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    MONITORINFO monitor{sizeof(MONITORINFO)};
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const bool hasWorkArea = GetMonitorInfoW(primary, &monitor) != FALSE;
    const LONG workWidth = monitor.rcWork.right - monitor.rcWork.left;
    const LONG workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
    if (hasWorkArea) {
        g_scale = std::min({g_scale, workWidth / 1000.0f, workHeight / 740.0f});
    }
    const int width = static_cast<int>(1000 * g_scale);
    const int height = static_cast<int>(740 * g_scale);
    const int x = hasWorkArea ? monitor.rcWork.left + (workWidth - width) / 2 : CW_USEDEFAULT;
    const int y = hasWorkArea ? monitor.rcWork.top + (workHeight - height) / 2 : CW_USEDEFAULT;
    ui::SetDpiScale(g_scale);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LauncherIcon(false);
    wc.hIconSm = LauncherIcon(true);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kWndClass, LAUNCHER_WINDOW_TITLE, WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                             x, y, width, height,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Не удалось создать окно.", L"Zapret Launcher", MB_ICONERROR);
        return 1;
    }
    SetWindowIcons(g_hwnd, GetDpiForWindow(g_hwnd));
    ApplyCustomFrame(g_hwnd, width, height);

    if (!CreateDevice(g_hwnd)) {
        CleanupDevice();
        MessageBoxW(nullptr, L"Не удалось инициализировать DirectX 11.", L"Zapret Launcher", MB_ICONERROR);
        return 1;
    }

    if (!LoadPngTexture(hInst, PNG_ZAPRET_LAUNCHER, 256, g_launcherIcon)) {
        CleanupDevice();
        MessageBoxW(g_hwnd, L"Не удалось загрузить встроенную иконку.", L"Zapret Launcher", MB_ICONERROR);
        DestroyWindow(g_hwnd);
        return 1;
    }
    ui::SetLauncherIcon(reinterpret_cast<uintptr_t>(g_launcherIcon.Get()));
    for (size_t i = 0; i < std::size(g_diagnosticIcons); ++i) {
        if (LoadPngTexture(hInst, kDiagnosticIconResources[i], 128, g_diagnosticIcons[i]))
            ui::SetDiagnosticIcon(i, reinterpret_cast<uintptr_t>(g_diagnosticIcons[i].Get()));
    }
    TrayAdd(g_hwnd);

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    LoadFonts(hInst);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ui::ApplyTheme(g_scale);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);

    try {
        g_app = new AppState();
        g_app->Init();
    } catch (const std::exception& e) {
        MessageBoxW(g_hwnd, util::FromUtf8(e.what()).c_str(), L"Zapret Launcher", MB_ICONERROR);
        DestroyWindow(g_hwnd);
    }
    g_imguiReady = IsWindow(g_hwnd) != FALSE;
    if (g_imguiReady) {
        ShowWindow(g_hwnd, startHidden ? SW_HIDE : SW_SHOWDEFAULT);
        UpdateWindow(g_hwnd);
    }

    bool lastRunning = false;
    if (g_app && IsWindow(g_hwnd)) {
        lastRunning = g_app->svcStatus.state == SERVICE_RUNNING;
        TrayUpdateTip(lastRunning);
    }

    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }

        g_app->Tick();
        bool running = g_app->svcStatus.state == SERVICE_RUNNING;
        if (running != lastRunning) {
            lastRunning = running;
            TrayUpdateTip(running);
        }

        if (!IsWindowVisible(g_hwnd) || IsIconic(g_hwnd)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);   // окно скрыто - почти не грузим CPU
            continue;
        }

        RenderFrame();
    }

    // Деструкторы завершают терминал и фоновые задачи.
    g_imguiReady = false;
    delete g_app;
    g_app = nullptr;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    UnregisterClassW(kWndClass, hInst);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
