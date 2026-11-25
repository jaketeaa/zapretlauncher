#include "tray_panel.h"

#include "app_version.h"
#include "font_resources.h"
#include "ui.h"
#include "utils.h"

#include <windowsx.h>
#include <gdiplus.h>
#include <wrl/client.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace tray_panel {
namespace {
using namespace Gdiplus;

constexpr wchar_t kClass[] = L"ZapretLauncherBetaTrayPanel";
constexpr int kWidth = 360;
constexpr int kHeight = 466;
constexpr int kVisibleRows = 7;
constexpr UINT_PTR kRefresh = 1;

const Color kBackground(15, 15, 17), kCard(25, 25, 29), kField(33, 33, 38);
const Color kBorder(48, 48, 54), kWhite(242, 242, 244), kMuted(165, 165, 174);
const Color kAmber(255, 176, 32), kSoftAmber(255, 199, 100), kGreen(80, 213, 125), kRed(246, 109, 103);

enum class Action { None, Close, Open, Power, Restart, Strategy, Favorites, All, Back, Choose, Favorite, Diagnose, Exit };
struct Target {
    RectF rect;
    Action action;
    bool enabled;
    std::string strategy;
};
struct Window {
    HWND hwnd = nullptr, launcher = nullptr;
    HINSTANCE instance = nullptr;
    AppState* app = nullptr;
    ULONG_PTR gdiplus = 0;
    Microsoft::WRL::ComPtr<IStream> iconStream;
    std::unique_ptr<Bitmap> icon;
    std::string lastUptime;
    float scale = 1.f;
    int scroll = 0, wheel = 0;
    bool showingStrategies = false, favoritesOnly = false;
    bool trackingMouse = false, keyboard = false;
    PointF mouse{-1, -1};
    int focus = -1;
    std::vector<Target> targets;
    std::unique_ptr<Bitmap> frame;

    ~Window() {
        icon.reset();
        iconStream.Reset();
        frame.reset();
        if (gdiplus) GdiplusShutdown(gdiplus);
    }
};

std::unique_ptr<Window> panel;

void LoadPanelIcon(Window& w) {
    const auto resource = FindResourceW(w.instance, MAKEINTRESOURCEW(PNG_ZAPRET_LAUNCHER), RT_RCDATA);
    const auto data = resource ? LoadResource(w.instance, resource) : nullptr;
    const auto* bytes = data ? LockResource(data) : nullptr;
    const DWORD size = bytes ? SizeofResource(w.instance, resource) : 0;
    if (!size || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &w.iconStream))) return;
    ULONG written = 0;
    const LARGE_INTEGER start{};
    if (FAILED(w.iconStream->Write(bytes, size, &written)) || written != size ||
        FAILED(w.iconStream->Seek(start, STREAM_SEEK_SET, nullptr))) return;
    w.icon.reset(Bitmap::FromStream(w.iconStream.Get()));
    if (w.icon && w.icon->GetLastStatus() != Ok) w.icon.reset();
}

void Shape(GraphicsPath& path, RectF r, float radius) {
    const float d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}
void Fill(Graphics& g, RectF r, float radius, Color color, Color edge = Color()) {
    GraphicsPath path;
    Shape(path, r, radius);
    SolidBrush brush(color);
    g.FillPath(&brush, &path);
    if (edge.GetAlpha()) {
        Pen border(edge, 1.f);
        g.DrawPath(&border, &path);
    }
}
void Text(Graphics& g, const std::wstring& text, RectF rect, float size, Color color,
          bool bold = false, StringAlignment align = StringAlignmentNear, bool mono = false) {
    Font font(mono ? L"Consolas" : L"Segoe UI", size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    StringFormat format(StringFormatFlagsNoWrap);
    format.SetAlignment(align);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush brush(color);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, rect, &format, &brush);
}
void Line(Graphics& g, float x, float y, float ex, float ey, Color ink, float weight = 1.5f) {
    Pen pen(ink, weight);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, x, y, ex, ey);
}
void Dot(Graphics& g, float x, float y, float radius, Color ink) {
    SolidBrush brush(ink);
    g.FillEllipse(&brush, x - radius, y - radius, 2 * radius, 2 * radius);
}

