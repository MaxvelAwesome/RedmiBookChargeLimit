// Окно приложения (Direct2D/DirectWrite, Mica, светлая/тёмная тема) и иконка в трее.
// Пока окно скрыто, графические ресурсы освобождены, таймеры остановлены — процесс почти не потребляет
// ни памяти, ни CPU; обновление подсказки в трее идёт по системным уведомлениям о питании.
#include "ui.h"

#include "battery.h"
#include "installer.h"
#include "ipc.h"
#include "../res/resource.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace ui {
using Microsoft::WRL::ComPtr;

namespace {

// Локальные копии GUID уведомлений о питании (чтобы не зависеть от initguid).
constexpr GUID kGuidBatteryPercent = {0xA7AD8041, 0xB45A, 0x4CAE, {0x87, 0xA3, 0xEE, 0xCB, 0xB4, 0x68, 0xA9, 0xE1}};
constexpr GUID kGuidAcDcSource     = {0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};

constexpr UINT WM_APP_TRAY      = WM_APP + 10;
constexpr UINT WM_APP_DATA      = WM_APP + 11;
constexpr UINT WM_APP_INSTALLED = WM_APP + 12;
constexpr UINT_PTR kTimerRefresh = 1;
constexpr UINT_PTR kTimerSlow    = 2;
constexpr UINT_PTR kTimerTrim    = 3;
constexpr UINT kRefreshMs = 2000;
constexpr UINT kSlowMs    = 60000;

// Размеры клиентской области в DIP.
constexpr float kW = 440.f;
constexpr float kH = 620.f;
constexpr float kPad = 20.f;

// Программная отрисовка: не тянет в процесс драйвер GPU (меньше памяти), для статичного UI скорости хватает.
constexpr D2D1_RENDER_TARGET_TYPE kRtType = D2D1_RENDER_TARGET_TYPE_SOFTWARE;

D2D1_COLOR_F Hex(UINT32 rgb, float a = 1.f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}

struct Theme {
    bool dark = false;
    D2D1_COLOR_F bg, card, cardHover, stroke, text, text2, text3;
    D2D1_COLOR_F accent, accentHover, accentText;
    D2D1_COLOR_F green, greenTrack, orange, orangeTrack, red, redTrack;
    D2D1_COLOR_F onFill, onTrack;
};

struct Data {
    battery::Info bat;
    ipc::State svc;
    bool svcOk = false;
    bool installed = false;
};

DWORD ReadPersonalize(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", name,
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}

// Системный акцент: светлый вариант для тёмной темы, тёмный — для светлой (как в WinUI).
bool ReadAccent(bool dark, D2D1_COLOR_F& out) {
    BYTE pal[32] = {};
    DWORD sz = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &sz) != ERROR_SUCCESS || sz < 32)
        return false;
    int idx = dark ? 1 : 4;  // Light2 / Dark1
    out = D2D1::ColorF(pal[idx * 4] / 255.f, pal[idx * 4 + 1] / 255.f, pal[idx * 4 + 2] / 255.f, 1.f);
    return true;
}

