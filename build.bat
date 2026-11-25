@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo === Настройка проекта (скачивание ImGui и nlohmann/json) ===
cmake -S . -B build/Beta -G "Visual Studio 18 2026" -A x64
if errorlevel 1 goto fail
echo === Сборка ===
cmake --build build/Beta --config Release
if errorlevel 1 goto fail
echo.
echo ГОТОВО: build\Beta\Release
explorer build\Beta\Release
pause
exit /b 0
:fail
echo.
echo ОШИБКА СБОРКИ. Скопируйте текст ошибки выше и отправьте мне.
pause
exit /b 1