bool TargetAt(const Target& target, const PointF& point) {
    return target.enabled && target.rect.Contains(point.X, point.Y);
}
bool Hit(const Window& w, const RectF& r) { return r.Contains(w.mouse.X, w.mouse.Y); }
std::size_t Hovered(const Window& w) {
    return static_cast<std::size_t>(std::find_if(w.targets.begin(), w.targets.end(),
        [&](const auto& target) { return Hit(w, target.rect); }) - w.targets.begin());
}

void Button(Window& w, Graphics& g, Action action, RectF rect, const wchar_t* label, bool enabled = true,
            bool primary = false, Color ink = kWhite, std::string strategy = {}) {
    const bool focused = w.keyboard && w.focus == static_cast<int>(w.targets.size());
    const bool over = enabled && Hit(w, rect);
    w.targets.push_back({rect, action, enabled, std::move(strategy)});
    const Color fill = !enabled ? Color(24, 24, 27) : primary ? (over ? kSoftAmber : kAmber) :
                       over ? Color(48, 45, 43) : kField;
    Fill(g, rect, 10, fill, (over || focused) && enabled && !primary ? Color(117, 86, 39) : kBorder);
    if (focused && enabled && primary) Fill(g, {rect.X + 3, rect.Y + 3, rect.Width - 6, rect.Height - 6},
                                            8, fill, Color(88, 60, 17));
    Text(g, label, rect, 13, !enabled ? Color(104, 104, 113) : primary ? Color(32, 24, 12) : ink, true,
         StringAlignmentCenter);
}

void Star(Graphics& g, float x, float y, bool favorite, bool hovered) {
    PointF points[10];
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + i * .62831853f;
        const float r = i % 2 ? 3.5f : 8.f;
        points[i] = {x + r * std::cos(a), y + r * std::sin(a)};
    }
    const auto ink = favorite || hovered ? kSoftAmber : kMuted;
    Pen pen(ink, 1.25f);
    pen.SetLineJoin(LineJoinRound);
    if (favorite) {
        SolidBrush fill(ink);
        g.FillPolygon(&fill, points, 10);
    } else g.DrawPolygon(&pen, points, 10);
}

std::vector<std::string> Strategies(const Window& w) {
    std::vector<std::string> names;
    const auto& all = w.app->strategies;
    for (const auto& name : all) if (w.app->Favorite(name)) names.push_back(name);
    if (!w.favoritesOnly) for (const auto& name : all) if (!w.app->Favorite(name)) names.push_back(name);
    return names;
}
bool StrategyEnabled(const AppState& app) {
    return !app.Restoring() && !app.Deleting() && !app.svcMgr.Busy() && !app.term.IsRunning() &&
        !app.updater.Installing() && (!app.svcStatus.installed || (app.OwnService() && app.CanControl()));
}