Theme MakeTheme(bool dark) {
    Theme t;
    t.dark = dark;
    if (dark) {
        t.bg = Hex(0x202020);
        t.card = Hex(0xFFFFFF, 0.055f);
        t.cardHover = Hex(0xFFFFFF, 0.085f);
        t.stroke = Hex(0x000000, 0.20f);
        t.text = Hex(0xFFFFFF);
        t.text2 = Hex(0xFFFFFF, 0.786f);
        t.text3 = Hex(0xFFFFFF, 0.42f);
        t.accent = Hex(0x4CC2FF);
        t.accentText = Hex(0x000000);
        t.green = Hex(0x2FC562);
        t.greenTrack = Hex(0x2FC562, 0.22f);
        t.orange = Hex(0xF2A50C);
        t.orangeTrack = Hex(0xF2A50C, 0.22f);
        t.red = Hex(0xF0505A);
        t.redTrack = Hex(0xF0505A, 0.22f);
        t.onFill = Hex(0xFFFFFF);
        t.onTrack = Hex(0xFFFFFF);
    } else {
        t.bg = Hex(0xF3F3F3);
        t.card = Hex(0xFFFFFF, 0.70f);
        t.cardHover = Hex(0xF9F9F9, 0.50f);
        t.stroke = Hex(0x000000, 0.0578f);
        t.text = Hex(0x000000, 0.896f);
        t.text2 = Hex(0x000000, 0.606f);
        t.text3 = Hex(0x000000, 0.361f);
        t.accent = Hex(0x005FB8);
        t.accentText = Hex(0xFFFFFF);
        t.green = Hex(0x35D06A);
        t.greenTrack = Hex(0xACE9C1);
        t.orange = Hex(0xF5A623);
        t.orangeTrack = Hex(0xFBDDA6);
        t.red = Hex(0xE5484D);
        t.redTrack = Hex(0xF6C1C3);
        t.onFill = Hex(0xFFFFFF);
        t.onTrack = Hex(0x0E4D26);
    }
    ReadAccent(dark, t.accent);
    t.accentHover = t.accent;
    t.accentHover.a = 0.9f;
    return t;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[128];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

enum class Hit { None, Toggle, FullBtn, Uninstall, Install };

class App {
public:
    int Run(HINSTANCE inst, bool startHidden);

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT, WPARAM, LPARAM);

    // данные
    void RequestRefresh();
    void SendCommand(uint32_t cmd, uint32_t arg);
    void OnData(std::unique_ptr<Data> d, bool fromCommand);

    // окно
    void ShowMain();
    void HideMain();
    void ToggleFromHotkey();
    void ApplyTheme();
    void ResizeForDpi(UINT dpi, const RECT* suggested);

    // отрисовка
    bool EnsureResources();
    void ReleaseResources();
    void Paint();
    void DrawBatteryCard(const D2D1_RECT_F& r);
    void DrawMetric(const D2D1_RECT_F& r, const wchar_t* label, const std::wstring& value);
    void DrawToggle(const D2D1_RECT_F& r, bool on, bool enabled, bool hover);
    void DrawButton(const D2D1_RECT_F& r, const wchar_t* text, bool accent, bool enabled, bool hover);
    void Card(const D2D1_RECT_F& r, bool hover = false);
    void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c);
    Hit HitTest(float x, float y) const;
    void OnClick(Hit h);

    // трей
    void TrayAdd();
    void TrayUpdate();
    void TrayMenu(int x, int y);

    bool CanControl() const { return data_.svcOk && data_.svc.hw != ipc::kHwUnsupported && !busy_; }
    bool LimitActive() const { return data_.svcOk && data_.svc.enabled && !data_.svc.fullCharge; }

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HANDLE mutex_ = nullptr;
    HANDLE showEvent_ = nullptr;
    UINT taskbarCreated_ = 0;
    HPOWERNOTIFY pn1_ = nullptr, pn2_ = nullptr;
    UINT dpi_ = 96;
    bool mica_ = false;
    bool visible_ = false;
    bool busy_ = false;
    bool installing_ = false;
    std::atomic<bool> refreshing_{false};
    Hit hover_ = Hit::None;
    bool tracking_ = false;
    Theme th_;
    Data data_;
    bool haveData_ = false;

    // трей
    NOTIFYICONDATAW nid_{};
    bool trayAdded_ = false;
    int trayIconId_ = 0;
    HICON trayIcon_ = nullptr;

    // D2D
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> br_;
    ComPtr<IDWriteTextFormat> fTitle_, fBig_, fSub_, fLabel_, fValue_, fBody_, fCaption_, fButton_;

    // зоны нажатия (DIP)
    D2D1_RECT_F rcToggle_{}, rcFull_{}, rcUninstall_{}, rcInstall_{};
};

App* g_app = nullptr;

// ---------------------------------------------------------------- данные

void App::RequestRefresh() {
    if (refreshing_.exchange(true)) return;
    if (!TrySubmitThreadpoolCallback(
            [](PTP_CALLBACK_INSTANCE, void* ctx) {
                HWND h = static_cast<HWND>(ctx);
                auto d = std::make_unique<Data>();
                battery::Query(d->bat);
                d->svcOk = ipc::Call(ipc::kGetState, 0, d->svc, 1500);
                d->installed = d->svcOk || installer::IsInstalled();
                if (PostMessageW(h, WM_APP_DATA, 0, reinterpret_cast<LPARAM>(d.get()))) d.release();
            },
            hwnd_, nullptr)) {
        refreshing_ = false;
    }
}

void App::SendCommand(uint32_t cmd, uint32_t arg) {
    if (busy_) return;
    struct Ctx { HWND h; uint32_t cmd, arg; };
    auto* c = new Ctx{hwnd_, cmd, arg};
    busy_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!TrySubmitThreadpoolCallback(
            [](PTP_CALLBACK_INSTANCE, void* ctx) {
                std::unique_ptr<Ctx> c(static_cast<Ctx*>(ctx));
                auto d = std::make_unique<Data>();
                d->svcOk = ipc::Call(c->cmd, c->arg, d->svc, 10000);
                battery::Query(d->bat);
                d->installed = d->svcOk || installer::IsInstalled();
                if (PostMessageW(c->h, WM_APP_DATA, 1, reinterpret_cast<LPARAM>(d.get()))) d.release();
            },
            c, nullptr)) {
        delete c;
        busy_ = false;
    }
}

