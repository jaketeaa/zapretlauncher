#include "ui_widgets.h"
#include "app_version.h"
#include "utils.h"
#include <imgui_stdlib.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui_detail {
namespace {
bool ActiveStrategy(const AppState& app, const std::string& name) {
    return app.OwnService() && !app.svcStatus.strategy.empty() &&
        util::ToLowerAscii(name) == util::ToLowerAscii(app.svcStatus.strategy + ".bat");
}

void Star(const char* id, bool favorite, bool& clicked) {
    const float size = S(26);
    clicked = ImGui::InvisibleButton(id, {size, size}, ImGuiButtonFlags_EnableNav);
    const auto motion = Animate(ImGui::GetItemID(), favorite ? 1.f : 0.f, favorite ? 1.f : 0.f, .18f);
    auto* draw = ImGui::GetWindowDrawList();
    const auto p = ImGui::GetItemRectMin();
    const ImVec2 center(p.x + size * .5f, p.y + size * .5f);
    ImVec2 points[10];
    for (int i = 0; i < 10; ++i) {
        const float theta = -1.5707963f + i * .62831853f;
        const float radius = S(i % 2 ? 3.5f : 8.3f);
        points[i] = {center.x + radius * std::cos(theta), center.y + radius * std::sin(theta)};
    }
    draw->AddConcavePolyFilled(points, 10, Ink(WithAlpha(kAccentL, motion * .9f)));
    draw->AddPolyline(points, 10, Ink(Mix(kMuted, kAccentL, motion)), ImDrawFlags_Closed, S(1.3f));
    if (ImGui::IsItemFocused()) draw->AddRect(p, ImGui::GetItemRectMax(), Ink(kAccentL), S(6));
    Tooltip(favorite ? "Убрать из избранного" : "Добавить в избранное");
}

ImVec4 KindColor(beta::Kind kind) {
    switch (kind) {
        case beta::Kind::Success: return kGreen;
        case beta::Kind::Warning: return kWarn;
        case beta::Kind::Error: return kRed;
        default: return kMuted;
    }
}

void EventGlyph(beta::Kind kind) {
    const auto p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 center(p.x + S(8), p.y + h * .5f);
    auto* list = ImGui::GetWindowDrawList();
    const auto ink = Ink(KindColor(kind));
    list->AddCircle(center, S(6.5f), ink, 20, S(1.2f));
    if (kind == beta::Kind::Success) {
        list->AddLine({center.x - S(3), center.y}, {center.x - S(.5f), center.y + S(2.5f)}, ink, S(1.3f));
        list->AddLine({center.x - S(.5f), center.y + S(2.5f)}, {center.x + S(3.5f), center.y - S(2.5f)}, ink, S(1.3f));
    } else {
        const float sign = kind == beta::Kind::Info ? -1.f : 1.f;
        list->AddLine({center.x, center.y - S(3) * sign}, {center.x, center.y + S(.6f) * sign}, ink, S(1.3f));
        list->AddCircleFilled({center.x, center.y + S(3) * sign}, S(.8f), ink);
    }
    ImGui::Dummy({S(16), h});
    ImGui::SameLine(0, S(8));
}

void OpenUrl(HWND owner, const std::wstring& url) {
    if (!url.empty()) ShellExecuteW(owner, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CopyGlyph(bool save) {
    const auto p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
    const ImVec2 c(p.x + S(16), (p.y + q.y) * .5f);
    auto* dl = ImGui::GetWindowDrawList();
    const auto color = Ink(kMuted);
    const float d = save ? -1.f : 1.f;
    dl->AddLine({c.x, c.y - S(5) * d}, {c.x, c.y + S(4) * d}, color, S(1.3f));
    dl->AddLine({c.x - S(3), c.y + S(1) * d}, {c.x, c.y + S(4) * d}, color, S(1.3f));
    dl->AddLine({c.x + S(3), c.y + S(1) * d}, {c.x, c.y + S(4) * d}, color, S(1.3f));
    dl->AddLine({c.x - S(5), c.y + S(7)}, {c.x + S(5), c.y + S(7)}, color, S(1.2f));
}

void FillSegment(ImDrawList* draw, ImVec2 a, ImVec2 b, float base) {
    const auto uv = ImGui::GetFontTexUvWhitePixel();
    const auto top = Ink(WithAlpha(kAccent, .21f));
    const auto bottom = Ink(WithAlpha(kAccent, .015f));
    draw->PrimReserve(6, 4);
    const auto first = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
    for (const auto off : {0, 1, 2, 0, 2, 3}) draw->PrimWriteIdx(static_cast<ImDrawIdx>(first + off));
    draw->PrimWriteVtx(a, uv, top);
    draw->PrimWriteVtx(b, uv, top);
    draw->PrimWriteVtx({b.x, base}, uv, bottom);
    draw->PrimWriteVtx({a.x, base}, uv, bottom);
}

void DrawPlot(const std::deque<beta::Sample>& series, double maxValue) {
    const auto pos = ImGui::GetCursorScreenPos();
    const ImVec2 outer(ImGui::GetContentRegionAvail().x, S(176));
    const ImVec2 end(pos.x + outer.x, pos.y + outer.y);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, end, Ink(C(13, 13, 15)), S(12));
    draw->AddRect(pos, end, Ink(kDivider), S(12));
    const ImVec2 a(pos.x + S(58), pos.y + S(15)), b(end.x - S(16), end.y - S(28));
    ImGui::InvisibleButton("##latency_plot", outer);
    if (series.empty()) {
        const char* empty = "История появится после первой проверки.";
        const auto text = ImGui::CalcTextSize(empty);
        draw->AddText({pos.x + (outer.x - text.x) * .5f, pos.y + (outer.y - text.y) * .5f}, Ink(kMuted), empty);
        return;
    }
    const float goal = static_cast<float>(std::max(100.0, std::ceil(maxValue / 50.0) * 50.0));
    const float limit = std::max(1.f, Animate(ImGui::GetID("##latency_scale"), goal, goal, .22f));
    for (int i = 0; i <= 4; ++i) {
        const float y = b.y - (b.y - a.y) * (i / 4.f);
        const auto label = std::to_string(static_cast<long long>(std::llround(limit * i / 4.f)));
        draw->AddText({a.x - S(8) - ImGui::CalcTextSize(label.c_str()).x, y - ImGui::GetFontSize() * .5f}, Ink(kMuted), label.c_str());
        draw->AddLine({a.x, y}, {b.x, y}, Ink(kDivider), S(1));
    }
    const auto total = std::chrono::duration<double>(series.back().time - series.front().time).count();
    const auto xAt = [&](const beta::Sample& sample) {
        const auto elapsed = std::chrono::duration<double>(sample.time - series.front().time).count();
        const float t = total > 0 ? static_cast<float>(elapsed / total) : .5f;
        return a.x + (b.x - a.x) * t;
    };
    const auto point = [&](const beta::Sample& sample) {
        return ImVec2(xAt(sample), b.y - (b.y - a.y) * static_cast<float>(*sample.ms) / limit);
    };
    draw->PushClipRect({a.x - S(3), a.y - S(4)}, {b.x + S(3), b.y + S(4)}, true);
    for (std::size_t i = 1; i < series.size(); ++i) {
        const auto& last = series[i - 1];
        const auto& next = series[i];
        if (!last.ms || !next.ms) continue;
        FillSegment(draw, point(last), point(next), b.y);
    }
    for (std::size_t i = 0; i < series.size(); ++i) {
        if (!series[i].ms) continue;
        if (i && series[i - 1].ms) draw->AddLine(point(series[i - 1]), point(series[i]), Ink(kAccentL), S(2));
        else draw->AddCircleFilled(point(series[i]), S(2.4f), Ink(kAccentL));
    }
    if (ImGui::IsItemHovered()) {
        const float mouse = ImGui::GetMousePos().x;
        auto nearest = std::min_element(series.begin(), series.end(), [&](const auto& x, const auto& y) {
            return std::abs(xAt(x) - mouse) < std::abs(xAt(y) - mouse);
        });
        const auto x = xAt(*nearest);
        draw->AddLine({x, a.y}, {x, b.y}, Ink(WithAlpha(kMuted, .6f)));
        if (nearest->ms) draw->AddCircleFilled(point(*nearest), S(4), Ink(kAccentL));
        const auto detail = beta::LocalTime(nearest->date) + "  ·  " +
            (nearest->ms ? std::to_string(*nearest->ms) + " ms" : "Нет HTTPS-ответа");
        Tooltip(detail.c_str());
    }
    draw->PopClipRect();
    for (int i = 0; i <= (series.size() == 1 ? 0 : 2); ++i) {
        const float t = series.size() == 1 ? .5f : i / 2.f;
        const auto at = series.front().date + std::chrono::duration_cast<beta::WallClock::duration>(
            std::chrono::duration<double>(total * t));
        const auto label = beta::LocalTime(at);
        const float x = a.x + (b.x - a.x) * t - ImGui::CalcTextSize(label.c_str()).x * .5f;
        draw->AddText({std::clamp(x, pos.x + S(8), end.x - ImGui::CalcTextSize(label.c_str()).x - S(8)), b.y + S(6)},
                      Ink(kMuted), label.c_str());
    }
}
}

void DrawStrategyPicker(AppState& app) {
    static std::string search;
    static int filter = 0;
    auto& chosen = app.config.settings.selectedBat;
    ImGui::SetNextItemWidth(-1);
    ImGui::SetNextWindowSizeConstraints({S(400), 0}, {S(600), S(385)});
    if (!ImGui::BeginCombo("##bat", chosen.empty() ? "Нет файлов general*.bat" : chosen.c_str())) return;

    static const char* filters[]{"Все", "Избранные", "Недавние"};
    if (const int next = Segments("##strategy_filter", filters, 3, filter, {ImGui::GetContentRegionAvail().x, S(32)}); next >= 0)
        filter = next;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter_search", "Поиск стратегии", &search);
    const auto query = util::ToLowerAscii(search);
    ImGui::BeginChild("##strategy_choices", {0, S(220)});
    bool any = false;
    std::vector<std::string> names;
    if (filter == 2) {
        for (const auto& name : app.config.settings.recentStrategies)
            if (std::find(app.strategies.begin(), app.strategies.end(), name) != app.strategies.end())
                names.push_back(name);
    } else {
        for (const auto& name : app.strategies) if (app.Favorite(name)) names.push_back(name);
        if (filter == 0) for (const auto& name : app.strategies) if (!app.Favorite(name)) names.push_back(name);
    }
    for (const auto& name : names) {
        if (!query.empty() && util::ToLowerAscii(name).find(query) == std::string::npos) continue;
        any = true;
        ImGui::PushID(name.c_str());
        bool clicked = false;
        Star("##favorite", app.Favorite(name), clicked);
        if (clicked) app.ToggleFavorite(name);
        ImGui::SameLine(0, S(7));
        const bool active = ActiveStrategy(app, name);
        const auto text = active ? name + "  ·  активна" : name;
        if (active) ImGui::PushStyleColor(ImGuiCol_Text, kAccentL);
        const bool pick = ImGui::Selectable((text + "###pick").c_str(), chosen == name,
                                            ImGuiSelectableFlags_NoAutoClosePopups, {0, S(26)});
        if (active) ImGui::PopStyleColor();
        ImGui::PopID();
        if (pick) {
            app.PickStrategy(name);
            ImGui::EndChild();
            ImGui::CloseCurrentPopup();
            ImGui::EndCombo();
            return;
        }
    }
    if (!any) ImGui::TextDisabled("Нет подходящих стратегий.");
    ImGui::EndChild();
    ImGui::EndCombo();
}

void DrawUptime(AppState& app, float width) {
    const float live = Animate(ImGui::GetID("##uptime_reveal"), app.uptime.Verified() ? 1.f : 0.f, 0.f, .28f);
    const auto& value = app.uptimeText;
    if (g_fontUptime) ImGui::PushFont(g_fontUptime);
    const auto origin = ImGui::GetCursorScreenPos();
    const auto text = ImGui::CalcTextSize(value.c_str());
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddText(
        {std::round(origin.x + (contentWidth - text.x) * .5f), std::round(origin.y)},
        Ink(Mix(kMuted, kAccentL, live)), value.c_str());
    ImGui::Dummy({contentWidth, ImGui::GetFontSize()});
    if (g_fontUptime) ImGui::PopFont();
    const char* caption = "Время работы службы";
    ImGui::SetCursorPosX(std::max(S(16), (width - ImGui::CalcTextSize(caption).x) * .5f));
    ImGui::TextColored(kMuted, "%s", caption);
}

void DrawMonitoringControls(AppState& app) {
    auto& cfg = app.config.settings;
    ImGui::SameLine();
    if (Button(cfg.monitorOn ? "Остановить мониторинг" : "Запустить мониторинг",
               {S(208), S(36)}, cfg.monitorOn ? Action::Neutral : Action::Quiet)) {
        cfg.monitorOn = !cfg.monitorOn;
        if (cfg.monitorOn) app.nextDiagnosis = beta::Clock::now();
        app.SaveConfig();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(88));
    const auto label = std::to_string(cfg.monitorSeconds) + " с";
    if (ImGui::BeginCombo("##interval", label.c_str())) {
        for (int interval : {5, 10, 30, 60}) {
            const auto item = std::to_string(interval) + " с";
            if (ImGui::Selectable(item.c_str(), interval == cfg.monitorSeconds)) {
                cfg.monitorSeconds = interval;
                app.nextDiagnosis = beta::Clock::now() + std::chrono::seconds(interval);
                app.SaveConfig();
            }
        }
        ImGui::EndCombo();
    }
    Tooltip("Интервал между началами проверок. Следующий запрос ждёт завершения предыдущего.");
    ImGui::SameLine();
    if (Button("Очистить историю", {S(161), S(36)})) app.history.Clear();
}

void DrawConnectionHistory(AppState& app) {
    BeginCard("##stability_card", {0, S(360)});
    Section("Стабильность подключения");
    static int selected = 0;
    const char* names[std::size(diagnostics::kServices)]{};
    for (std::size_t i = 0; i < std::size(names); ++i) names[i] = diagnostics::kServices[i].name;
    if (const auto choice = Segments("##history_service", names, static_cast<int>(std::size(names)),
                                     selected, {std::min(S(440), ImGui::GetContentRegionAvail().x), S(34)}); choice >= 0)
        selected = choice;
    if (g_diagnosticIcons[selected]) {
        ImGui::Image(g_diagnosticIcons[selected], {S(24), S(24)});
        ImGui::SameLine();
    }
    const auto& series = app.history[selected];
    const auto index = static_cast<std::size_t>(selected);
    const auto& network = app.diagnosticReport.network;
    const auto latest = index < network.size() ? network[index].latencyMs : std::nullopt;
    const char* status = index >= network.size() ? "Не проверен" : latest ? "Доступен" : "Недоступен";
    const auto current = std::string(status) + "  ·  " + (latest ? std::to_string(*latest) + " ms" : "—");
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(latest ? kGreen : index >= network.size() ? kMuted : kRed, "%s", current.c_str());
    Tooltip("Время HTTPS-запроса до заголовков ответа. Включает подключение и ожидание ответа, это не ICMP RTT.");

    std::size_t successes = 0;
    long long minimum = 0, maximum = 0, sum = 0;
    for (const auto& sample : series) {
        if (!sample.ms) continue;
        if (!successes) minimum = *sample.ms;
        minimum = std::min(minimum, *sample.ms);
        maximum = std::max(maximum, *sample.ms);
        sum += *sample.ms;
        ++successes;
    }
    if (successes) {
        char stats[168]{};
        std::snprintf(stats, sizeof(stats), "Мин. %lld ms   Сред. %.1f ms   Макс. %lld ms   Успешно %.0f%%",
                      minimum, static_cast<double>(sum) / successes, maximum, 100.0 * successes / series.size());
        WrappedColored(kMuted, stats);

    } else {
        ImGui::TextColored(kMuted, "%s", series.empty() ? "Мин. —     Сред. —     Макс. —     Успешно —" :
                           "Мин. —     Сред. —     Макс. —     Успешно 0%");
    }
    ImGui::PushID(selected);
    DrawPlot(series, static_cast<double>(maximum));
    ImGui::PopID();
    ImGui::EndChild();
}

void DrawLauncherUpdates(AppState& app, HWND owner) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(16)});
    ImGui::BeginChild("##launcher_updates", {}, ImGuiChildFlags_AlwaysUseWindowPadding |
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    Section("Обновления лаунчера");
    Label("ZAPRET LAUNCHER  ·  " LAUNCHER_DISPLAY_VERSION);
    const bool busy = app.launcherJob.valid();
    auto& cfg = app.config.settings;
    const bool onBeta = cfg.updateChannel == "beta";
    const char* channels[]{"Стабильный", "Бета"};
    ImGui::BeginDisabled(busy);
    if (const auto change = Segments("##updates_channel", channels, 2, onBeta ? 1 : 0, {S(248), S(34)}); change >= 0) {
        const std::string channel = change == 1 ? "beta" : "stable";
        if (channel != cfg.updateChannel) {
            cfg.updateChannel = channel;
            app.launcherRelease = {};
            app.lastLauncherCheck = {};
            app.SaveConfig();
        }
    }
    ImGui::SameLine();
    if (Button("Проверить обновления##launcher", {S(187), S(34)}, Action::Primary)) app.CheckLauncher();
    ImGui::EndDisabled();
    if (cfg.updateChannel.empty()) WrappedColored(kMuted, "Для экспериментальных релизов выберите канал «Бета».");
    if (Toggle("Проверять обновления лаунчера при запуске", cfg.checkLauncherOnStart)) {
        cfg.checkLauncherOnStart = !cfg.checkLauncherOnStart;
        app.SaveConfig();
    }
    const auto checked = app.lastLauncherCheck.time_since_epoch().count() != 0;
    const auto& release = app.launcherRelease;
    const auto time = checked ? beta::LocalTime(app.lastLauncherCheck, "%d.%m.%Y %H:%M:%S") : "—";
    ImGui::TextColored(kMuted, "Последняя проверка  ·  %s", time.c_str());
    ImGui::TextColored(kMuted, "Последняя версия  ·  %s", busy ? "Проверка…" : release.tag.empty() ? "—" : release.tag.c_str());
    if (!busy && checked && !release.error.empty()) WrappedColored(kRed, release.error);
    else if (!busy && checked && release.tag.empty()) Label("В выбранном канале пока нет опубликованных релизов.");
    else if (!busy && checked && !release.newer) Label("Более новой версии в выбранном канале нет.");
    if (!busy && release.newer && release.tag != app.snoozedTag) {
        Section("Доступно обновление");
        ImGui::TextColored(kAccentL, "%s → %s", LAUNCHER_DISPLAY_VERSION, release.tag.c_str());
        Label("Что нового");
        BeginCard("##release_notes", {0, S(115)}, ImGuiWindowFlags_None);
        WrappedColored(kText, release.notes.empty() ? "Описание релиза отсутствует." : release.notes);
        ImGui::EndChild();
        if (release.asset.empty()) {
            WrappedColored(kWarn, "В этом релизе нет подходящего EXE.");
        } else {
            if (Button("Скачать EXE в браузере")) OpenUrl(owner, release.asset);
            ImGui::SameLine();
        }
        if (Button("Открыть страницу релиза")) OpenUrl(owner, release.page);
        ImGui::SameLine();
                if (Button("Напомнить позже")) {
            app.snoozedTag = release.tag;
            app.remindAfter = beta::Clock::now() + std::chrono::minutes(30);
        }
        Label("Установка выполняется вручную.");
    }
    ImGui::EndChild();
}