void DrawDashboard(Window& w, Graphics& g) {
    const auto& app = *w.app;
    const auto& s = app.svcStatus;
    const bool own = app.OwnService();
    const bool running = own && s.state == SERVICE_RUNNING;
    const bool pending = app.svcMgr.Busy() || s.state == SERVICE_START_PENDING || s.state == SERVICE_STOP_PENDING ||
                         app.updater.Installing() || app.Deleting() || app.Restoring();
    const bool managed = own && app.CanControl() && !pending;
    const bool needsSetup = !s.installed && (!s.queryError || s.queryError == ERROR_SERVICE_DOES_NOT_EXIST);
    const bool known = !s.queryError || needsSetup;
    const auto accent = pending ? kAmber : running ? kGreen : kMuted;
    const wchar_t* state = pending ? L"Выполняется операция" : !known ? L"Статус недоступен" :
        running ? L"Служба работает" : own ? L"Служба остановлена" :
        s.installed ? L"Другая установка" : L"Служба не установлена";

    Fill(g, {16, 69, 328, 157}, 18, kCard, running ? Color(70, 89, 71) : kBorder);
    Dot(g, 35, 94, 7, Color(35, accent.GetR(), accent.GetG(), accent.GetB()));
    Dot(g, 35, 94, 3.8f, accent);
    Text(g, L"СЛУЖБА ZAPRET", {48, 83, 190, 22}, 11, accent, true);
    Text(g, state, {29, 109, 230, 27}, 18, kWhite, true);
    Text(g, util::FromUtf8(app.uptimeText), {29, 143, 237, 36}, 28, running ? kSoftAmber : kWhite, true,
         StringAlignmentNear, true);
    Text(g, L"Время работы", {30, 181, 222, 20}, 12, kMuted);
    if (app.svcMgr.GetState() == ServiceManager::State::Failed)
        Text(g, util::FromUtf8(app.svcMgr.GetStatus()), {30, 201, 286, 17}, 11, kRed);
    else if (pending && app.svcMgr.Busy())
        Text(g, util::FromUtf8(app.svcMgr.GetStatus()), {30, 201, 286, 17}, 11, kSoftAmber);

    const RectF power(271, 113, 55, 55);
    const bool powerHover = managed && Hit(w, power);
    const bool powerFocus = w.keyboard && w.focus == static_cast<int>(w.targets.size());
    w.targets.push_back({power, Action::Power, managed, {}});
    if (running || powerHover) {
        SolidBrush glow(Color(23, 255, 176, 32));
        g.FillEllipse(&glow, 264.f, 106.f, 69.f, 69.f);
    }
    SolidBrush face(running && managed ? (powerHover ? kSoftAmber : kAmber) :
                    powerHover ? Color(58, 48, 34) : kField);
    g.FillEllipse(&face, power);
    Pen edge(powerFocus || powerHover ? kSoftAmber : running ? Color(211, 144, 35) : kBorder, 1.f);
    g.DrawEllipse(&edge, power);
    const auto glyph = !managed ? kMuted : running ? Color(47, 31, 10) : kSoftAmber;
    Pen arc(glyph, 2.35f);
    arc.SetStartCap(LineCapRound);
    arc.SetEndCap(LineCapRound);
    g.DrawArc(&arc, 288.f, 130.f, 21.f, 21.f, -48.f, 276.f);
    Line(g, 298.5f, 126.f, 298.5f, 138.f, glyph, 2.35f);

    Button(w, g, needsSetup ? Action::Open : Action::Power, {16, 237, 197, 36},
           needsSetup ? L"Открыть для установки" : running ? L"Остановить zapret" : L"Запустить zapret",
           needsSetup || managed, true);
    Button(w, g, Action::Restart, {222, 237, 122, 36}, L"Перезапустить", managed && running);

    const RectF select(16, 288, 280, 61);
    const bool hasStrategies = !app.strategies.empty();
    const bool selectFocus = w.keyboard && w.focus == static_cast<int>(w.targets.size());
    w.targets.push_back({select, Action::Strategy, hasStrategies, {}});
    Fill(g, select, 12, hasStrategies && Hit(w, select) ? Color(42, 39, 36) : kCard,
         selectFocus ? kAmber : kBorder);
    auto active = own && !s.strategy.empty() ? s.strategy : app.config.settings.selectedBat;
    if (active.empty()) active = "Не выбрана";
    Text(g, own && !s.strategy.empty() ? L"Активная стратегия" : L"Выбранная стратегия", {29, 295, 229, 18}, 11, kMuted);
    Text(g, util::FromUtf8(active), {29, 316, 226, 24}, 14, kWhite, true);
    Line(g, 271, 313, 276, 318, kSoftAmber);
    Line(g, 276, 318, 271, 323, kSoftAmber);

    const RectF favorites(305, 288, 39, 61);
    const bool starFocus = w.keyboard && w.focus == static_cast<int>(w.targets.size());
    w.targets.push_back({favorites, Action::Favorites, hasStrategies, {}});
    Fill(g, favorites, 12, Hit(w, favorites) && hasStrategies ? Color(46, 38, 28) : kCard, starFocus ? kAmber : kBorder);
    Star(g, 324.5f, 318, false, hasStrategies && Hit(w, favorites));

    Button(w, g, Action::Diagnose, {16, 362, 156, 36}, app.diagnosticJob.valid() ? L"Выполняется…" : L"Диагностика",
           !app.diagnosticJob.valid());
    Button(w, g, Action::Open, {181, 362, 163, 36}, L"Открыть лаунчер");

    Line(g, 16, 416, 344, 416, kBorder, 1);
    const auto& results = app.diagnosticReport.network;
    std::wstring footer = L"БЫСТРЫЙ ДОСТУП";
    if (app.diagnosticJob.valid()) footer = L"Идёт диагностика…";
    else if (!results.empty()) {
        const auto available = std::count_if(results.begin(), results.end(), [](const auto& row) {
            return row.level == diagnostics::Level::Good;
        });
        footer = L"Доступно сервисов: " + std::to_wstring(available) + L" из " + std::to_wstring(results.size());
    }
    Text(g, footer, {17, 427, 199, 28}, 11, kMuted, true);
    Button(w, g, Action::Exit, {228, 425, 116, 29}, L"Выйти", true, false, kRed);
}