void App::OnData(std::unique_ptr<Data> d, bool fromCommand) {
    data_ = *d;
    haveData_ = true;
    if (fromCommand) busy_ = false;
    else refreshing_ = false;
    if (!showEvent_) showEvent_ = OpenEventW(SYNCHRONIZE, FALSE, app::kShowEvent);
    TrayUpdate();
    if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- окно

void App::ApplyTheme() {
    bool dark = ReadPersonalize(L"AppsUseLightTheme", 1) == 0;
    th_ = MakeTheme(dark);
    BOOL d = dark;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof(d));
    int backdrop = 2;  // DWMSBT_MAINWINDOW (Mica)
    mica_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop)));
    if (mica_) {
        MARGINS m{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
    }
    int corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ResizeForDpi(UINT dpi, const RECT* suggested) {
    dpi_ = dpi;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    RECT rc{0, 0, static_cast<LONG>(std::lround(kW * dpi / 96.f)), static_cast<LONG>(std::lround(kH * dpi / 96.f))};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (suggested) {
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
        int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ReleaseResources();
}

void App::ShowMain() {
    if (installing_) return;
    if (!visible_) {
        visible_ = true;
        ResizeForDpi(GetDpiForWindow(hwnd_), nullptr);
        ApplyTheme();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetTimer(hwnd_, kTimerRefresh, kRefreshMs, nullptr);
        RequestRefresh();
    } else if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    // Окно может открываться из фона (кнопка Xiaomi) — аккуратно выводим на передний план.
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

void App::HideMain() {
    if (!visible_) return;
    visible_ = false;
    KillTimer(hwnd_, kTimerRefresh);
    ShowWindow(hwnd_, SW_HIDE);
    ReleaseResources();
    dw_.Reset();
    d2d_.Reset();
    hover_ = Hit::None;
    // Сжатие кучи и рабочего набора — чуть позже, когда отработают сообщения скрытия.
    SetTimer(hwnd_, kTimerTrim, 1500, nullptr);
}

void App::ToggleFromHotkey() {
    if (visible_ && !IsIconic(hwnd_) && GetForegroundWindow() == hwnd_) HideMain();
    else ShowMain();
}

// ---------------------------------------------------------------- отрисовка

bool App::EnsureResources() {
    if (rt_) return true;
    // Фабрики создаются только на время показа окна и полностью освобождаются при скрытии.
    if (!d2d_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (!dw_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                           reinterpret_cast<IUnknown**>(dw_.GetAddressOf()))))
        return false;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float dpi = static_cast<float>(dpi_);
    auto props = D2D1::RenderTargetProperties(kRtType,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                              dpi, dpi);
    auto hprops = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
    if (FAILED(d2d_->CreateHwndRenderTarget(props, hprops, &rt_))) return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(th_.text, &br_);

    auto mk = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* family, float size, DWRITE_FONT_WEIGHT w) {
        dw_->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", &f);
        if (f) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    };
    const wchar_t* disp = L"Segoe UI Variable Display";
    const wchar_t* text = L"Segoe UI Variable Text";
    mk(fTitle_, disp, 22.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fBig_, disp, 44.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fSub_, text, 15.f, DWRITE_FONT_WEIGHT_NORMAL);
    mk(fLabel_, text, 12.f, DWRITE_FONT_WEIGHT_NORMAL);
    mk(fValue_, disp, 17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fBody_, text, 14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fCaption_, text, 12.f, DWRITE_FONT_WEIGHT_NORMAL);
    mk(fButton_, text, 14.f, DWRITE_FONT_WEIGHT_NORMAL);
    if (fButton_) fButton_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (fCaption_) fCaption_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return br_ && fTitle_ && fBig_ && fSub_ && fLabel_ && fValue_ && fBody_ && fCaption_ && fButton_;
}

void App::ReleaseResources() {
    fTitle_.Reset(); fBig_.Reset(); fSub_.Reset(); fLabel_.Reset(); fValue_.Reset();
    fBody_.Reset(); fCaption_.Reset(); fButton_.Reset();
    br_.Reset();
    rt_.Reset();
}

void App::Card(const D2D1_RECT_F& r, bool hover) {
    auto rr = D2D1::RoundedRect(r, 8.f, 8.f);
    br_->SetColor(hover ? th_.cardHover : th_.card);
    if (!mica_ && !th_.dark && !hover) br_->SetColor(Hex(0xFBFBFB));
    rt_->FillRoundedRectangle(rr, br_.Get());
    br_->SetColor(th_.stroke);
    auto inner = D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 8.f, 8.f);
    rt_->DrawRoundedRectangle(inner, br_.Get(), 1.f);
}

void App::Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    br_->SetColor(c);
    rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, br_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

namespace {
// Заголовок карточки батареи: «4 ч 32 мин» (единицы мельче) + подпись.
struct Headline {
    std::wstring big;
    std::vector<DWRITE_TEXT_RANGE> smallRanges;
    std::wstring sub;
};

void AddTime(Headline& h, long sec) {
    long minutes = (sec + 30) / 60;
    long hh = minutes / 60, mm = minutes % 60;
    if (hh > 0) {
        h.big = std::to_wstring(hh);
        h.smallRanges.push_back({static_cast<UINT32>(h.big.size()), 3});
        h.big += L" ч ";
    }
    std::wstring m = std::to_wstring(mm);
    h.big += m;
    h.smallRanges.push_back({static_cast<UINT32>(h.big.size()), 4});
    h.big += L" мин";
}

void AddPercent(Headline& h, int p) {
    h.big = std::to_wstring(p < 0 ? 0 : p);
    h.smallRanges.push_back({static_cast<UINT32>(h.big.size()), 2});
    h.big += L" %";
}
}  // namespace

