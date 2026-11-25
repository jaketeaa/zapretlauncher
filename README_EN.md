<p align="center">
   <a href="README.md">Русский</a> · <strong>English</strong>
</p>

<p align="center">
  <img src="assets/readme/banner.svg" alt="ZAPRET LAUNCHER · Made by JAKETEA" width="100%">
</p>

<h1 align="center">ZAPRET LAUNCHER</h1>

<p align="center">A Windows desktop launcher for managing the Flowseal build of zapret.</p>

<div align="center">

[![Latest release](https://img.shields.io/github/v/release/jaketeaa/zapretlauncher?color=FFB020&style=flat-square)](https://github.com/jaketeaa/zapretlauncher/releases/latest)
[![GitHub downloads](https://img.shields.io/github/downloads/jaketeaa/zapretlauncher/total?color=35353B&style=flat-square)](https://github.com/jaketeaa/zapretlauncher/releases)
[![GitHub stars](https://img.shields.io/github/stars/jaketeaa/zapretlauncher?color=35353B&style=flat-square)](https://github.com/jaketeaa/zapretlauncher/stargazers)
![Windows x64](https://img.shields.io/badge/Windows-x64-35353B?style=flat-square)
![C++17](https://img.shields.io/badge/C%2B%2B-17-35353B?style=flat-square)
![Dear ImGui](https://img.shields.io/badge/Dear%20ImGui-1.91.5-35353B?style=flat-square)
![DirectX 11](https://img.shields.io/badge/DirectX-11-35353B?style=flat-square)

</div>

<p align="center">
  <a href="https://github.com/jaketeaa/zapretlauncher/releases"><img src="https://img.shields.io/badge/Download%20ZAPRET%20LAUNCHER-FFB020?style=for-the-badge" alt="Download ZAPRET LAUNCHER"></a>
</p>

<p align="center">
  <a href="#features">Features</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="https://github.com/jaketeaa/zapretlauncher/issues">Report an issue</a>
</p>

## About

Built by **JAKETEA**, ZAPRET LAUNCHER brings service controls, strategy selection, and logs into one window. It gives Windows users a graphical way to handle tasks they would otherwise perform through BAT files. It is an independent interface for [Flowseal/zapret-discord-youtube](https://github.com/Flowseal/zapret-discord-youtube). Strategy results depend on your network and provider.

## Features

- **zapret service.** Install, remove, start, or stop the Windows service and switch between strategies from `general*.bat`.
- **Network tools.** Game Filter, IPSet Filter modes, IPSet List and Hosts File updates, and a domain list editor.
- **Diagnostics.** Connectivity checks for several services, an interactive `service.bat` terminal, and a launcher log you can copy.
- **Installation and updates.** Choose a working folder, download the Flowseal build from GitHub, check for newer versions, and remove the zapret installation.
- **Interface.** Obsidian & Amber styling, Liquid Glass effects, animations, a system tray icon, and an option to start with Windows.

## Quick start

1. Open [Releases](https://github.com/jaketeaa/zapretlauncher/releases). Download `ZapretLauncher.exe` from **Assets** if it is attached to the release. Otherwise, [build from source](#build-from-source).
2. Run the launcher and approve the Windows administrator prompt. Administrator privileges are required to manage the Windows service.
3. Use **Settings → zapret file location** to select a working folder if needed. You can also point the launcher at an existing extracted Flowseal build.
4. If the components are missing, select **Download components** in Settings.
5. On the **Home** tab, select a strategy and press the power button or **Install service**. The same tab shows the service status.

The current app uses Russian interface labels. **Главная** means Home and **Настройки** means Settings.

**Requirements:** Windows 10 or 11, an x64 build, and DirectX 11. Downloading and updating components requires GitHub access. The EXE embeds the fonts and icon and links the C++ runtime statically.

## Technologies

| Component | Role |
| --- | --- |
| C++17 | Application code |
| Dear ImGui | User interface |
| DirectX 11 | Rendering and glass effects |
| Win32 API | Window, system tray, and services |
| WinHTTP | Network requests |
| nlohmann/json | Configuration storage |
| CMake | Build and dependency setup |

## Build from source

Install Git, Visual Studio 2022 or later or the corresponding Build Tools with **Desktop development with C++**, MSVC, and a Windows SDK. Use CMake 3.24 or later with support for your Visual Studio generator.

```powershell
git clone https://github.com/jaketeaa/zapretlauncher.git
cd zapretlauncher
cmake -S . -B build -A x64
cmake --build build --config Release
```

The output is `build/Release/ZapretLauncher.exe`. CMake downloads Dear ImGui and nlohmann/json during configuration. The bundled `build.bat` uses the Visual Studio 2026 generator.

## FAQ

**Where are the zapret files stored?** Settings shows the working folder and lets you change it or open it in File Explorer.

**How do I change strategies or update components?** Use **Home → Change strategy** for strategies and **Settings → Flowseal build** for updates.

**What if GitHub downloads fail?** Check your access to GitHub and read the log message. If you already have an extracted Flowseal build, select its folder in Settings.

**What do the two removal options do?** **Remove service** removes the Windows service. **Remove zapret** is for removing the working installation.

**Can I use a VPN alongside the launcher?** Compatibility depends on the VPN, routing, and selected strategy. If you run into problems, compare the diagnostic results with the VPN on and off.

## Feedback

Open [GitHub Issues](https://github.com/jaketeaa/zapretlauncher/issues) to report a bug or suggest a feature. For a bug, include the launcher and Windows versions, the expected result, and steps to reproduce it. A log excerpt or screenshot can help. Remove personal information, local paths, and keys before posting.

## Credits

**Designed & Developed by JAKETEA**

- [bol-van/zapret](https://github.com/bol-van/zapret) — original zapret project.
- [Flowseal/zapret-discord-youtube](https://github.com/Flowseal/zapret-discord-youtube) — Windows build.
- [Dear ImGui](https://github.com/ocornut/imgui) and [nlohmann/json](https://github.com/nlohmann/json) — application libraries.
- Bundled font license notices: [Manrope](assets/fonts/Manrope-OFL.txt) and [Roboto Mono](assets/fonts/RobotoMono-OFL.txt).