void DrawStrategies(Window& w, Graphics& g) {
    const auto& app = *w.app;
    Button(w, g, Action::Back, {16, 68, 67, 32}, L"‹ Назад");
    Text(g, L"Стратегии", {95, 67, 240, 34}, 19, kWhite, true);
    Button(w, g, Action::All, {16, 107, 155, 32}, L"Все", true, !w.favoritesOnly);
    Button(w, g, Action::Favorites, {179, 107, 165, 32}, L"Избранные", true, w.favoritesOnly);
    const auto names = Strategies(w);
    const auto maxScroll = std::max(0, static_cast<int>(names.size()) - kVisibleRows);
    w.scroll = std::clamp(w.scroll, 0, maxScroll);
    const bool allowed = StrategyEnabled(app);
    if (names.empty()) {
        Fill(g, {16, 151, 328, 76}, 14, kCard, kBorder);
        Text(g, w.favoritesOnly ? L"Избранных стратегий пока нет" : L"Файлы стратегий не найдены",
             {23, 170, 314, 35}, 14, kMuted, false, StringAlignmentCenter);
    }
    for (int i = w.scroll; i < static_cast<int>(names.size()) && i < w.scroll + kVisibleRows; ++i) {
        const auto& name = names[i];
        const float y = 150.f + (i - w.scroll) * 38.f;
        const RectF row(16, y, 282, 33), star(303, y, 34, 33);
        const bool active = app.OwnService() && !app.svcStatus.strategy.empty() &&
            util::ToLowerAscii(name) == util::ToLowerAscii(app.svcStatus.strategy + ".bat");
        const bool picked = name == app.config.settings.selectedBat;
        const bool focus = w.keyboard && w.focus == static_cast<int>(w.targets.size());
        w.targets.push_back({row, Action::Choose, allowed, name});
        Fill(g, row, 8, allowed && Hit(w, row) ? Color(49, 41, 31) : active ? Color(43, 35, 26) : kCard,
             focus || active ? Color(115, 83, 41) : kBorder);
        Dot(g, 29, y + 16.5f, 3, active ? kGreen : picked ? kAmber : kBorder);
        Text(g, util::FromUtf8(name), {40, y, 245, 33}, 12, allowed ? kWhite : kMuted, active);
        const bool focusedStar = w.keyboard && w.focus == static_cast<int>(w.targets.size());
        w.targets.push_back({star, Action::Favorite, !app.Restoring(), name});
        Fill(g, star, 8, Hit(w, star) ? Color(48, 39, 27) : kCard, focusedStar ? kAmber : kBorder);
        Star(g, 320, y + 16.5f, app.Favorite(name), Hit(w, star));
    }
    if (maxScroll > 0) {
        constexpr float h = 259.f;
        const float thumb = std::max(25.f, h * kVisibleRows / static_cast<float>(names.size()));
        Fill(g, {341, 150, 3, h}, 1.5f, kBorder);
        Fill(g, {341, 150 + (h - thumb) * w.scroll / maxScroll, 3, thumb}, 1.5f, Color(126, 107, 83));
    }
    Text(g, !allowed ? L"Управление сейчас недоступно" : app.OwnService() ?
         L"Выбор применяет стратегию к службе" : L"Выбор сохранит стратегию в настройках",
         {16, 420, 328, 27}, 12, kMuted);
}