void App::DrawBatteryCard(const D2D1_RECT_F& r) {
    const battery::Info& b = data_.bat;
    int p = b.percent < 0 ? 0 : b.percent > 100 ? 100 : b.percent;
    bool limit = LimitActive();

    D2D1_COLOR_F fill = th_.green, track = th_.greenTrack;
    if (!b.acOnline && p <= 10) { fill = th_.red; track = th_.redTrack; }
    else if (!b.acOnline && p <= 20) { fill = th_.orange; track = th_.orangeTrack; }

    auto rr = D2D1::RoundedRect(r, 16.f, 16.f);
    br_->SetColor(track);
    rt_->FillRoundedRectangle(rr, br_.Get());
    float fillRight = r.left + (r.right - r.left) * p / 100.f;
    if (p > 0) {
        rt_->PushAxisAlignedClip(D2D1::RectF(r.left, r.top, fillRight, r.bottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        br_->SetColor(fill);
        rt_->FillRoundedRectangle(rr, br_.Get());
        rt_->PopAxisAlignedClip();
    }

    // Отметка лимита 80%.
    if (limit) {
        float x = r.left + (r.right - r.left) * 0.8f;
        D2D1_COLOR_F mc = x < fillRight ? Hex(0xFFFFFF, 0.75f) : (th_.dark ? Hex(0xFFFFFF, 0.5f) : Hex(0x0E4D26, 0.45f));
        br_->SetColor(mc);
        for (float y = r.top + 30.f; y < r.bottom - 8.f; y += 7.f)
            rt_->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x, std::fmin(y + 4.f, r.bottom - 8.f)), br_.Get(), 1.5f);
        Text(L"80%", fCaption_.Get(), D2D1::RectF(x - 20.f, r.top + 6.f, x + 30.f, r.top + 26.f), mc);
    }

    // Текст.
    Headline h;
    if (!haveData_) {
        h.big = L"…";
        h.sub = L"Загрузка";
    } else if (!b.present) {
        h.big = L"—";
        h.sub = L"Батарея не обнаружена";
    } else if (b.acOnline) {
        long target = (limit && p < 80) ? b.fullMwh * 8 / 10 : b.fullMwh;
        if (b.charging && b.rateMw > 0 && b.capacityMwh >= 0 && target > b.capacityMwh) {
            AddTime(h, static_cast<long>(static_cast<double>(target - b.capacityMwh) / b.rateMw * 3600.0));
            h.sub = Fmt(limit && p < 80 ? L"До 80%% | Батарея %d%%" : L"До полной зарядки | Батарея %d%%", p);
        } else if (b.charging) {
            AddPercent(h, p);
            h.sub = L"Заряжается";
        } else if (p >= 99) {
            AddPercent(h, p);
            h.sub = L"Батарея заряжена | Питание от сети";
        } else if (limit && p >= 75) {
            AddPercent(h, p);
            h.sub = L"Зарядка на паузе | Лимит 80%";
        } else {
            AddPercent(h, p);
            h.sub = L"Подключено, не заряжается";
        }
    } else {
        long sec = b.estimatedSec;
        if (sec <= 0 && b.rateMw < 0 && b.capacityMwh > 0)
            sec = static_cast<long>(static_cast<double>(b.capacityMwh) / -b.rateMw * 3600.0);
        if (sec > 0 && sec < 48 * 3600) {
            AddTime(h, sec);
            h.sub = Fmt(L"Осталось | Батарея %d%%", p);
        } else {
            AddPercent(h, p);
            h.sub = L"Работа от батареи";
        }
    }

    // Текст двухцветный: белый над заливкой, контрастный над дорожкой (граница — край заливки).
    float tx = r.left + 24.f;
    ComPtr<IDWriteTextLayout> lay;
    if (SUCCEEDED(dw_->CreateTextLayout(h.big.c_str(), static_cast<UINT32>(h.big.size()), fBig_.Get(), 360.f, 56.f, &lay))) {
        for (const auto& rg : h.smallRanges) {
            lay->SetFontSize(20.f, rg);
            lay->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, rg);
        }
        lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    }
    auto drawText = [&](const D2D1_COLOR_F& c) {
        if (lay) {
            br_->SetColor(c);
            rt_->DrawTextLayout(D2D1::Point2F(tx, r.top + 14.f), lay.Get(), br_.Get());
        }
        Text(h.sub, fSub_.Get(), D2D1::RectF(tx, r.top + 72.f, r.right - 16.f, r.top + 98.f), c);
    };
    if (fillRight > r.left) {
        rt_->PushAxisAlignedClip(D2D1::RectF(r.left, r.top, fillRight, r.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        drawText(th_.onFill);
        rt_->PopAxisAlignedClip();
    }
    if (fillRight < r.right) {
        rt_->PushAxisAlignedClip(D2D1::RectF(fillRight, r.top, r.right, r.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        drawText(th_.onTrack);
        rt_->PopAxisAlignedClip();
    }
}

void App::DrawMetric(const D2D1_RECT_F& r, const wchar_t* label, const std::wstring& value) {
    Card(r);
    Text(label, fLabel_.Get(), D2D1::RectF(r.left + 14.f, r.top + 8.f, r.right - 8.f, r.top + 28.f), th_.text2);
    Text(value, fValue_.Get(), D2D1::RectF(r.left + 14.f, r.top + 28.f, r.right - 8.f, r.bottom - 8.f), th_.text);
}

void App::DrawToggle(const D2D1_RECT_F& r, bool on, bool enabled, bool hover) {
    auto rr = D2D1::RoundedRect(r, 10.f, 10.f);
    float cy = (r.top + r.bottom) / 2.f;
    float knob = hover && enabled ? 7.f : 6.f;
    if (on) {
        br_->SetColor(enabled ? th_.accent : th_.text3);
        rt_->FillRoundedRectangle(rr, br_.Get());
        br_->SetColor(enabled ? th_.accentText : th_.bg);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.right - 10.f, cy), knob, knob), br_.Get());
    } else {
        br_->SetColor(enabled ? th_.text2 : th_.text3);
        auto in = D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 9.5f, 9.5f);
        rt_->DrawRoundedRectangle(in, br_.Get(), 1.f);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 10.f, cy), knob - 1.f, knob - 1.f), br_.Get());
    }
}

