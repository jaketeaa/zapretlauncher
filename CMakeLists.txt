cmake_minimum_required(VERSION 3.21)
project(ZapretLauncher LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "Проект собирается только компилятором MSVC (Visual Studio / Build Tools).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Статическая линковка рантайма (/MT) -> один автономный exe
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Защита от ошибки "совместимость с CMake < 3.5" в новых версиях CMake
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------- Автоматическая загрузка зависимостей ----------
include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json imgui)

# ---------- Dear ImGui (Win32 + DirectX 11) ----------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE NOMINMAX)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# ---------- Само приложение ----------
configure_file(src/fonts.rc.in fonts.rc @ONLY)
add_executable(ZapretLauncher WIN32
    ${CMAKE_CURRENT_BINARY_DIR}/fonts.rc
    src/glass_renderer.cpp
    src/installation.cpp
    src/main.cpp
    src/ui.cpp
    src/utils.cpp
    src/http_client.cpp
    src/config_manager.cpp
    src/launcher_log.cpp
    src/diagnostics.cpp
    src/github_updater.cpp
    src/service_control.cpp
    src/console_session.cpp
    src/service_manager.cpp
    app.manifest
)

target_include_directories(ZapretLauncher PRIVATE src)
target_compile_definitions(ZapretLauncher PRIVATE
    UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS
    WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_compile_options(ZapretLauncher PRIVATE /W3 /utf-8 /EHsc /permissive-)
target_link_libraries(ZapretLauncher PRIVATE
    imgui
    nlohmann_json::nlohmann_json
    winhttp shell32 advapi32 user32 ole32 dwmapi uuid gdi32 windowscodecs
)

target_link_options(ZapretLauncher PRIVATE /MANIFESTUAC:NO)