void Draw(Window& w, HDC target, int width, int height) {
    if (width < 1 || height < 1 || !w.app) return;
    if (!w.frame || w.frame->GetWidth() != static_cast<UINT>(width) || w.frame->GetHeight() != static_cast<UINT>(height))
        w.frame = std::make_unique<Bitmap>(width, height, PixelFormat32bppPARGB);
    if (w.frame->GetLastStatus() != Ok) return;
    {
        Graphics g(w.frame.get());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(kBackground);
        g.SetPageUnit(UnitPixel);
        g.ScaleTransform(w.scale, w.scale);
        w.targets.clear();
        Fill(g, {.6f, .6f, kWidth - 1.2f, kHeight - 1.2f}, 18, kBackground, kBorder);
        if (w.icon) {
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.DrawImage(w.icon.get(), RectF(17, 18, 31, 31));
        }
        Text(g, L"ZAPRET LAUNCHER", {56, 13, 172, 26}, 16, kWhite, true);
        Text(g, LAUNCHER_LABEL_W, {57, 37, 193, 15}, 11, kSoftAmber);
        Button(w, g, Action::Close, {311, 17, 32, 28}, L"×");
        Line(g, 16, 59, 344, 59, kBorder, 1);
        if (w.showingStrategies) DrawStrategies(w, g);
        else {
            DrawDashboard(w, g);
            w.lastUptime = w.app->uptimeText;
        }
    }
    {
        Graphics dest(target);
        dest.DrawImage(w.frame.get(), 0, 0, width, height);
    }

}

void Redraw(Window& w) { InvalidateRect(w.hwnd, nullptr, FALSE); }
void Hide(Window& w) {
    KillTimer(w.hwnd, kRefresh);
    w.mouse = {-1, -1};
    w.trackingMouse = false;
    ShowWindow(w.hwnd, SW_HIDE);
}

void Activate(Window& w, Target hit) {
    if (!w.app || !hit.enabled) return;
    auto& app = *w.app;
    switch (hit.action) {
        case Action::Close: Hide(w); return;
        case Action::Open: Hide(w); SendMessageW(w.launcher, WM_APP_SHOW, 0, 0); return;
        case Action::Exit: Hide(w); PostMessageW(w.launcher, WM_APP_EXIT, 0, 0); return;
        case Action::Power: app.ToggleService(); break;
        case Action::Restart:
            if (app.OwnService() && app.CanControl() && app.svcStatus.state == SERVICE_RUNNING)
                app.svcMgr.Restart();
            break;
        case Action::Diagnose:
            app.StartDiagnostics(true);
            app.showDiagnosis = true;
            break;
        case Action::Favorite:
            if (!app.Restoring()) app.ToggleFavorite(hit.strategy);
            break;
        case Action::Choose:
            if (!StrategyEnabled(app)) break;
            if (app.OwnService()) app.ApplyStrategy(hit.strategy);
            else if (!app.svcStatus.installed) app.PickStrategy(hit.strategy);
            w.showingStrategies = false;
            w.focus = -1;
            break;
        case Action::All:
        case Action::Favorites:
        case Action::Strategy:
            w.showingStrategies = true;
            w.favoritesOnly = hit.action == Action::Favorites;
            w.scroll = 0;
            w.focus = -1;
            break;
        case Action::Back: w.showingStrategies = false; w.focus = -1; break;
        default: break;
    }
    Redraw(w);
}