void App::DrawButton(const D2D1_RECT_F& r, const wchar_t* text, bool accent, bool enabled, bool hover) {
    auto rr = D2D1::RoundedRect(r, 4.f, 4.f);
    if (accent && enabled) {
        br_->SetColor(hover ? th_.accentHover : th_.accent);
        rt_->FillRoundedRectangle(rr, br_.Get());
        Text(text, fButton_.Get(), r, th_.accentText);
    } else {
        br_->SetColor(hover && enabled ? th_.cardHover : (th_.dark ? Hex(0xFFFFFF, 0.06f) : Hex(0xFFFFFF, 0.9f)));
        rt_->FillRoundedRectangle(rr, br_.Get());
        br_->SetColor(th_.dark ? Hex(0xFFFFFF, 0.09f) : Hex(0x000000, 0.10f));
        auto in = D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 4.f, 4.f);
        rt_->DrawRoundedRectangle(in, br_.Get(), 1.f);
        Text(text, fButton_.Get(), r, enabled ? th_.text : th_.text3);
    }
}

void App::Paint() {
    if (!EnsureResources()) return;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(mica_ ? D2D1::ColorF(0, 0, 0, 0) : th_.bg);

    const battery::Info& b = data_.bat;
    const ipc::State& s = data_.svc;
    const float L = kPad, R = kW - kPad;
    float y = 14.f;

    // Заголовок + состояние.
    Text(L"Батарея", fTitle_.Get(), D2D1::RectF(L, y, R, y + 36.f), th_.text);
    {
        std::wstring chip;
        if (!haveData_) chip = L"";
        else if (!data_.svcOk) chip = L"Лимит не управляется";
        else if (s.fullCharge) chip = L"Разовая зарядка до 100%";
        else if (s.enabled) chip = L"Лимит 80% включён";
        else chip = L"Без лимита";
        ComPtr<IDWriteTextFormat> f = fLabel_;
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        Text(chip, f.Get(), D2D1::RectF(L, y + 8.f, R, y + 30.f), th_.text2);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    y += 46.f;

    DrawBatteryCard(D2D1::RectF(L, y, R, y + 112.f));
    y += 112.f + 16.f;

    // Метрики.
    const float gap = 8.f, cw = (R - L - gap) / 2.f, ch = 64.f;
    auto cell = [&](int i) {
        float cx = L + (i % 2) * (cw + gap);
        float cy = y + (i / 2) * (ch + gap);
        return D2D1::RectF(cx, cy, cx + cw, cy + ch);
    };
    std::wstring dash = L"—";
    std::wstring power = dash;
    if (b.present && b.capacityMwh >= 0) {
        double w = b.rateMw / 1000.0;
        power = std::fabs(w) < 0.05 ? std::wstring(L"0 Вт") : Fmt(w > 0 ? L"+%.1f Вт" : L"−%.1f Вт", std::fabs(w));
    }
    DrawMetric(cell(0), L"Мощность", power);
    DrawMetric(cell(1), L"Напряжение", b.voltageMv > 0 ? Fmt(L"%.2f В", b.voltageMv / 1000.0) : dash);
    DrawMetric(cell(2), L"Энергия сейчас / полная",
               b.capacityMwh >= 0 && b.fullMwh > 0 ? Fmt(L"%.1f / %.1f Вт·ч", b.capacityMwh / 1000.0, b.fullMwh / 1000.0) : dash);
    std::wstring health = dash;
    if (s.health) health = Fmt(L"%u %%", s.health);
    else if (b.fullMwh > 0 && b.designMwh > 0) health = Fmt(L"%.0f %%", 100.0 * b.fullMwh / b.designMwh);
    if (b.fullMwh > 0 && b.designMwh > 0 && b.fullMwh < b.designMwh)
        health += Fmt(L"  · износ %.0f %%", 100.0 - 100.0 * b.fullMwh / b.designMwh);
    DrawMetric(cell(3), L"Здоровье батареи", health);
    DrawMetric(cell(4), L"Циклы зарядки", b.cycles > 0 ? std::to_wstring(b.cycles) : dash);
    if (b.temperatureC > -100)
        DrawMetric(cell(5), L"Температура", Fmt(L"%.1f °C", b.temperatureC));
    else
        DrawMetric(cell(5), L"Проектная ёмкость", b.designMwh > 0 ? Fmt(L"%.1f Вт·ч", b.designMwh / 1000.0) : dash);
    y += 3 * ch + 2 * gap + 16.f;

    rcToggle_ = rcFull_ = rcInstall_ = D2D1::RectF();
    if (haveData_ && !data_.installed) {
        D2D1_RECT_F c = D2D1::RectF(L, y, R, y + 156.f);
        Card(c);
        Text(L"Приложение не установлено", fBody_.Get(), D2D1::RectF(L + 16.f, y + 14.f, R - 16.f, y + 36.f), th_.text);
        Text(L"Установите его, чтобы лимит 80% сам применялся после включения, перезагрузки, сна и "
             L"подключения зарядки. Понадобятся права администратора.",
             fCaption_.Get(), D2D1::RectF(L + 16.f, y + 38.f, R - 16.f, y + 92.f), th_.text2);
        rcInstall_ = D2D1::RectF(L + 16.f, y + 104.f, L + 196.f, y + 136.f);
        DrawButton(rcInstall_, installing_ ? L"Установка…" : L"Установить", true, !installing_, hover_ == Hit::Install);
        y += 156.f + 8.f;
    } else {
        bool ctl = CanControl();
        // Лимит.
        rcToggle_ = D2D1::RectF(L, y, R, y + 70.f);
        Card(rcToggle_, hover_ == Hit::Toggle && ctl);
        Text(L"Лимит заряда 80%", fBody_.Get(), D2D1::RectF(L + 16.f, y + 13.f, R - 80.f, y + 35.f), ctl ? th_.text : th_.text3);
        Text(L"Бережёт батарею: заряд не выше 80%", fCaption_.Get(),
             D2D1::RectF(L + 16.f, y + 35.f, R - 120.f, y + 57.f), th_.text2);
        bool on = data_.svcOk ? s.enabled != 0 : false;
        Text(on ? L"Вкл." : L"Откл.", fButton_.Get(), D2D1::RectF(R - 112.f, y, R - 64.f, y + 70.f), ctl ? th_.text : th_.text3);
        DrawToggle(D2D1::RectF(R - 60.f, y + 25.f, R - 20.f, y + 45.f), on, ctl, hover_ == Hit::Toggle);
        y += 70.f + 8.f;

        // Разовая зарядка до 100%.
        D2D1_RECT_F c = D2D1::RectF(L, y, R, y + 70.f);
        Card(c);
        const wchar_t* desc;
        bool fullEnabled = ctl;
        if (s.fullCharge) desc = L"Идёт зарядка. Лимит 80% вернётся сам на 100%";
        else if (!s.enabled) { desc = L"Доступно, когда лимит включён"; fullEnabled = false; }
        else if (!b.acOnline) { desc = L"Подключите зарядное устройство"; fullEnabled = false; }
        else desc = L"После 100% лимит 80% включится автоматически";
        Text(L"Зарядить до 100% один раз", fBody_.Get(), D2D1::RectF(L + 16.f, y + 13.f, R - 140.f, y + 35.f),
             fullEnabled || s.fullCharge ? th_.text : th_.text3);
        Text(desc, fCaption_.Get(), D2D1::RectF(L + 16.f, y + 35.f, R - 136.f, y + 62.f), th_.text2);
        rcFull_ = D2D1::RectF(R - 128.f, y + 19.f, R - 16.f, y + 51.f);
        DrawButton(rcFull_, s.fullCharge ? L"Отменить" : L"Зарядить", !s.fullCharge, s.fullCharge ? ctl : fullEnabled,
                   hover_ == Hit::FullBtn);
        y += 70.f + 8.f;
    }

    // Подвал: состояние службы и удаление.
    y = kH - 40.f;
    {
        D2D1_COLOR_F dot = th_.green;
        std::wstring st;
        if (!haveData_) { st = L"Подключение…"; dot = th_.text3; }
        else if (!data_.installed) { st = L"Работает без установки"; dot = th_.orange; }
        else if (!data_.svcOk) { st = L"Служба не отвечает"; dot = th_.red; }
        else if (s.hw == ipc::kHwUnsupported) { st = L"Прошивка не поддерживает лимит"; dot = th_.red; }
        else if (s.hw == ipc::kHwError) { st = L"Нет связи с прошивкой, повторяю…"; dot = th_.orange; }
        else if (busy_) { st = L"Применяю…"; dot = th_.accent; }
        else st = L"Служба работает";
        br_->SetColor(dot);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(L + 5.f, y + 12.f), 4.f, 4.f), br_.Get());
        Text(st, fLabel_.Get(), D2D1::RectF(L + 16.f, y, R - 150.f, y + 24.f), th_.text2);
    }
    rcUninstall_ = D2D1::RectF();
    if (data_.installed) {
        rcUninstall_ = D2D1::RectF(R - 140.f, y, R, y + 24.f);
        ComPtr<IDWriteTextLayout> lay;
        const wchar_t* t = L"Удалить приложение";
        if (SUCCEEDED(dw_->CreateTextLayout(t, static_cast<UINT32>(wcslen(t)), fLabel_.Get(), 140.f, 24.f, &lay))) {
            lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (hover_ == Hit::Uninstall) lay->SetUnderline(TRUE, {0, static_cast<UINT32>(wcslen(t))});
            br_->SetColor(th_.accent);
            rt_->DrawTextLayout(D2D1::Point2F(R - 140.f, y), lay.Get(), br_.Get());
        }
    }

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) ReleaseResources();
}

