@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo === Настройка проекта (скачивание ImGui и nlohmann/json) ===
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
if errorlevel 1 goto fail
echo === Сборка ===
cmake --build build --config Release
if errorlevel 1 goto fail
echo.
echo ГОТОВО: build\Release\ZapretLauncher.exe
explorer build\Release
pause
exit /b 0
:fail
echo.
echo ОШИБКА СБОРКИ. Скопируйте текст ошибки выше и отправьте мне.
pause
exit /b 1