void MoveFocus(Window& w, int direction) {
    const auto count = static_cast<int>(w.targets.size());
    if (!count) return;
    int candidate = w.focus < 0 ? (direction > 0 ? count - 1 : 0) : w.focus;
    for (int i = 0; i < count; ++i) {
        candidate = (candidate + direction + count) % count;
        if (w.targets[candidate].enabled) {
            w.focus = candidate;
            w.keyboard = true;
            Redraw(w);
            return;
        }
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const auto hdc = BeginPaint(hwnd, &paint);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            Draw(*w, hdc, rect.right, rect.bottom);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(hwnd)) Hide(*w);
            return 0;
        case WM_CLOSE: Hide(*w); return 0;
        case WM_TIMER: if (wp == kRefresh) Redraw(*w); return 0;
        case WM_MOUSEMOVE: {
            const auto hovered = Hovered(*w);
            const bool hadKeyboardFocus = w->keyboard;
            w->mouse = {GET_X_LPARAM(lp) / w->scale, GET_Y_LPARAM(lp) / w->scale};
            w->keyboard = false;
            if (!w->trackingMouse) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
                w->trackingMouse = TrackMouseEvent(&tracking) == TRUE;
            }
            if (hadKeyboardFocus || Hovered(*w) != hovered) Redraw(*w);
            return 0;
        }
        case WM_MOUSELEAVE:
            w->trackingMouse = false;
            w->mouse = {-1, -1};
            Redraw(*w);
            return 0;
        case WM_LBUTTONUP: {
            const PointF point{GET_X_LPARAM(lp) / w->scale, GET_Y_LPARAM(lp) / w->scale};
            const auto it = std::find_if(w->targets.begin(), w->targets.end(), [&](const auto& hit) { return TargetAt(hit, point); });
            if (it != w->targets.end()) Activate(*w, *it);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (w->showingStrategies) {
                w->wheel += GET_WHEEL_DELTA_WPARAM(wp);
                const int steps = w->wheel / WHEEL_DELTA;
                w->scroll = std::clamp(w->scroll - steps * 3, 0, std::max(0, static_cast<int>(Strategies(*w).size()) - kVisibleRows));
                w->wheel %= WHEEL_DELTA;
                w->focus = -1;
                Redraw(*w);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { Hide(*w); return 0; }
            if (wp == VK_TAB || wp == VK_DOWN || wp == VK_UP) {
                MoveFocus(*w, wp == VK_UP || (wp == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000)) ? -1 : 1);
                return 0;
            }
            if ((wp == VK_PRIOR || wp == VK_NEXT) && w->showingStrategies) {
                w->scroll = std::clamp(w->scroll + (wp == VK_PRIOR ? -kVisibleRows : kVisibleRows),
                                       0, std::max(0, static_cast<int>(Strategies(*w).size()) - kVisibleRows));
                w->focus = -1;
                Redraw(*w);
                return 0;
            }
            if ((wp == VK_RETURN || wp == VK_SPACE) && !(lp & (1L << 30)) &&
                w->focus >= 0 && w->focus < static_cast<int>(w->targets.size())) {
                Activate(*w, w->targets[w->focus]);
                return 0;
            }
            break;
        // Show() derives its final size from the destination monitor, including for a hidden window.
        case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Create() {
    auto w = std::make_unique<Window>();
    w->instance = GetModuleHandleW(nullptr);
    GdiplusStartupInput startup;
    if (GdiplusStartup(&w->gdiplus, &startup, nullptr) != Ok) return false;
    LoadPanelIcon(*w);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = w->instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpfnWndProc = Proc;
    cls.lpszClassName = kClass;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // An unowned tool window also works when the main window is minimized.
    w->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"Zapret Launcher — быстрый доступ",
                              WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, w->instance, w.get());
    if (!w->hwnd) {
        UnregisterClassW(kClass, w->instance);
        return false;
    }
    panel = std::move(w);
    return true;
}