Hit App::HitTest(float x, float y) const {
    auto in = [&](const D2D1_RECT_F& r) { return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rcInstall_)) return Hit::Install;
    if (in(rcFull_)) return Hit::FullBtn;
    if (in(rcToggle_)) return Hit::Toggle;
    if (in(rcUninstall_)) return Hit::Uninstall;
    return Hit::None;
}

void App::OnClick(Hit h) {
    const ipc::State& s = data_.svc;
    switch (h) {
    case Hit::Toggle:
        if (CanControl()) SendCommand(ipc::kSetEnabled, s.enabled ? 0 : 1);
        break;
    case Hit::FullBtn:
        if (!CanControl()) break;
        if (s.fullCharge) SendCommand(ipc::kFullCharge, 0);
        else if (s.enabled && data_.bat.acOnline) SendCommand(ipc::kFullCharge, 1);
        break;
    case Hit::Uninstall:
        if (MessageBoxW(hwnd_,
                        L"Удалить RedmiBook Charge Limit?\n\nЛимит заряда будет выключен (батарея снова будет "
                        L"заряжаться до 100%), служба, настройки и журналы — удалены.",
                        app::kAppName, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            HANDLE p = installer::StartElevated(L"--uninstall", hwnd_);
            if (p) CloseHandle(p);  // установщик сам закроет это окно
        }
        break;
    case Hit::Install: {
        if (installing_) break;
        HANDLE p = installer::StartElevated(L"--install", hwnd_);
        if (!p) break;
        installing_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        struct Ctx { HWND h; HANDLE p; };
        auto* c = new Ctx{hwnd_, p};
        if (!TrySubmitThreadpoolCallback(
                [](PTP_CALLBACK_INSTANCE inst, void* ctx) {
                    CallbackMayRunLong(inst);
                    std::unique_ptr<Ctx> c(static_cast<Ctx*>(ctx));
                    WaitForSingleObject(c->p, INFINITE);
                    DWORD code = 1;
                    GetExitCodeProcess(c->p, &code);
                    CloseHandle(c->p);
                    PostMessageW(c->h, WM_APP_INSTALLED, code, 0);
                },
                c, nullptr)) {
            CloseHandle(p);
            delete c;
            installing_ = false;
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- трей

void App::TrayAdd() {
    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = WM_APP_TRAY;
    trayIconId_ = 0;
    if (trayIcon_) { DestroyIcon(trayIcon_); trayIcon_ = nullptr; }
    LoadIconMetric(inst_, MAKEINTRESOURCEW(IDI_TRAY_ON), LIM_SMALL, &trayIcon_);
    nid_.hIcon = trayIcon_;
    wcscpy_s(nid_.szTip, app::kAppName);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    if (trayAdded_) {
        nid_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    }
    TrayUpdate();
}

void App::TrayUpdate() {
    if (!trayAdded_) return;
    const ipc::State& s = data_.svc;
    int id = IDI_TRAY_ON;
    if (!data_.svcOk || !s.enabled) id = IDI_TRAY_OFF;
    else if (s.fullCharge) id = IDI_TRAY_FULL;

    nid_.uFlags = NIF_TIP | NIF_SHOWTIP;
    if (id != trayIconId_) {
        HICON ic = nullptr;
        if (SUCCEEDED(LoadIconMetric(inst_, MAKEINTRESOURCEW(id), LIM_SMALL, &ic))) {
            if (trayIcon_) DestroyIcon(trayIcon_);
            trayIcon_ = ic;
            trayIconId_ = id;
            nid_.hIcon = ic;
            nid_.uFlags |= NIF_ICON;
        }
    }
    std::wstring tip = L"Charge Limit";
    if (data_.bat.percent >= 0) tip += Fmt(L" — батарея %d%%", data_.bat.percent);
    if (!data_.svcOk) tip += L"\nЛимит не управляется";
    else if (s.fullCharge) tip += L"\nРазовая зарядка до 100%";
    else if (s.enabled) tip += L"\nЛимит 80% включён";
    else tip += L"\nЛимит выключен";
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void App::TrayMenu(int x, int y) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    const ipc::State& s = data_.svc;
    bool ctl = CanControl();
    AppendMenuW(m, MF_STRING, 1, L"Открыть");
    SetMenuDefaultItem(m, 1, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s.enabled && data_.svcOk ? MF_CHECKED : 0) | (ctl ? 0 : MF_GRAYED), 2, L"Лимит заряда 80%");
    bool fullOk = ctl && s.enabled && (s.fullCharge || data_.bat.acOnline);
    AppendMenuW(m, MF_STRING | (s.fullCharge ? MF_CHECKED : 0) | (fullOk ? 0 : MF_GRAYED), 3, L"Зарядить до 100% один раз");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, L"Выход (лимит продолжит работать)");

    SetForegroundWindow(hwnd_);
    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    int cmd = TrackPopupMenuEx(m, flags, x, y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
    case 1: ShowMain(); break;
    case 2: SendCommand(ipc::kSetEnabled, s.enabled ? 0 : 1); break;
    case 3: SendCommand(ipc::kFullCharge, s.fullCharge ? 0 : 1); break;
    case 4: DestroyWindow(hwnd_); break;
    default: break;
    }
}

// ---------------------------------------------------------------- оконная процедура

LRESULT CALLBACK App::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_app && !g_app->hwnd_) g_app->hwnd_ = h;
    return g_app ? g_app->WndProc(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT App::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreated_ && taskbarCreated_) {
        TrayAdd();  // Explorer перезапустился
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        ChangeWindowMessageFilterEx(hwnd_, taskbarCreated_, MSGFLT_ALLOW, nullptr);
        dpi_ = GetDpiForWindow(hwnd_);
        ApplyTheme();
        pn1_ = RegisterPowerSettingNotification(hwnd_, &kGuidBatteryPercent, DEVICE_NOTIFY_WINDOW_HANDLE);
        pn2_ = RegisterPowerSettingNotification(hwnd_, &kGuidAcDcSource, DEVICE_NOTIFY_WINDOW_HANDLE);
        TrayAdd();
        SetTimer(hwnd_, kTimerSlow, kSlowMs, nullptr);
        RequestRefresh();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        if (visible_) Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_DPICHANGED:
        ResizeForDpi(HIWORD(wp), reinterpret_cast<RECT*>(lp));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            ReleaseResources();
            ApplyTheme();
            TrayUpdate();
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ReleaseResources();
        ApplyTheme();
        return 0;

    case WM_TIMER:
        if (wp == kTimerRefresh && visible_) RequestRefresh();
        if (wp == kTimerTrim) {
            KillTimer(hwnd_, kTimerTrim);
            if (!visible_) {
                HeapCompact(GetProcessHeap(), 0);
                SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
            }
        }
        if (wp == kTimerSlow) {
            if (!showEvent_) showEvent_ = OpenEventW(SYNCHRONIZE, FALSE, app::kShowEvent);
            if (!visible_) RequestRefresh();
        }
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE || wp == PBT_APMPOWERSTATUSCHANGE || wp == PBT_APMRESUMEAUTOMATIC) RequestRefresh();
        return TRUE;

    case WM_APP_DATA:
        OnData(std::unique_ptr<Data>(reinterpret_cast<Data*>(lp)), wp == 1);
        return 0;

    case WM_APP_INSTALLED:
        if (wp == 0) {
            // Передаём работу установленной копии и выходим.
            if (mutex_) { CloseHandle(mutex_); mutex_ = nullptr; }
            std::wstring exe = app::InstallDir() + L"\\" + app::kExeName;
            std::wstring cmd = L"\"" + exe + L"\" --show";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                AllowSetForegroundWindow(pi.dwProcessId);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            DestroyWindow(hwnd_);
        } else {
            installing_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            MessageBoxW(hwnd_, L"Установка не завершена. Подробности — в журнале C:\\ProgramData\\RedmiChargeLimit\\log.txt.",
                        app::kAppName, MB_OK | MB_ICONWARNING);
        }
        return 0;

    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (visible_ && GetForegroundWindow() == hwnd_) HideMain();
            else ShowMain();
            break;
        case WM_CONTEXTMENU:
            TrayMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        default:
            break;
        }
        return 0;

    case app::WM_APP_SHOW:
        ShowMain();
        return 0;
    case app::WM_APP_QUIT:
        DestroyWindow(hwnd_);
        return 0;

    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        float s = 96.f / dpi_;
        Hit h = HitTest(GET_X_LPARAM(lp) * s, GET_Y_LPARAM(lp) * s);
        if (h != hover_) { hover_ = h; InvalidateRect(hwnd_, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ != Hit::None) { hover_ = Hit::None; InvalidateRect(hwnd_, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        float s = 96.f / dpi_;
        OnClick(HitTest(GET_X_LPARAM(lp) * s, GET_Y_LPARAM(lp) * s));
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, hover_ == Hit::Uninstall ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { HideMain(); return 0; }
        break;

    case WM_CLOSE:
        HideMain();  // окно прячется в трей, приложение продолжает работать
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, kTimerRefresh);
        KillTimer(hwnd_, kTimerSlow);
        if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &nid_);
        trayAdded_ = false;
        if (trayIcon_) { DestroyIcon(trayIcon_); trayIcon_ = nullptr; }
        if (pn1_) UnregisterPowerSettingNotification(pn1_);
        if (pn2_) UnregisterPowerSettingNotification(pn2_);
        ReleaseResources();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

int App::Run(HINSTANCE inst, bool startHidden) {
    inst_ = inst;

    // Один экземпляр на систему: второй запуск просто показывает окно первого.
    mutex_ = CreateMutexW(nullptr, FALSE, app::kUiMutex);
    if (!mutex_ || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startHidden) {
            if (HWND w = FindWindowW(app::kWindowClass, nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(w, app::WM_APP_SHOW, 0, 0);
            }
        }
        if (mutex_) CloseHandle(mutex_);
        return 0;
    }


    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
    wc.lpszClassName = app::kWindowClass;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    CreateWindowExW(0, app::kWindowClass, L"RedmiBook Charge Limit", style, CW_USEDEFAULT, CW_USEDEFAULT,
                    480, 700, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) { CloseHandle(mutex_); return 1; }
    showEvent_ = OpenEventW(SYNCHRONIZE, FALSE, app::kShowEvent);
    if (!startHidden) ShowMain();
    else SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));

    MSG msg;
    for (;;) {
        DWORD n = showEvent_ ? 1 : 0;
        DWORD r = MsgWaitForMultipleObjectsEx(n, &showEvent_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (n && r == WAIT_OBJECT_0) {
            ToggleFromHotkey();  // кнопка Xiaomi
            continue;
        }
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
    }

    if (showEvent_) CloseHandle(showEvent_);
    if (wc.hIcon) DestroyIcon(wc.hIcon);
    if (wc.hIconSm) DestroyIcon(wc.hIconSm);
    if (mutex_) CloseHandle(mutex_);
    return static_cast<int>(msg.wParam);
}

}  // namespace

int Run(HINSTANCE inst, bool startHidden) {
    App a;
    g_app = &a;
    int r = a.Run(inst, startHidden);
    g_app = nullptr;
    return r;
}

}  // namespace ui
