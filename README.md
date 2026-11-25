<p align="center">
   <strong>Русский</strong> · <a href="README_EN.md">English</a>
</p>

<p align="center">
  <img src="assets/readme/banner.svg" alt="ZAPRET LAUNCHER · Made by JAKETEA" width="100%">
</p>

<h1 align="center">ZAPRET LAUNCHER</h1>

<p align="center">Графический лаунчер для управления сборкой zapret от Flowseal на Windows.</p>

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
  <a href="https://github.com/jaketeaa/zapretlauncher/releases"><img src="https://img.shields.io/badge/Скачать%20ZAPRET%20LAUNCHER-FFB020?style=for-the-badge" alt="Скачать ZAPRET LAUNCHER"></a>
</p>

<p align="center">
  <a href="#возможности">Возможности</a> ·
  <a href="#быстрый-старт">Быстрый старт</a> ·
  <a href="https://github.com/jaketeaa/zapretlauncher/issues">Сообщить об ошибке</a>
</p>

## О проекте

Лаунчер от **JAKETEA** объединяет управление службой, выбор стратегии и просмотр журналов в одном окне. Он подойдёт пользователям Windows, которым удобнее выполнять эти действия через графический интерфейс, чем запускать BAT-файлы вручную. Это самостоятельный интерфейс для сборки [Flowseal/zapret-discord-youtube](https://github.com/Flowseal/zapret-discord-youtube). Результат работы стратегии зависит от сети и провайдера.

## Возможности

- **Служба zapret.** Установка и удаление службы Windows, запуск и остановка, смена стратегий из `general*.bat`.
- **Сетевые инструменты.** Game Filter, режимы IPSet Filter, обновление IPSet List и Hosts File, редактор списков доменов.
- **Диагностика.** Проверка связи с несколькими сервисами, интерактивный терминал для `service.bat`, журнал лаунчера с копированием.
- **Установка и обновления.** Выбор рабочей папки, загрузка сборки Flowseal с GitHub, проверка новых версий и удаление установки zapret.
- **Интерфейс.** Obsidian & Amber, эффекты Liquid Glass, анимации, системный трей и автозапуск с Windows.

## Быстрый старт

1. Откройте [Releases](https://github.com/jaketeaa/zapretlauncher/releases). Скачайте `ZapretLauncher.exe` из **Assets**, если файл прикреплён к релизу. Иначе воспользуйтесь [сборкой из исходников](#сборка-из-исходников).
2. Запустите лаунчер и подтвердите запрос Windows на права администратора. Они нужны для управления системной службой.
3. При необходимости откройте **Настройки → Расположение файлов zapret** и выберите папку. Можно указать папку с уже распакованной сборкой Flowseal.
4. Если компонентов ещё нет, нажмите **Скачать компоненты** в настройках.
5. На вкладке **Главная** выберите стратегию и нажмите кнопку питания или **Установить службу**. Состояние службы отображается на той же вкладке.

**Требования:** Windows 10 или 11, сборка x64, DirectX 11. Для скачивания и обновления компонентов нужен доступ к GitHub. Шрифты и иконка встроены в EXE, рантайм C++ слинкован статически.

## Технологии

| Компонент | Назначение |
| --- | --- |
| C++17 | Основной код |
| Dear ImGui | Графический интерфейс |
| DirectX 11 | Отрисовка и стеклянные эффекты |
| Win32 API | Окно, трей и системные службы |
| WinHTTP | Сетевые запросы |
| nlohmann/json | Чтение и запись настроек |
| CMake | Сборка проекта и загрузка зависимостей |

## Сборка из исходников

Установите Git, Visual Studio 2022 или новее либо соответствующие Build Tools с компонентами **Desktop development with C++**, MSVC и Windows SDK. Используйте CMake 3.24 или новее с поддержкой генератора вашей версии Visual Studio.

```powershell
git clone https://github.com/jaketeaa/zapretlauncher.git
cd zapretlauncher
cmake -S . -B build -A x64
cmake --build build --config Release
```

Готовый файл появится в `build/Release/ZapretLauncher.exe`. CMake скачает Dear ImGui и nlohmann/json при настройке проекта. Файл `build.bat` рассчитан на генератор Visual Studio 2026.

## Частые вопросы

**Где лежат файлы zapret?** Рабочую папку можно посмотреть или изменить в настройках. Лаунчер также может открыть её в Проводнике.

**Как сменить стратегию или обновить компоненты?** Для стратегии используйте **Главная → Сменить стратегию**. Для обновлений откройте **Настройки → Сборка Flowseal**.

**Что делать, если загрузка с GitHub не проходит?** Проверьте доступ к GitHub и сообщение в журнале. Если у вас уже есть распакованная сборка Flowseal, укажите её папку в настройках.

**Что удаляют две разные кнопки?** **Удалить сервис** удаляет службу Windows. **Удалить zapret** предназначена для удаления рабочей установки.

**Можно ли использовать лаунчер вместе с VPN?** Совместимость зависит от VPN, маршрутизации и выбранной стратегии. При проблемах сравните диагностику с включённым и выключенным VPN.

## Обратная связь

Откройте [GitHub Issues](https://github.com/jaketeaa/zapretlauncher/issues), чтобы сообщить об ошибке или предложить функцию. Для ошибки укажите версию лаунчера и Windows, опишите ожидаемый результат и шаги воспроизведения. Приложите журнал или скриншот, если они помогают понять проблему. Перед публикацией уберите личные данные, локальные пути и ключи.

## Авторство

**Designed & Developed by JAKETEA**

- [bol-van/zapret](https://github.com/bol-van/zapret) — оригинальный проект zapret.
- [Flowseal/zapret-discord-youtube](https://github.com/Flowseal/zapret-discord-youtube) — сборка для Windows.
- [Dear ImGui](https://github.com/ocornut/imgui) и [nlohmann/json](https://github.com/nlohmann/json) — библиотеки проекта.
- Уведомления о лицензиях встроенных шрифтов: [Manrope](assets/fonts/Manrope-OFL.txt) и [Roboto Mono](assets/fonts/RobotoMono-OFL.txt).