RECT Place(const RECT& anchor, const MONITORINFO& monitor, int width, int height, int gap) {
    const auto& work = monitor.rcWork;
    const auto& screen = monitor.rcMonitor;
    int x = anchor.right - width;
    int y = anchor.top - height - gap;
    if (work.top > screen.top && anchor.top < work.top) y = anchor.bottom + gap;
    else if (work.left > screen.left && anchor.left < work.left) {
        x = anchor.right + gap;
        y = anchor.bottom - height;
    } else if (work.right < screen.right && anchor.right > work.right) {
        x = anchor.left - width - gap;
        y = anchor.bottom - height;
    } else if (y < work.top && anchor.bottom + gap + height <= work.bottom) {
        y = anchor.bottom + gap;
    }
    x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
    y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    return {x, y, x + width, y + height};
}
}

bool Show(HWND launcher, AppState& app, const RECT& anchor) {
    if (!panel && !Create()) return false;
    auto& w = *panel;
    if (IsWindowVisible(w.hwnd)) {
        Hide(w);
        return true;
    }
    const auto display = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(display, &monitor)) return false;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(display, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = GetDpiForSystem();
    const int availW = monitor.rcWork.right - monitor.rcWork.left;
    const int availH = monitor.rcWork.bottom - monitor.rcWork.top;
    if (availW <= 0 || availH <= 0) return false;
    w.scale = std::min({dx / 96.f, availW / static_cast<float>(kWidth), availH / static_cast<float>(kHeight)});
    const int width = std::clamp(static_cast<int>(std::lround(kWidth * w.scale)), 1, availW);
    const int height = std::clamp(static_cast<int>(std::lround(kHeight * w.scale)), 1, availH);
    const auto pos = Place(anchor, monitor, width, height, std::max(1, static_cast<int>(w.scale * 8.f)));
    w.app = &app;
    w.launcher = launcher;
    w.showingStrategies = w.favoritesOnly = w.keyboard = false;
    w.focus = -1;
    w.targets.clear();
    w.scroll = w.wheel = 0;
    const int diameter = std::max(1, static_cast<int>(36.f * w.scale));
    const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    if (region && !SetWindowRgn(w.hwnd, region, FALSE)) DeleteObject(region);

    SetWindowPos(w.hwnd, HWND_TOPMOST, pos.left, pos.top, width, height, SWP_SHOWWINDOW);
    SetForegroundWindow(w.hwnd);
    SetTimer(w.hwnd, kRefresh, 250, nullptr);
    Redraw(w);
    UpdateWindow(w.hwnd);
    return true;
}
void RefreshUptime() {
    if (panel && !panel->showingStrategies && IsWindowVisible(panel->hwnd) &&
        panel->app && panel->lastUptime != panel->app->uptimeText)
        Redraw(*panel);
}

void Dismiss() { if (panel && IsWindowVisible(panel->hwnd)) Hide(*panel); }
void Shutdown() {
    if (!panel) return;
    const auto instance = panel->instance;
    DestroyWindow(panel->hwnd);
    panel.reset();
    UnregisterClassW(kClass, instance);
}
}