void DrawBackupTools(AppState& app, HWND owner) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(16), S(16)});
    ImGui::BeginChild("##backup_tools", {}, ImGuiChildFlags_AlwaysUseWindowPadding |
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    Section("Резервное копирование");
    Label("Сохраните настройки, избранные стратегии и пользовательские списки доменов.");
    const bool busy = app.backupJob.valid() || app.listDirty || app.Deleting() || app.updater.Busy() || app.svcMgr.Busy();
    ImGui::BeginDisabled(busy);
    if (Button("Экспорт настроек", {S(195), S(34)}, Action::Primary)) app.ChooseBackup(owner, true);
    CopyGlyph(true);
    ImGui::SameLine();
    if (Button("Импорт настроек", {S(195), S(34)})) app.ChooseBackup(owner, false);
    CopyGlyph(false);
    ImGui::EndDisabled();
    if (app.listDirty) Label("Сохраните изменения списка доменов перед копированием.");
    if (app.backupJob.valid()) Label("Обработка резервной копии…");
    if (app.backupPreview && !ImGui::IsPopupOpen("Восстановление настроек"))
        ImGui::OpenPopup("Восстановление настроек");
    ImGui::SetNextWindowSize({S(545), 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Восстановление настроек", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (app.backupPreview) {
            const auto& incoming = *app.backupPreview;
            ImGui::Text("Дата: %s  ·  Версия: %s", incoming.createdAt.c_str(), incoming.version.c_str());
            ImGui::Text("Списки: %zu  ·  Избранные стратегии: %zu", incoming.lists.size(), incoming.settings.favoriteStrategies.size());
            WrappedColored(kMuted, "Будут восстановлены выбранная стратегия, избранное, недавние стратегии, настройки трея, "
                                  "мониторинга и обновлений. Пользовательские списки с совпадающими именами будут заменены.");
            WrappedColored(kMuted, "Перед записью будет создана локальная копия. Автозапуск и сетевые фильтры можно применить отдельно.");
            if (Button("Отмена")) {
                app.backupPreview.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(busy || app.term.IsRunning() || app.autostartJob.valid() || app.launcherJob.valid());
            if (Button("Восстановить", {}, Action::Primary)) {
                app.RestoreBackup();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
        } else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    auto& cfg = app.config.settings;
    if (cfg.filtersPending) {
        ImGui::TextColored(kAccentL, "Параметры из копии  ·  Game Filter: %s  ·  IPSet: %s", cfg.gameFilter.c_str(), cfg.ipsetFilter.c_str());
        const bool canApply = !busy && !app.term.IsRunning() && app.svcStatus.serviceBatPresent &&
            (!app.svcStatus.installed || (app.OwnService() && app.CanControl()));
        ImGui::BeginDisabled(!canApply || cfg.gameFilter == app.svcStatus.gameMode);
        if (Button("Применить Game Filter")) app.svcMgr.SetGameFilter(cfg.gameFilter);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!canApply || cfg.ipsetFilter == app.svcStatus.ipsetMode || app.svcMgr.Busy());
        if (Button("Применить IPSet")) app.svcMgr.SetIpsetMode(cfg.ipsetFilter);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("Скрыть параметры")) { cfg.filtersPending = false; app.SaveConfig(); }
        if (cfg.backupAutostart != app.runWithWindows)
            Label(cfg.backupAutostart ? "Автозапуск в копии включён. Измените его в карточке «Запуск и выход»." :
                                      "Автозапуск в копии выключен. Измените его в карточке «Запуск и выход».");
    }
    ImGui::EndChild();
}

void DrawBetaNote() {
    BeginCard("##beta_note", {0, S(90)});
    ImGui::TextColored(kAccentL, "%s", "BETA");
    ImGui::SameLine();
    Section("Вы используете экспериментальную бета-версию");
    WrappedColored(kMuted, "Новые функции могут содержать ошибки. Спасибо за участие в тестировании.");
    ImGui::EndChild();
}

void DrawNotificationBell(AppState& app, const ImVec2& p) {
    ImGui::SetCursorScreenPos(p);
    const bool click = ImGui::InvisibleButton("##notifications", {S(42), S(36)}, ImGuiButtonFlags_EnableNav);
    const auto count = app.notifications.Unread();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + S(20), p.y + S(17));
    Glass(draw, p, {p.x + S(42), p.y + S(36)}, S(11), WithAlpha(kCard, .8f), ImGui::IsItemHovered() ? 1.f : 0.f);
    const auto ink = Ink(kText);
    draw->PathArcTo(c, S(6), 3.14159f, 6.28318f, 16);
    draw->PathLineTo({c.x + S(6), c.y + S(3)});
    draw->PathLineTo({c.x + S(8), c.y + S(5)});
    draw->PathLineTo({c.x - S(8), c.y + S(5)});
    draw->PathLineTo({c.x - S(6), c.y + S(3)});
    draw->PathStroke(ink, ImDrawFlags_Closed, S(1.35f));
    draw->AddCircleFilled({c.x, c.y + S(8)}, S(1.5f), ink);
    if (count) {
        const auto number = count > 99 ? std::string("99+") : std::to_string(count);
        if (g_fontCaption) ImGui::PushFont(g_fontCaption);
        const auto text = ImGui::CalcTextSize(number.c_str());
        const ImVec2 at(p.x + S(36) - text.x * .5f, p.y - S(4));
        draw->AddRectFilled({at.x - S(3), at.y}, {at.x + text.x + S(3), at.y + text.y},
                            Ink(kAccentL), S(7));
        draw->AddText(at, Ink(C(29, 22, 12)), number.c_str());
        if (g_fontCaption) ImGui::PopFont();
    }
    Tooltip("Центр уведомлений");
    if (click) ImGui::OpenPopup("##notification_panel");
    ImGui::SetNextWindowPos({p.x + S(42), p.y + S(41)}, ImGuiCond_Appearing, {1, 0});
    ImGui::SetNextWindowSize({S(405), S(375)}, ImGuiCond_Always);
    if (!ImGui::BeginPopup("##notification_panel")) return;
    Section("Уведомления");
    if (Button("Отметить все прочитанными")) app.notifications.MarkAll();
    ImGui::SameLine();
    if (Button("Очистить")) app.notifications.Clear();
    ImGui::BeginChild("##events", {0, 0});
    if (app.notifications.events.empty()) Label("Событий пока нет.");
    for (auto it = app.notifications.events.begin(); it != app.notifications.events.end();) {
        auto& event = *it;
        ImGui::PushID(static_cast<int>(event.id));
        EventGlyph(event.kind);
        ImGui::TextColored(event.unread ? kAccentL : kText, "%s", event.title.c_str());
        ImGui::SameLine();
        if (Button("×##event_close", {S(29), S(24)}, Action::Quiet)) {
            ImGui::PopID();
            it = app.notifications.events.erase(it);
            continue;
        }
        WrappedColored(kMuted, event.message);
        Label(beta::LocalTime(event.date).c_str());
        if (event.unread) {
            ImGui::SameLine();
            if (Button("Прочитано", {S(101), S(25)}, Action::Quiet)) event.unread = false;
        }
        ImGui::Separator();
        ImGui::PopID();
        ++it;
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void DrawToasts(AppState& app, float footerHeight) {
    const auto now = beta::Clock::now();
    const auto* viewport = ImGui::GetMainViewport();
    int slot = 0;
    for (auto& event : app.notifications.events) {
        if (!event.toast) continue;
        const float age = std::chrono::duration<float>(now - event.time).count();
        const float lifetime = event.kind == beta::Kind::Error ? 8.f : 5.f;
        if (age >= lifetime) continue;
        if (slot == 3) break;
        const float opacity = std::clamp(std::min(age, lifetime - age) / .3f, 0.f, 1.f);
        ++slot;
        const std::string id = "##toast_" + std::to_string(event.id);
        const float vertical = Animate(ImGui::GetID(id.c_str()), static_cast<float>(slot), static_cast<float>(slot), .2f);
        ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x - S(22 - (1 - opacity) * 20),
                                 viewport->Pos.y + viewport->Size.y - footerHeight - S(22 + (vertical - 1) * 102)},
                                ImGuiCond_Always, {1, 1});
        ImGui::SetNextWindowSize({S(370), S(93)}, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);
        constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin(id.c_str(), nullptr, flags)) {
            EventGlyph(event.kind);
            ImGui::TextColored(KindColor(event.kind), "%s", event.title.c_str());
            ImGui::SameLine(S(316));
            if (Button("×##toast_close", {S(28), S(23)}, Action::Quiet)) event.toast = false;
            std::string text = event.message;
            if (text.size() > 140) {
                std::size_t size = 140;
                while ((static_cast<unsigned char>(text[size]) & 0xC0) == 0x80) --size;
                text.resize(size);
                text += "…";
            }
            WrappedColored(kText, text);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}
}
