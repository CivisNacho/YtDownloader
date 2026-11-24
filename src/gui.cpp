// Interfaz gráfica estilo Windows 11: fondo Mica, barra de título integrada y controles
// Fluent (cuadros de texto, botones, interruptores, desplegables...) dibujados con Direct2D
// y DirectWrite. Solo usa DLL del sistema, así que el .exe sigue siendo autocontenido.

#include "gui.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "deps.h"
#include "options.h"
#include "process.h"

namespace {

// ============================================================================ Utilidades

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string U(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring trim(const std::wstring& s) {
    auto b = s.find_first_not_of(L" \t\r\n\"");
    if (b == std::wstring::npos) return {};
    auto e = s.find_last_not_of(L" \t\r\n\"");
    return s.substr(b, e - b + 1);
}

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

D2D1_COLOR_F argb(uint32_t c) {
    return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f);
}

D2D1_COLOR_F withAlpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

bool inside(const D2D1_RECT_F& r, D2D1_POINT_2F p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

DWORD windowsBuild() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static DWORD build = [] {
        OSVERSIONINFOW v{sizeof(v)};
        if (auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&v);
        return v.dwBuildNumber;
    }();
    return build;
}

// ------------------------------------------------------------------- Registro (ajustes)

const wchar_t* kRegKey = L"Software\\YtDownloader";

std::wstring regString(const wchar_t* name, const std::wstring& def) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    return def;
}

DWORD regDword(const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) return v;
    return def;
}

void regSet(const wchar_t* name, const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_SZ, v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

void regSet(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &v, sizeof(v));
}

bool systemUsesDarkTheme() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 0;
}

std::wstring downloadsFolder() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out.empty() ? L"." : out;
}

// ============================================================================ Tema

struct Theme {
    bool dark = true;
    D2D1_COLOR_F solidBg, text, text2, text3, card, cardStroke, ctl, ctlHover, ctlPressed, ctlDisabled, ctlStroke,
        ctlStrokeBottom, strong, inputActive, accent, accentText, accentDisabled, onAccent, onAccentDisabled, subtle,
        subtlePressed, selection, success, error, separator, toggleOffFill;
};

// Paleta de acento del sistema (8 colores RGBA: Light3, Light2, Light1, Accent, Dark1, Dark2, Dark3, -).
bool accentPalette(D2D1_COLOR_F out[7]) {
    BYTE data[32];
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS ||
        size < 28)
        return false;
    for (int i = 0; i < 7; ++i)
        out[i] = D2D1::ColorF(data[i * 4] / 255.f, data[i * 4 + 1] / 255.f, data[i * 4 + 2] / 255.f, 1.f);
    return true;
}

Theme makeTheme(bool dark) {
    Theme t;
    t.dark = dark;
    D2D1_COLOR_F pal[7];
    bool havePal = accentPalette(pal);
    if (dark) {
        t.solidBg = argb(0xFF202020);
        t.text = argb(0xFFFFFFFF);
        t.text2 = argb(0xC8FFFFFF);
        t.text3 = argb(0x5DFFFFFF);
        t.card = argb(0x0DFFFFFF);
        t.cardStroke = argb(0x1A000000);
        t.ctl = argb(0x0FFFFFFF);
        t.ctlHover = argb(0x15FFFFFF);
        t.ctlPressed = argb(0x08FFFFFF);
        t.ctlDisabled = argb(0x0BFFFFFF);
        t.ctlStroke = argb(0x12FFFFFF);
        t.ctlStrokeBottom = argb(0x18FFFFFF);
        t.strong = argb(0x8BFFFFFF);
        t.inputActive = argb(0xB31E1E1E);
        t.accent = havePal ? pal[1] : argb(0xFF60CDFF);
        t.accentText = havePal ? pal[0] : argb(0xFF99EBFF);
        t.accentDisabled = argb(0x28FFFFFF);
        t.onAccent = argb(0xFF000000);
        t.onAccentDisabled = argb(0x87FFFFFF);
        t.subtle = argb(0x0FFFFFFF);
        t.subtlePressed = argb(0x0AFFFFFF);
        t.success = argb(0xFF6CCB5F);
        t.error = argb(0xFFFF99A4);
        t.separator = argb(0x15FFFFFF);
        t.toggleOffFill = argb(0x08FFFFFF);
    } else {
        t.solidBg = argb(0xFFF3F3F3);
        t.text = argb(0xE4000000);
        t.text2 = argb(0x9E000000);
        t.text3 = argb(0x5C000000);
        t.card = argb(0xB3FFFFFF);
        t.cardStroke = argb(0x0F000000);
        t.ctl = argb(0xB3FFFFFF);
        t.ctlHover = argb(0x80F9F9F9);
        t.ctlPressed = argb(0x4DF9F9F9);
        t.ctlDisabled = argb(0x4DF9F9F9);
        t.ctlStroke = argb(0x0F000000);
        t.ctlStrokeBottom = argb(0x29000000);
        t.strong = argb(0x72000000);
        t.inputActive = argb(0xFFFFFFFF);
        t.accent = havePal ? pal[4] : argb(0xFF005FB8);
        t.accentText = havePal ? pal[5] : argb(0xFF003E92);
        t.accentDisabled = argb(0x37000000);
        t.onAccent = argb(0xFFFFFFFF);
        t.onAccentDisabled = argb(0xFFFFFFFF);
        t.subtle = argb(0x09000000);
        t.subtlePressed = argb(0x06000000);
        t.success = argb(0xFF0F7B0F);
        t.error = argb(0xFFC42B1C);
        t.separator = argb(0x0F000000);
        t.toggleOffFill = argb(0x06000000);
    }
    t.selection = havePal ? pal[3] : argb(0xFF0078D4);
    return t;
}

// Glifos de Segoe Fluent Icons / Segoe MDL2 Assets.
namespace glyph {
const wchar_t* Link = L"";
const wchar_t* Paste = L"";
const wchar_t* Video = L"";
const wchar_t* Audio = L"";
const wchar_t* Quality = L"";
const wchar_t* Format = L"";
const wchar_t* List = L"";
const wchar_t* Photo = L"";
const wchar_t* Folder = L"";
const wchar_t* OpenFolder = L"";
const wchar_t* Download = L"";
const wchar_t* Cancel = L"";
const wchar_t* Chevron = L"";
const wchar_t* Info = L"";
const wchar_t* Error = L"";
const wchar_t* Done = L"";
const wchar_t* Sync = L"";
const wchar_t* Log = L"";
const wchar_t* Retry = L"";
}  // namespace glyph

// ============================================================================ Estado global

constexpr UINT WM_APP_EVENT = WM_APP + 1;
constexpr UINT WM_APP_ASK = WM_APP + 2;
constexpr UINT_PTR kTimerCaret = 1;
constexpr UINT_PTR kTimerAnim = 2;
constexpr float kTitleH = 44.f;
constexpr float kMargin = 20.f;
constexpr float kFooterH = 80.f;

struct Widget;

struct Gfx {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    IWICImagingFactory* wic = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1SolidColorBrush* brush2 = nullptr;  // texto seleccionado
    ID2D1Bitmap* icon = nullptr;
    IDWriteTextFormat* fBody = nullptr;        // 14, izquierda, centrado vertical, con elipsis
    IDWriteTextFormat* fBodyStrong = nullptr;  // 14 semibold
    IDWriteTextFormat* fBodyCenter = nullptr;  // 14 centrado
    IDWriteTextFormat* fBodyRight = nullptr;   // 14 alineado a la derecha
    IDWriteTextFormat* fCaption = nullptr;     // 12
    IDWriteTextFormat* fInput = nullptr;       // 14, sin elipsis (cuadros de texto)
    IDWriteTextFormat* fSubtitle = nullptr;    // 20 semibold
    IDWriteTextFormat* fIcon = nullptr;        // 16 iconos
    IDWriteTextFormat* fIconBig = nullptr;     // 20 iconos
    IDWriteTextFormat* fIconSmall = nullptr;   // 12 iconos
};

enum class Phase { Preparing, Ready, Downloading, Done, Failed, ToolsFailed };

struct App {
    HWND hwnd = nullptr;
    HINSTANCE inst = nullptr;
    Gfx g;
    Theme th;
    UINT dpi = 96;
    bool mica = false;
    bool customFrame = false;
    bool active = true;
    float width = 0, height = 0;  // en DIP

    // Scroll del contenido central
    float scroll = 0, contentH = 0;
    D2D1_RECT_F viewport{};

    std::vector<Widget*> widgets;
    Widget* hot = nullptr;
    Widget* captured = nullptr;
    Widget* focus = nullptr;
    bool caretOn = true;
    bool trackingMouse = false;

    // Animaciones
    double animStart = 0;
    float progressShown = 0;

    // Opciones
    Mode mode = Mode::Video;
    std::string videoQuality = "best", audioQuality = "best", videoFormat = "mp4", audioFormat = "mp3";
    bool playlist = false, thumbnail = true;

    // Trabajo en segundo plano
    Phase phase = Phase::Preparing;
    std::wstring statusTitle, statusDetail;
    float percent = -1;  // <0 = indeterminado
    Tools tools;
    bool toolsReady = false;
    std::thread worker;
    std::atomic<bool> busy{false};
    std::shared_ptr<proc::CancelToken> cancel;
    std::deque<std::wstring> log;
    std::wstring lastFile;
    std::wstring lastOutputDir;
};

App A;

float px2dip(float v) { return v * 96.f / A.dpi; }
float dip2px(float v) { return v * A.dpi / 96.f; }

void invalidate() {
    if (A.hwnd) InvalidateRect(A.hwnd, nullptr, FALSE);
}

double nowSeconds() { return GetTickCount64() / 1000.0; }

void addLog(const std::wstring& line) {
    A.log.push_back(line);
    while (A.log.size() > 400) A.log.pop_front();
}

// ------------------------------------------------------------------- Primitivas de dibujo

void setColor(D2D1_COLOR_F c) { A.g.brush->SetColor(c); }

void fillRR(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    setColor(c);
    A.g.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), A.g.brush);
}

void strokeRR(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1.f) {
    setColor(c);
    float h = w / 2;
    A.g.rt->DrawRoundedRectangle(D2D1::RoundedRect(R(r.left + h, r.top + h, r.right - h, r.bottom - h), rad, rad),
                                 A.g.brush, w);
}

void fillRect(D2D1_RECT_F r, D2D1_COLOR_F c) {
    setColor(c);
    A.g.rt->FillRectangle(r, A.g.brush);
}

void text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* f, D2D1_COLOR_F c) {
    if (s.empty()) return;
    setColor(c);
    A.g.rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, A.g.brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float textWidth(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    float w = 0;
    if (SUCCEEDED(A.g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000.f, 100.f, &l))) {
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        w = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    return w;
}

// Borde de control Fluent: trazo fino con la parte inferior algo más marcada.
void controlBorder(D2D1_RECT_F r, float rad, D2D1_COLOR_F stroke, D2D1_COLOR_F bottom) {
    strokeRR(r, rad, stroke);
    setColor(bottom);
    A.g.rt->DrawLine(D2D1::Point2F(r.left + rad, r.bottom - 0.5f), D2D1::Point2F(r.right - rad, r.bottom - 0.5f),
                     A.g.brush, 1.f);
}

// ============================================================================ Widgets

struct Widget {
    D2D1_RECT_F rc{};
    bool visible = true;
    bool enabled = true;
    bool inContent = true;  // se desplaza con el scroll
    bool pressed = false;

    virtual ~Widget() = default;
    virtual void draw() = 0;
    virtual bool focusable() const { return false; }
    virtual void mouseDown(D2D1_POINT_2F) { pressed = true; }
    virtual void mouseMove(D2D1_POINT_2F) {}
    virtual void mouseUp(D2D1_POINT_2F p, bool over) {
        bool was = pressed;
        pressed = false;
        if (was && over) click(p);
    }
    virtual void doubleClick(D2D1_POINT_2F p) { mouseDown(p); }
    virtual void click(D2D1_POINT_2F) {}
    virtual void key(WPARAM) {}
    virtual void chr(wchar_t) {}
    virtual void focusChanged(bool) {}
    virtual void contextMenu(POINT) {}
    virtual bool animating() const { return false; }
    virtual LPCWSTR cursor() const { return IDC_ARROW; }
    bool isHot() const { return A.hot == this; }
    // Rectángulo en coordenadas de ventana (DIP).
    D2D1_RECT_F screenRc() const {
        return inContent ? R(rc.left, rc.top - A.scroll, rc.right, rc.bottom - A.scroll) : rc;
    }
};

// ------------------------------------------------------------------- Botón

struct Button : Widget {
    std::wstring label;
    const wchar_t* icon = nullptr;
    bool accent = false;
    std::function<void()> onClick;

    void draw() override {
        const Theme& t = A.th;
        bool hot = isHot() && enabled;
        D2D1_COLOR_F fg;
        if (accent) {
            D2D1_COLOR_F fill = !enabled          ? t.accentDisabled
                                : (pressed && hot) ? withAlpha(t.accent, 0.8f)
                                : hot              ? withAlpha(t.accent, 0.9f)
                                                   : t.accent;
            fillRR(rc, 4, fill);
            if (enabled) controlBorder(rc, 4, argb(t.dark ? 0x14FFFFFF : 0x14FFFFFF), argb(t.dark ? 0x24000000 : 0x66000000));
            fg = !enabled ? t.onAccentDisabled : (pressed && hot) ? withAlpha(t.onAccent, 0.7f) : t.onAccent;
        } else {
            D2D1_COLOR_F fill = !enabled ? t.ctlDisabled : (pressed && hot) ? t.ctlPressed : hot ? t.ctlHover : t.ctl;
            fillRR(rc, 4, fill);
            controlBorder(rc, 4, t.ctlStroke, (pressed && hot) || !enabled ? t.ctlStroke : t.ctlStrokeBottom);
            fg = !enabled ? t.text3 : (pressed && hot) ? t.text2 : t.text;
        }
        float iconW = icon ? 16.f : 0.f;
        float gap = icon && !label.empty() ? 8.f : 0.f;
        float tw = label.empty() ? 0.f : textWidth(label, A.g.fBodyCenter);
        float total = iconW + gap + tw;
        float x = (rc.left + rc.right - total) / 2;
        if (icon) text(icon, R(x, rc.top, x + iconW, rc.bottom), A.g.fIcon, fg);
        if (!label.empty())
            text(label, R(x + iconW + gap - 1, rc.top, x + total + 1, rc.bottom - 1), A.g.fBodyCenter, fg);
    }
    void click(D2D1_POINT_2F) override {
        if (onClick) onClick();
    }
};

// ------------------------------------------------------------------- Interruptor (ToggleSwitch)

struct Toggle : Widget {
    bool on = false;
    float anim = 0;  // 0 = apagado, 1 = encendido
    std::function<void(bool)> onChange;

    D2D1_RECT_F track() const { return R(rc.right - 40, (rc.top + rc.bottom) / 2 - 10, rc.right, (rc.top + rc.bottom) / 2 + 10); }

    void draw() override {
        const Theme& t = A.th;
        bool hot = isHot() && enabled;
        float target = on ? 1.f : 0.f;
        anim += (target - anim) * 0.35f;
        if (std::fabs(anim - target) < 0.01f) anim = target;

        text(on ? L"Activado" : L"Desactivado", R(rc.left, rc.top, rc.right - 52, rc.bottom - 1), A.g.fBodyRight,
             enabled ? t.text : t.text3);

        D2D1_RECT_F tr = track();
        if (on) {
            D2D1_COLOR_F fill = !enabled ? t.accentDisabled : pressed ? withAlpha(t.accent, 0.8f)
                                : hot                         ? withAlpha(t.accent, 0.9f)
                                                              : t.accent;
            fillRR(tr, 10, fill);
        } else {
            fillRR(tr, 10, hot ? t.ctlHover : t.toggleOffFill);
            strokeRR(tr, 10, enabled ? t.strong : t.text3);
        }
        float knob = pressed && enabled ? 14.f : hot ? 14.f : 12.f;
        float x0 = tr.left + 10, x1 = tr.right - 10;
        float cx = x0 + (x1 - x0) * anim;
        if (pressed && enabled) cx += on ? -2.f : 2.f;
        D2D1_COLOR_F kc = on ? (enabled ? t.onAccent : t.onAccentDisabled) : (enabled ? t.text2 : t.text3);
        setColor(kc);
        A.g.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, (tr.top + tr.bottom) / 2), knob / 2, knob / 2), A.g.brush);
    }
    bool animating() const override { return anim != (on ? 1.f : 0.f); }
    void click(D2D1_POINT_2F) override {
        on = !on;
        if (onChange) onChange(on);
    }
};

// ------------------------------------------------------------------- Selector segmentado

struct Segmented : Widget {
    std::vector<std::pair<const wchar_t*, std::wstring>> items;  // icono, texto
    int sel = 0;
    int hotIdx = -1, pressIdx = -1;
    std::function<void(int)> onChange;

    D2D1_RECT_F itemRect(int i) const {
        float w = (rc.right - rc.left - 4) / items.size();
        return R(rc.left + 2 + w * i, rc.top + 2, rc.left + 2 + w * (i + 1), rc.bottom - 2);
    }
    int indexAt(D2D1_POINT_2F p) const {
        for (int i = 0; i < (int)items.size(); ++i)
            if (inside(itemRect(i), p)) return i;
        return -1;
    }
    void draw() override {
        const Theme& t = A.th;
        fillRR(rc, 5, t.ctl);
        controlBorder(rc, 5, t.ctlStroke, t.ctlStrokeBottom);
        for (int i = 0; i < (int)items.size(); ++i) {
            D2D1_RECT_F r = itemRect(i);
            bool selected = i == sel;
            D2D1_COLOR_F fg = t.text;
            if (selected) {
                fillRR(r, 4, t.accent);
                fg = t.onAccent;
            } else if (isHot() && hotIdx == i) {
                fillRR(r, 4, pressIdx == i ? t.subtlePressed : t.subtle);
                fg = pressIdx == i ? t.text2 : t.text;
            }
            float tw = textWidth(items[i].second, A.g.fBodyCenter);
            float total = 16 + 8 + tw;
            float x = (r.left + r.right - total) / 2;
            text(items[i].first, R(x, r.top, x + 16, r.bottom), A.g.fIcon, fg);
            text(items[i].second, R(x + 24, r.top, x + total + 1, r.bottom - 1), A.g.fBodyCenter, fg);
        }
    }
    void mouseMove(D2D1_POINT_2F p) override {
        int h = indexAt(p);
        if (h != hotIdx) {
            hotIdx = h;
            invalidate();
        }
    }
    void mouseDown(D2D1_POINT_2F p) override {
        Widget::mouseDown(p);
        pressIdx = indexAt(p);
    }
    void mouseUp(D2D1_POINT_2F p, bool over) override {
        int i = indexAt(p);
        if (pressed && over && i >= 0 && i == pressIdx && i != sel) {
            sel = i;
            if (onChange) onChange(i);
        }
        pressed = false;
        pressIdx = -1;
    }
};

// ------------------------------------------------------------------- Desplegable (ComboBox)

struct Dropdown : Widget {
    std::vector<std::wstring> items;
    int sel = 0;
    std::function<void(int)> onChange;

    void draw() override {
        const Theme& t = A.th;
        bool hot = isHot() && enabled;
        fillRR(rc, 4, !enabled ? t.ctlDisabled : pressed ? t.ctlPressed : hot ? t.ctlHover : t.ctl);
        controlBorder(rc, 4, t.ctlStroke, pressed ? t.ctlStroke : t.ctlStrokeBottom);
        D2D1_COLOR_F fg = enabled ? t.text : t.text3;
        if (sel >= 0 && sel < (int)items.size())
            text(items[sel], R(rc.left + 11, rc.top, rc.right - 34, rc.bottom - 1), A.g.fBody, fg);
        float dy = pressed ? 1.f : 0.f;
        text(glyph::Chevron, R(rc.right - 30, rc.top + dy, rc.right - 12, rc.bottom + dy), A.g.fIconSmall,
             enabled ? t.text2 : t.text3);
    }
    void click(D2D1_POINT_2F) override;
};

// ------------------------------------------------------------------- Cuadro de texto

struct TextBox : Widget {
    std::wstring value;
    std::wstring placeholder;
    size_t caret = 0, anchor = 0;
    float scrollX = 0;
    bool selecting = false;
    std::function<void()> onEnter;
    std::function<void()> onChange;

    static constexpr float kPad = 11.f;

    bool focusable() const override { return true; }
    LPCWSTR cursor() const override { return IDC_IBEAM; }
    bool focused() const { return A.focus == this; }

    D2D1_RECT_F textArea() const { return R(rc.left + kPad, rc.top, rc.right - kPad, rc.bottom); }

    IDWriteTextLayout* layout() const {
        IDWriteTextLayout* l = nullptr;
        A.g.dw->CreateTextLayout(value.c_str(), (UINT32)value.size(), A.g.fInput, 100000.f, rc.bottom - rc.top, &l);
        return l;
    }

    float caretX(IDWriteTextLayout* l, size_t pos) const {
        float x = 0, y = 0;
        DWRITE_HIT_TEST_METRICS m{};
        l->HitTestTextPosition((UINT32)pos, FALSE, &x, &y, &m);
        return x;
    }

    void ensureCaretVisible() {
        IDWriteTextLayout* l = layout();
        if (!l) return;
        float cx = caretX(l, caret);
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        l->Release();
        float visW = textArea().right - textArea().left;
        if (visW <= 0) {
            scrollX = 0;
            return;
        }
        if (cx - scrollX > visW - 2) scrollX = cx - visW + 2;
        if (cx - scrollX < 0) scrollX = cx;
        float maxScroll = std::max(0.f, m.widthIncludingTrailingWhitespace - visW + 2);
        scrollX = std::clamp(scrollX, 0.f, maxScroll);
    }

    size_t posAt(D2D1_POINT_2F p) const {
        IDWriteTextLayout* l = layout();
        if (!l) return 0;
        BOOL trailing = FALSE, isInside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        l->HitTestPoint(p.x - textArea().left + scrollX, (rc.bottom - rc.top) / 2, &trailing, &isInside, &m);
        l->Release();
        return std::min(value.size(), (size_t)(m.textPosition + (trailing ? m.length : 0)));
    }

    // Sin foco se muestra el principio del texto; con foco, la zona del cursor.
    void relayout() {
        if (focused()) ensureCaretVisible();
        else scrollX = 0;
    }

    bool hasSelection() const { return caret != anchor; }
    size_t selStart() const { return std::min(caret, anchor); }
    size_t selEnd() const { return std::max(caret, anchor); }

    void setText(const std::wstring& s) {
        value = s;
        caret = anchor = value.size();
        scrollX = 0;
        relayout();
    }

    void replaceSelection(const std::wstring& s) {
        std::wstring clean;
        for (wchar_t c : s)
            if (c != L'\r' && c != L'\n' && c != L'\t') clean += c;
        value.replace(selStart(), selEnd() - selStart(), clean);
        caret = anchor = selStart() + clean.size();
        ensureCaretVisible();
        if (onChange) onChange();
    }

    void copy() const {
        if (!hasSelection()) return;
        std::wstring s = value.substr(selStart(), selEnd() - selStart());
        if (!OpenClipboard(A.hwnd)) return;
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
        if (h) {
            memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }

    static std::wstring clipboardText() {
        std::wstring out;
        if (!OpenClipboard(A.hwnd)) return out;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                out = p;
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return out;
    }

    void paste() { replaceSelection(trim(clipboardText())); }

    void selectAll() {
        anchor = 0;
        caret = value.size();
        ensureCaretVisible();
    }

    static bool isWord(wchar_t c) { return iswalnum(c) || c == L'_'; }

    size_t wordLeft(size_t p) const {
        while (p > 0 && !isWord(value[p - 1])) --p;
        while (p > 0 && isWord(value[p - 1])) --p;
        return p;
    }
    size_t wordRight(size_t p) const {
        while (p < value.size() && isWord(value[p])) ++p;
        while (p < value.size() && !isWord(value[p])) ++p;
        return p;
    }

    void draw() override {
        const Theme& t = A.th;
        bool hot = isHot();
        bool f = focused();
        fillRR(rc, 4, f ? t.inputActive : hot ? t.ctlHover : t.ctl);
        strokeRR(rc, 4, t.ctlStroke);
        setColor(f ? t.accent : t.strong);
        float bw = f ? 2.f : 1.f;
        A.g.rt->DrawLine(D2D1::Point2F(rc.left + 3, rc.bottom - bw / 2), D2D1::Point2F(rc.right - 3, rc.bottom - bw / 2),
                         A.g.brush, bw);

        D2D1_RECT_F ta = textArea();
        A.g.rt->PushAxisAlignedClip(ta, D2D1_ANTIALIAS_MODE_ALIASED);
        if (value.empty()) {
            text(placeholder, R(ta.left, ta.top, ta.right, ta.bottom - 1), A.g.fBody, t.text2);
        }
        IDWriteTextLayout* l = layout();
        if (l) {
            float ox = ta.left - scrollX, oy = rc.top - 1;
            if (hasSelection() && f) {
                UINT32 count = 0;
                l->HitTestTextRange((UINT32)selStart(), (UINT32)(selEnd() - selStart()), ox, oy, nullptr, 0, &count);
                std::vector<DWRITE_HIT_TEST_METRICS> ms(count);
                if (count &&
                    SUCCEEDED(l->HitTestTextRange((UINT32)selStart(), (UINT32)(selEnd() - selStart()), ox, oy,
                                                  ms.data(), count, &count))) {
                    for (auto& m : ms) fillRect(R(m.left, rc.top + 6, m.left + m.width, rc.bottom - 6), t.selection);
                }
                A.g.brush2->SetColor(argb(0xFFFFFFFF));
                l->SetDrawingEffect(A.g.brush2, DWRITE_TEXT_RANGE{(UINT32)selStart(), (UINT32)(selEnd() - selStart())});
            }
            setColor(t.text);
            A.g.rt->DrawTextLayout(D2D1::Point2F(ox, oy), l, A.g.brush);
            if (f && A.caretOn && A.active) {
                float cx = std::floor(ox + caretX(l, caret)) + 0.5f;
                setColor(t.text);
                A.g.rt->DrawLine(D2D1::Point2F(cx, rc.top + 8), D2D1::Point2F(cx, rc.bottom - 8), A.g.brush, 1.f);
            }
            l->Release();
        }
        A.g.rt->PopAxisAlignedClip();
    }

    void mouseDown(D2D1_POINT_2F p) override {
        caret = posAt(p);
        if (!(GetKeyState(VK_SHIFT) & 0x8000)) anchor = caret;
        selecting = true;
        A.caretOn = true;
        ensureCaretVisible();
        invalidate();
    }
    void doubleClick(D2D1_POINT_2F) override {
        selectAll();
        selecting = false;
        invalidate();
    }
    void mouseMove(D2D1_POINT_2F p) override {
        if (!selecting) return;
        caret = posAt(p);
        ensureCaretVisible();
        invalidate();
    }
    void mouseUp(D2D1_POINT_2F, bool) override { selecting = false; }

    void key(WPARAM vk) override {
        bool shift = GetKeyState(VK_SHIFT) & 0x8000;
        bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        size_t old = caret;
        switch (vk) {
            case VK_LEFT:
                if (hasSelection() && !shift) caret = selStart();
                else if (caret > 0) caret = ctrl ? wordLeft(caret) : caret - 1;
                break;
            case VK_RIGHT:
                if (hasSelection() && !shift) caret = selEnd();
                else if (caret < value.size()) caret = ctrl ? wordRight(caret) : caret + 1;
                break;
            case VK_HOME: caret = 0; break;
            case VK_END: caret = value.size(); break;
            case VK_DELETE:
                if (!hasSelection() && caret < value.size()) anchor = ctrl ? wordRight(caret) : caret + 1;
                if (hasSelection()) {
                    if (shift) copy();
                    replaceSelection(L"");
                }
                A.caretOn = true;
                invalidate();
                return;
            case VK_INSERT:
                if (ctrl) copy();
                else if (shift) paste();
                invalidate();
                return;
            default: return;
        }
        (void)old;
        if (!shift) anchor = caret;
        A.caretOn = true;
        ensureCaretVisible();
        invalidate();
    }

    void chr(wchar_t c) override {
        switch (c) {
            case 1: selectAll(); break;     // Ctrl+A
            case 3: copy(); break;          // Ctrl+C
            case 22: paste(); break;        // Ctrl+V
            case 24:                        // Ctrl+X
                copy();
                replaceSelection(L"");
                break;
            case 8:  // Retroceso
                if (!hasSelection() && caret > 0)
                    anchor = (GetKeyState(VK_CONTROL) & 0x8000) ? wordLeft(caret) : caret - 1;
                if (hasSelection()) replaceSelection(L"");
                break;
            case 127:  // Ctrl+Retroceso
                if (!hasSelection()) anchor = wordLeft(caret);
                replaceSelection(L"");
                break;
            case 13:
                if (onEnter) onEnter();
                break;
            default:
                if (c >= 32) replaceSelection(std::wstring(1, c));
                break;
        }
        A.caretOn = true;
        invalidate();
    }

    void contextMenu(POINT pt) override {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | (hasSelection() ? 0 : MF_GRAYED), 1, L"Cortar\tCtrl+X");
        AppendMenuW(m, MF_STRING | (hasSelection() ? 0 : MF_GRAYED), 2, L"Copiar\tCtrl+C");
        AppendMenuW(m, MF_STRING | (IsClipboardFormatAvailable(CF_UNICODETEXT) ? 0 : MF_GRAYED), 3, L"Pegar\tCtrl+V");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (value.empty() ? MF_GRAYED : 0), 4, L"Seleccionar todo\tCtrl+A");
        int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, A.hwnd, nullptr);
        DestroyMenu(m);
        switch (cmd) {
            case 1: copy(); replaceSelection(L""); break;
            case 2: copy(); break;
            case 3: paste(); break;
            case 4: selectAll(); break;
        }
        invalidate();
    }

    void focusChanged(bool gained) override {
        if (gained) {
            A.caretOn = true;
        } else {
            anchor = caret;
            selecting = false;
            scrollX = 0;
        }
        invalidate();
    }
};

void Dropdown::click(D2D1_POINT_2F) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == sel ? MF_CHECKED : 0), i + 1, items[i].c_str());
    D2D1_RECT_F r = screenRc();
    POINT pt{(LONG)dip2px(r.left), (LONG)dip2px(r.bottom + 2)};
    ClientToScreen(A.hwnd, &pt);
    TPMPARAMS tp{sizeof(tp)};
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, A.hwnd, &tp);
    DestroyMenu(m);
    if (cmd > 0 && cmd - 1 != sel) {
        sel = cmd - 1;
        if (onChange) onChange(sel);
    }
    invalidate();
}

// ============================================================================ Controles concretos

TextBox urlBox, dirBox;
Button pasteBtn, browseBtn, openDirBtn, mainBtn, secondaryBtn;
Segmented modeSeg;
Dropdown qualityDd, formatDd;
Toggle playlistTg, thumbTg;

// Tarjetas tipo "SettingsCard" (fila con icono, título, descripción y control a la derecha).
struct Card {
    D2D1_RECT_F rc{};
    const wchar_t* icon = nullptr;
    std::wstring title, desc;
    bool enabled = true;
};

Card urlCard, modeCard, qualityCard, formatCard, playlistCard, thumbCard, dirCard;
std::vector<Card*> cards = {&urlCard, &modeCard, &qualityCard, &formatCard, &playlistCard, &thumbCard, &dirCard};
D2D1_RECT_F sectionOptions{}, sectionDest{};
D2D1_RECT_F footerRc{};

std::wstring qualityLabel(const std::string& v, Mode m) {
    if (m == Mode::Video) {
        if (v == "best") return L"La mejor disponible";
        if (v == "worst") return L"La más ligera";
        if (v == "2160") return L"2160p (4K)";
        if (v == "1440") return L"1440p (2K)";
        if (v == "1080") return L"1080p (Full HD)";
        if (v == "720") return L"720p (HD)";
        return W(v) + L"p";
    }
    if (v == "best") return L"La mejor (VBR)";
    return W(v) + L" kbps";
}

std::wstring formatLabel(const std::string& v) {
    std::wstring s = W(v);
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

void persist() {
    regSet(L"Mode", A.mode == Mode::Audio ? 1u : 0u);
    regSet(L"VideoQuality", W(A.videoQuality));
    regSet(L"AudioQuality", W(A.audioQuality));
    regSet(L"VideoFormat", W(A.videoFormat));
    regSet(L"AudioFormat", W(A.audioFormat));
    regSet(L"Thumbnail", A.thumbnail ? 1u : 0u);
    regSet(L"OutputDir", dirBox.value);
}

void refreshOptions() {
    bool video = A.mode == Mode::Video;
    const auto& qs = video ? videoQualities() : audioQualities();
    const auto& fs = video ? videoFormats() : audioFormats();
    const std::string& q = video ? A.videoQuality : A.audioQuality;
    const std::string& f = video ? A.videoFormat : A.audioFormat;
    qualityDd.items.clear();
    qualityDd.sel = 0;
    for (size_t i = 0; i < qs.size(); ++i) {
        qualityDd.items.push_back(qualityLabel(qs[i], A.mode));
        if (qs[i] == q) qualityDd.sel = (int)i;
    }
    formatDd.items.clear();
    formatDd.sel = 0;
    for (size_t i = 0; i < fs.size(); ++i) {
        formatDd.items.push_back(formatLabel(fs[i]));
        if (fs[i] == f) formatDd.sel = (int)i;
    }
    modeCard.icon = video ? glyph::Video : glyph::Audio;
    qualityCard.desc = video ? L"Resolución máxima del vídeo" : L"Bitrate del audio";
    formatCard.desc = video ? L"Formato del archivo de vídeo" : L"Formato del archivo de audio";
    bool thumbOk = !video && f == "mp3";
    thumbCard.enabled = thumbTg.enabled = thumbOk;
    thumbTg.on = thumbOk && A.thumbnail;
    thumbTg.anim = thumbTg.on ? 1.f : 0.f;
    thumbCard.desc = thumbOk ? L"Usa la miniatura del vídeo como portada del MP3"
                             : L"Solo disponible al descargar audio en MP3";
    invalidate();
}

// ============================================================================ Estado / trabajo en segundo plano

void setStatus(Phase p, const std::wstring& title, const std::wstring& detail, float pct = -1) {
    A.phase = p;
    A.statusTitle = title;
    A.statusDetail = detail;
    A.percent = pct;
    if (pct < 0) A.progressShown = 0;
}

struct Event {
    enum Kind { Log, Progress, ToolsOk, ToolsFail, Finished } kind = Log;
    std::wstring title, detail, file;
    float percent = -1;
    int code = 0;
    Tools tools;
};

void post(Event* e) {
    if (!A.hwnd || !PostMessageW(A.hwnd, WM_APP_EVENT, 0, (LPARAM)e)) delete e;
}

void updateButtons();

void joinWorker() {
    if (A.worker.joinable()) A.worker.join();
}

void startPrepare() {
    joinWorker();
    setStatus(Phase::Preparing, L"Preparando youtube-dl y ffmpeg…", L"Comprobando los componentes necesarios");
    updateButtons();
    A.busy = true;
    HWND hwnd = A.hwnd;
    A.worker = std::thread([hwnd] {
        DepsOptions d;
        d.binDir = defaultBinDir();
        d.autoInstall = false;
        d.log = [](const std::string& m) {
            auto* e = new Event;
            e->kind = Event::Log;
            e->detail = W(m);
            post(e);
        };
        d.ask = [hwnd](const std::string& q) {
            std::wstring wq = W(q);
            return SendMessageW(hwnd, WM_APP_ASK, 0, (LPARAM)&wq) == IDYES;
        };
        auto* e = new Event;
        try {
            e->tools = ensureTools(d);
            e->kind = Event::ToolsOk;
        } catch (const std::exception& ex) {
            e->kind = Event::ToolsFail;
            e->detail = W(ex.what());
        }
        A.busy = false;
        post(e);
    });
}

// Interpreta una línea de youtube-dl y la traduce a un evento de progreso.
struct ProgressParser {
    std::wstring file;
    int item = 0, items = 0;
    std::wstring lastError;

    static std::wstring baseName(const std::wstring& p) {
        auto s = p.find_last_of(L"\\/");
        std::wstring n = s == std::wstring::npos ? p : p.substr(s + 1);
        if (!n.empty() && n.back() == L'"') n.pop_back();
        if (!n.empty() && n.front() == L'"') n.erase(0, 1);
        return n;
    }

    std::wstring prefix() const {
        return items > 1 ? L"(" + std::to_wstring(item) + L" de " + std::to_wstring(items) + L") " : L"";
    }

    Event* parse(const std::string& raw) {
        static const std::wregex rePct(LR"(^\[download\]\s+([\d.]+)%\s+of\s+~?\s*([\d.]+\s*\w+)(?:\s+at\s+(\S+))?(?:\s+ETA\s+(\S+))?)");
        static const std::wregex reItem(LR"(^\[download\] Downloading (?:video|item) (\d+) of (\d+))");
        std::wstring line = W(raw);
        std::wsmatch m;
        auto ev = [&](const std::wstring& title, float pct, const std::wstring& detail) {
            auto* e = new Event;
            e->kind = Event::Progress;
            e->title = prefix() + title;
            e->percent = pct;
            e->detail = detail;
            e->file = file;
            return e;
        };
        if (line.rfind(L"ERROR:", 0) == 0) {
            lastError = trim(line.substr(6));
            return nullptr;
        }
        if (std::regex_search(line, m, reItem)) {
            item = std::stoi(m[1].str());
            items = std::stoi(m[2].str());
            return ev(L"Obteniendo información…", -1, L"");
        }
        if (line.rfind(L"[download] Destination:", 0) == 0) {
            file = baseName(trim(line.substr(23)));
            return ev(L"Descargando…", 0, file);
        }
        if (std::regex_search(line, m, rePct)) {
            float pct = std::stof(m[1].str());
            std::wstring pctTxt = m[1].str();
            std::replace(pctTxt.begin(), pctTxt.end(), L'.', L',');
            std::wstring detail = pctTxt + L" % de " + m[2].str();
            if (m[3].matched && m[3].str() != L"Unknown") detail += L"  ·  " + m[3].str();
            if (m[4].matched && m[4].str() != L"Unknown") detail += L"  ·  quedan " + m[4].str();
            return ev(file.empty() ? L"Descargando…" : file, pct, detail);
        }
        if (line.find(L"has already been downloaded") != std::wstring::npos)
            return ev(L"Ya estaba descargado", 100, file);
        if (line.rfind(L"[ffmpeg] Merging formats into", 0) == 0) {
            file = baseName(trim(line.substr(29)));
            return ev(L"Uniendo vídeo y audio…", -1, file);
        }
        if (line.rfind(L"[ExtractAudio]", 0) == 0) {
            auto p = line.find(L"Destination:");
            if (p != std::wstring::npos) file = baseName(trim(line.substr(p + 12)));
            return ev(L"Convirtiendo audio…", -1, file);
        }
        if (line.rfind(L"[VideoConvertor]", 0) == 0 || line.find(L"Converting video") != std::wstring::npos)
            return ev(L"Convirtiendo vídeo…", -1, file);
        if (line.rfind(L"[EmbedThumbnail]", 0) == 0) return ev(L"Añadiendo carátula…", -1, file);
        if (line.find(L"Adding metadata") != std::wstring::npos) return ev(L"Añadiendo metadatos…", -1, file);
        if (line.rfind(L"[youtube]", 0) == 0 || line.rfind(L"[youtube:", 0) == 0 || line.rfind(L"[generic]", 0) == 0)
            return ev(L"Obteniendo información…", -1, L"");
        return nullptr;
    }
};

void startDownload() {
    std::wstring url = trim(urlBox.value);
    if (url.empty()) {
        setStatus(Phase::Failed, L"Falta el enlace", L"Pega el enlace de un vídeo o una lista de YouTube");
        A.focus = &urlBox;
        updateButtons();
        invalidate();
        return;
    }
    if (!A.toolsReady) {
        startPrepare();
        return;
    }
    std::wstring dir = trim(dirBox.value);
    if (dir.empty()) {
        dir = downloadsFolder();
        dirBox.setText(dir);
    }
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);

    DownloadOptions o;
    o.urls = {U(url)};
    o.mode = A.mode;
    o.quality = A.mode == Mode::Video ? A.videoQuality : A.audioQuality;
    o.format = A.mode == Mode::Video ? A.videoFormat : A.audioFormat;
    o.outputDir = U(dir);
    o.playlist = A.playlist;
    o.embedThumbnail = A.mode == Mode::Audio && A.audioFormat == "mp3" && A.thumbnail;
    std::string err = validate(o);
    if (!err.empty()) {
        setStatus(Phase::Failed, L"Opciones no válidas", W(err));
        updateButtons();
        invalidate();
        return;
    }
    persist();

    std::vector<std::string> warnings;
    auto args = A.tools.ytdl;
    auto extra = buildYtdlArgs(o, proc::toUtf8(A.tools.ffmpegDir), A.tools.haveFfmpeg, warnings);
    args.insert(args.end(), extra.begin(), extra.end());
    args.push_back("--newline");
    args.push_back("--");
    args.insert(args.end(), o.urls.begin(), o.urls.end());

    A.log.clear();
    for (const auto& w : warnings) addLog(L"Aviso: " + W(w));
    A.lastOutputDir = dir;
    A.lastFile.clear();
    setStatus(Phase::Downloading, L"Obteniendo información…", warnings.empty() ? L"" : W(warnings.front()));
    updateButtons();
    invalidate();

    joinWorker();
    A.cancel = std::make_shared<proc::CancelToken>();
    auto cancel = A.cancel;
    A.busy = true;
    A.worker = std::thread([args, cancel] {
        ProgressParser parser;
        int rc = proc::runCapture(
            args,
            [&](const std::string& line) {
                auto* lg = new Event;
                lg->kind = Event::Log;
                lg->detail = W(line);
                lg->code = 1;  // solo registro
                post(lg);
                if (Event* e = parser.parse(line)) post(e);
            },
            cancel.get());
        auto* e = new Event;
        e->kind = Event::Finished;
        e->code = rc;
        e->file = parser.file;
        e->detail = parser.lastError;
        A.busy = false;
        post(e);
    });
}

void showLog() {
    std::wstring s;
    size_t start = A.log.size() > 30 ? A.log.size() - 30 : 0;
    for (size_t i = start; i < A.log.size(); ++i) s += A.log[i] + L"\n";
    if (s.empty()) s = L"(vacío)";
    MessageBoxW(A.hwnd, s.c_str(), L"Registro de youtube-dl", MB_OK | MB_ICONINFORMATION);
}

void openFolder(const std::wstring& dir) {
    if (dir.empty()) return;
    ShellExecuteW(A.hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void updateButtons() {
    mainBtn.enabled = true;
    secondaryBtn.visible = false;
    switch (A.phase) {
        case Phase::Preparing:
            mainBtn.label = L"Descargar";
            mainBtn.icon = glyph::Download;
            mainBtn.accent = true;
            mainBtn.enabled = false;
            break;
        case Phase::Downloading:
            mainBtn.label = L"Cancelar";
            mainBtn.icon = glyph::Cancel;
            mainBtn.accent = false;
            break;
        case Phase::ToolsFailed:
            mainBtn.label = L"Reintentar";
            mainBtn.icon = glyph::Retry;
            mainBtn.accent = true;
            break;
        default:
            mainBtn.label = L"Descargar";
            mainBtn.icon = glyph::Download;
            mainBtn.accent = true;
            break;
    }
    if (A.phase == Phase::Done) {
        secondaryBtn.visible = true;
        secondaryBtn.label = L"Abrir carpeta";
        secondaryBtn.icon = glyph::OpenFolder;
        secondaryBtn.onClick = [] { openFolder(A.lastOutputDir); };
    } else if (A.phase == Phase::Failed && !A.log.empty()) {
        secondaryBtn.visible = true;
        secondaryBtn.label = L"Ver registro";
        secondaryBtn.icon = glyph::Log;
        secondaryBtn.onClick = showLog;
    }
}

void handleEvent(Event* e) {
    switch (e->kind) {
        case Event::Log:
            if (e->code == 1) {
                addLog(e->detail);
            } else if (A.phase == Phase::Preparing) {
                std::wstring d = trim(e->detail);
                if (d.rfind(L"-> ", 0) == 0) d = L"Descargando " + d.substr(3);
                A.statusDetail = d;
            }
            break;
        case Event::Progress:
            if (A.phase != Phase::Downloading) break;
            A.statusTitle = e->title;
            A.statusDetail = e->detail;
            if (e->percent < 0 || A.percent < 0 || e->percent < A.percent - 5) A.progressShown = std::max(0.f, e->percent);
            A.percent = e->percent;
            if (!e->file.empty()) A.lastFile = e->file;
            break;
        case Event::ToolsOk:
            A.tools = e->tools;
            A.toolsReady = true;
            if (A.tools.haveFfmpeg)
                setStatus(Phase::Ready, L"Listo para descargar", L"Pega un enlace de YouTube y pulsa Descargar");
            else
                setStatus(Phase::Ready, L"Listo (sin ffmpeg)",
                          L"Sin ffmpeg la calidad queda limitada y no se convertirán formatos");
            break;
        case Event::ToolsFail:
            setStatus(Phase::ToolsFailed, L"No se pudo preparar youtube-dl", e->detail);
            break;
        case Event::Finished:
            if (e->code == 0) {
                setStatus(Phase::Done, L"Descarga completada", e->file.empty() ? A.lastFile : e->file);
            } else if (e->code == -2) {
                setStatus(Phase::Ready, L"Descarga cancelada", L"Puedes volver a intentarlo cuando quieras");
            } else if (e->code == -1) {
                setStatus(Phase::Failed, L"No se pudo ejecutar youtube-dl", L"Comprueba que el antivirus no lo bloquea");
            } else {
                setStatus(Phase::Failed, L"No se pudo completar la descarga",
                          e->detail.empty() ? L"youtube-dl terminó con el código " + std::to_wstring(e->code)
                                            : e->detail);
            }
            if (e->code != 0) {
                // Recordamos qué fichero se estaba bajando por si acaso.
            }
            break;
    }
    delete e;
    updateButtons();
    invalidate();
}

// ============================================================================ Diseño (layout)

void layout() {
    float W_ = A.width, H_ = A.height;
    float m = kMargin;
    float x0 = m, x1 = std::max(m + 300.f, W_ - m);
    float y = kTitleH + 4;

    footerRc = R(x0, H_ - m - kFooterH + 4, x1, H_ - m + 4);
    if (footerRc.top < kTitleH + 40) footerRc = R(x0, kTitleH + 40, x1, kTitleH + 40 + kFooterH);
    A.viewport = R(0, kTitleH, W_, footerRc.top - 8);

    // Tarjeta del enlace
    urlCard.rc = R(x0, y, x1, y + 92);
    urlBox.rc = R(x0 + 16, y + 44, x1 - 16 - 104, y + 76);
    pasteBtn.rc = R(x1 - 16 - 96, y + 44, x1 - 16, y + 76);
    y += 92;

    sectionOptions = R(x0 + 2, y + 10, x1, y + 40);
    y += 40;

    auto row = [&](Card& c, Widget& w, float cw, float ch) {
        c.rc = R(x0, y, x1, y + 64);
        float cy = y + 32;
        w.rc = R(x1 - 16 - cw, cy - ch / 2, x1 - 16, cy + ch / 2);
        y += 64 + 4;
    };
    row(modeCard, modeSeg, 210, 34);
    row(qualityCard, qualityDd, 210, 32);
    row(formatCard, formatDd, 210, 32);
    row(playlistCard, playlistTg, 140, 32);
    row(thumbCard, thumbTg, 140, 32);
    y -= 4;

    sectionDest = R(x0 + 2, y + 10, x1, y + 40);
    y += 40;
    dirCard.rc = R(x0, y, x1, y + 92);
    dirBox.rc = R(x0 + 16, y + 44, x1 - 16 - 104 - 40, y + 76);
    browseBtn.rc = R(x1 - 16 - 96 - 40, y + 44, x1 - 16 - 40, y + 76);
    openDirBtn.rc = R(x1 - 16 - 32, y + 44, x1 - 16, y + 76);
    y += 92 + 12;

    A.contentH = y;
    urlBox.relayout();
    dirBox.relayout();
    float maxScroll = std::max(0.f, A.contentH - A.viewport.bottom);
    A.scroll = std::clamp(A.scroll, 0.f, maxScroll);

    // Pie: estado + botones
    float by = (footerRc.top + footerRc.bottom) / 2 - 16;
    mainBtn.rc = R(footerRc.right - 16 - 132, by, footerRc.right - 16, by + 32);
    secondaryBtn.rc = R(mainBtn.rc.left - 8 - 140, by, mainBtn.rc.left - 8, by + 32);
}

// ============================================================================ Pintado

void drawCard(const Card& c) {
    const Theme& t = A.th;
    fillRR(c.rc, 6, t.card);
    strokeRR(c.rc, 6, t.cardStroke);
    D2D1_COLOR_F fg = c.enabled ? t.text : t.text3;
    D2D1_COLOR_F fg2 = c.enabled ? t.text2 : t.text3;
    bool tall = c.rc.bottom - c.rc.top > 70;
    float top = tall ? c.rc.top + 12 : c.rc.top;
    float h = tall ? 24 : c.rc.bottom - c.rc.top;
    if (c.icon) text(c.icon, R(c.rc.left + 16, top, c.rc.left + 36, top + h), A.g.fIconBig, fg);
    float tx = c.rc.left + 52;
    if (tall) {
        text(c.title, R(tx, top, c.rc.right - 16, top + 24), A.g.fBody, fg);
    } else if (c.desc.empty()) {
        text(c.title, R(tx, c.rc.top, c.rc.right - 240, c.rc.bottom), A.g.fBody, fg);
    } else {
        float cy = (c.rc.top + c.rc.bottom) / 2;
        text(c.title, R(tx, cy - 20, c.rc.right - 240, cy + 1), A.g.fBody, fg);
        text(c.desc, R(tx, cy + 1, c.rc.right - 240, cy + 19), A.g.fCaption, fg2);
    }
}

void drawFooter() {
    const Theme& t = A.th;
    D2D1_RECT_F r = footerRc;
    fillRR(r, 8, t.card);
    strokeRR(r, 8, t.cardStroke);

    const wchar_t* icon = glyph::Info;
    D2D1_COLOR_F ic = t.accentText;
    switch (A.phase) {
        case Phase::Done: icon = glyph::Done; ic = t.success; break;
        case Phase::Failed:
        case Phase::ToolsFailed: icon = glyph::Error; ic = t.error; break;
        case Phase::Preparing:
        case Phase::Downloading: icon = A.percent >= 0 ? glyph::Download : glyph::Sync; ic = t.accentText; break;
        default: break;
    }
    float right = (secondaryBtn.visible ? secondaryBtn.rc.left : mainBtn.rc.left) - 16;
    bool working = A.phase == Phase::Preparing || A.phase == Phase::Downloading;
    float cy = working ? r.top + 30 : (r.top + r.bottom) / 2;
    text(icon, R(r.left + 18, cy - 12, r.left + 40, cy + 12), A.g.fIconBig, ic);
    text(A.statusTitle, R(r.left + 54, cy - 21, right, cy + 1), A.g.fBodyStrong, t.text);
    text(A.statusDetail, R(r.left + 54, cy + 1, right, cy + 19), A.g.fCaption, t.text2);

    if (working) {
        // Barra de progreso Fluent: pista fina y barra de acento redondeada.
        float px0 = r.left + 54, px1 = right, py = r.bottom - 16;
        setColor(t.strong);
        A.g.rt->DrawLine(D2D1::Point2F(px0, py), D2D1::Point2F(px1, py), A.g.brush, 1.f);
        float w = px1 - px0;
        if (A.percent >= 0) {
            float v = std::clamp(A.progressShown / 100.f, 0.f, 1.f);
            if (v > 0) fillRR(R(px0, py - 1.5f, px0 + std::max(3.f, w * v), py + 1.5f), 1.5f, t.accent);
        } else {
            double ph = std::fmod(nowSeconds() - A.animStart, 1.8) / 1.8;
            float seg = w * 0.35f;
            float sx = (float)(px0 - seg + (w + seg) * ph);
            float a = std::max(px0, sx), b = std::min(px1, sx + seg);
            if (b > a) fillRR(R(a, py - 1.5f, b, py + 1.5f), 1.5f, t.accent);
        }
    }
}

void drawTitleBar() {
    const Theme& t = A.th;
    D2D1_COLOR_F fg = A.active ? t.text : t.text3;
    if (A.g.icon) A.g.rt->DrawBitmap(A.g.icon, R(16, 14, 32, 30), A.active ? 1.f : 0.6f);
    text(L"YtDownloader", R(44, 0, A.width - 160, kTitleH), A.g.fCaption, fg);
}

void paint() {
    if (!A.g.rt) return;
    const Theme& t = A.th;
    A.g.rt->BeginDraw();
    A.g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    A.g.rt->Clear(A.mica ? D2D1::ColorF(0, 0, 0, 0) : t.solidBg);

    drawTitleBar();

    // Contenido desplazable
    A.g.rt->PushAxisAlignedClip(A.viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    A.g.rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -A.scroll));
    for (Card* c : cards) drawCard(*c);
    text(L"Opciones de descarga", sectionOptions, A.g.fBodyStrong, t.text);
    text(L"Destino", sectionDest, A.g.fBodyStrong, t.text);
    for (Widget* w : A.widgets)
        if (w->visible && w->inContent) w->draw();
    A.g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    A.g.rt->PopAxisAlignedClip();

    // Indicador de scroll
    float maxScroll = A.contentH - A.viewport.bottom;
    if (maxScroll > 0) {
        float vh = A.viewport.bottom - A.viewport.top;
        float total = A.contentH - A.viewport.top;
        float th = std::max(24.f, vh * vh / total);
        float ty = A.viewport.top + (vh - th) * (A.scroll / maxScroll);
        fillRR(R(A.width - 7, ty + 2, A.width - 4, ty + th - 2), 1.5f, t.strong);
    }

    drawFooter();
    for (Widget* w : A.widgets)
        if (w->visible && !w->inContent) w->draw();

    HRESULT hr = A.g.rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        release(A.g.icon);
        release(A.g.brush);
        release(A.g.brush2);
        release(A.g.rt);
    }
}

bool anyAnimating() {
    if (A.phase == Phase::Preparing || (A.phase == Phase::Downloading && A.percent < 0)) return true;
    if (A.phase == Phase::Downloading && std::fabs(A.progressShown - A.percent) > 0.1f) return true;
    for (Widget* w : A.widgets)
        if (w->animating()) return true;
    return false;
}

void tickAnimations() {
    if (A.percent >= 0) {
        A.progressShown += (A.percent - A.progressShown) * 0.25f;
        if (std::fabs(A.progressShown - A.percent) < 0.1f) A.progressShown = A.percent;
    }
}

void ensureAnimTimer() {
    if (anyAnimating()) SetTimer(A.hwnd, kTimerAnim, 16, nullptr);
}

// ============================================================================ Recursos gráficos

IDWriteTextFormat* makeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT ta,
                              bool ellipsis) {
    IDWriteTextFormat* f = nullptr;
    A.g.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                             L"es-es", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(ta);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* sign = nullptr;
        A.g.dw->CreateEllipsisTrimmingSign(f, &sign);
        f->SetTrimming(&tr, sign);
        release(sign);
    }
    return f;
}

bool fontExists(const wchar_t* family) {
    IDWriteFontCollection* c = nullptr;
    BOOL exists = FALSE;
    UINT32 idx = 0;
    if (SUCCEEDED(A.g.dw->GetSystemFontCollection(&c, FALSE))) c->FindFamilyName(family, &idx, &exists);
    release(c);
    return exists;
}

void createDeviceIndependent() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &A.g.d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&A.g.dw));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&A.g.wic));

    const wchar_t* ui = fontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* disp = fontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* icons = fontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";

    A.g.fBody = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fBodyStrong = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fBodyCenter = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    A.g.fBodyRight = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, false);
    A.g.fCaption = makeFormat(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fInput = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    A.g.fSubtitle = makeFormat(disp, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fIcon = makeFormat(icons, 16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    A.g.fIconBig = makeFormat(icons, 20, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    A.g.fIconSmall = makeFormat(icons, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
}

void createDeviceResources() {
    if (A.g.rt) return;
    RECT rc;
    GetClientRect(A.hwnd, &rc);
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)A.dpi, (float)A.dpi);
    auto hprops = D2D1::HwndRenderTargetProperties(
        A.hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right - rc.left), std::max<LONG>(1, rc.bottom - rc.top)));
    if (FAILED(A.g.d2d->CreateHwndRenderTarget(props, hprops, &A.g.rt))) return;
    A.g.rt->SetTextAntialiasMode(A.mica ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    A.g.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &A.g.brush);
    A.g.rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &A.g.brush2);

    // Icono de la aplicación (recurso 1) para la barra de título.
    if (A.g.wic) {
        HICON hi = (HICON)LoadImageW(A.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, (int)dip2px(16), (int)dip2px(16), 0);
        if (hi) {
            IWICBitmap* wb = nullptr;
            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(A.g.wic->CreateBitmapFromHICON(hi, &wb)) &&
                SUCCEEDED(A.g.wic->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                           WICBitmapPaletteTypeMedianCut)))
                A.g.rt->CreateBitmapFromWicBitmap(conv, nullptr, &A.g.icon);
            release(conv);
            release(wb);
            DestroyIcon(hi);
        }
    }
}

void discardDeviceResources() {
    release(A.g.icon);
    release(A.g.brush);
    release(A.g.brush2);
    release(A.g.rt);
}

// ============================================================================ Ventana

void applyTheme() {
    A.th = makeTheme(systemUsesDarkTheme());
    BOOL dark = A.th.dark;
    DwmSetWindowAttribute(A.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    // Menús contextuales en modo oscuro (API no documentada de uxtheme, la usan muchas apps).
    if (HMODULE ux = GetModuleHandleW(L"uxtheme.dll")) {
        using SetPreferredAppMode = int(WINAPI*)(int);
        using FlushMenuThemes = void(WINAPI*)();
        if (auto f = (SetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135))) f(1 /*AllowDark*/);
        if (auto f = (FlushMenuThemes)GetProcAddress(ux, MAKEINTRESOURCEA(136))) f();
    }
    invalidate();
}

void applyBackdrop() {
    DWORD build = windowsBuild();
    A.mica = false;
    if (build >= 22621) {
        int type = 2;  // DWMSBT_MAINWINDOW (Mica)
        A.mica = SUCCEEDED(DwmSetWindowAttribute(A.hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &type, sizeof(type)));
    } else if (build >= 22000) {
        BOOL on = TRUE;
        A.mica = SUCCEEDED(DwmSetWindowAttribute(A.hwnd, 1029 /*DWMWA_MICA_EFFECT*/, &on, sizeof(on)));
    }
    if (A.customFrame || A.mica) {
        MARGINS m{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(A.hwnd, &m);
    }
}

void updateSize() {
    RECT rc;
    GetClientRect(A.hwnd, &rc);
    A.width = px2dip((float)(rc.right - rc.left));
    A.height = px2dip((float)(rc.bottom - rc.top));
    if (A.g.rt) A.g.rt->Resize(D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)));
    layout();
}

Widget* widgetAt(D2D1_POINT_2F p) {
    for (auto it = A.widgets.rbegin(); it != A.widgets.rend(); ++it) {
        Widget* w = *it;
        if (!w->visible) continue;
        if (w->inContent) {
            if (!inside(A.viewport, p)) continue;
            if (inside(w->rc, D2D1::Point2F(p.x, p.y + A.scroll))) return w;
        } else if (inside(w->rc, p)) {
            return w;
        }
    }
    return nullptr;
}

D2D1_POINT_2F local(Widget* w, D2D1_POINT_2F p) { return w && w->inContent ? D2D1::Point2F(p.x, p.y + A.scroll) : p; }

D2D1_POINT_2F mousePoint(LPARAM lp) {
    return D2D1::Point2F(px2dip((float)GET_X_LPARAM(lp)), px2dip((float)GET_Y_LPARAM(lp)));
}

void setFocus(Widget* w) {
    if (A.focus == w) return;
    Widget* old = A.focus;
    A.focus = w;
    if (old) old->focusChanged(false);
    if (w) {
        w->focusChanged(true);
        SetTimer(A.hwnd, kTimerCaret, GetCaretBlinkTime(), nullptr);
    } else {
        KillTimer(A.hwnd, kTimerCaret);
    }
    invalidate();
}

// Asegura que el widget enfocado sea visible dentro de la zona con scroll.
void scrollIntoView(Widget* w) {
    if (!w || !w->inContent) return;
    if (w->rc.top - A.scroll < A.viewport.top + 8) A.scroll = w->rc.top - A.viewport.top - 8;
    if (w->rc.bottom - A.scroll > A.viewport.bottom - 8) A.scroll = w->rc.bottom - A.viewport.bottom + 8;
    layout();
}

void chooseFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Elige la carpeta de destino");
    IShellItem* cur = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(trim(dirBox.value).c_str(), nullptr, IID_PPV_ARGS(&cur)))) {
        dlg->SetFolder(cur);
        release(cur);
    }
    if (SUCCEEDED(dlg->Show(A.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                dirBox.setText(path);
                persist();
                CoTaskMemFree(path);
            }
            release(item);
        }
    }
    release(dlg);
    invalidate();
}

void initWidgets() {
    urlCard.icon = glyph::Link;
    urlCard.title = L"Enlace del vídeo o de la lista";
    urlBox.placeholder = L"https://www.youtube.com/watch?v=…";
    urlBox.onEnter = [] {
        if (A.phase != Phase::Downloading && A.phase != Phase::Preparing) startDownload();
    };
    pasteBtn.label = L"Pegar";
    pasteBtn.icon = glyph::Paste;
    pasteBtn.onClick = [] {
        urlBox.setText(trim(TextBox::clipboardText()));
        setFocus(&urlBox);
    };

    modeCard.title = L"Tipo de descarga";
    modeCard.desc = L"Vídeo con sonido o solo la pista de audio";
    modeSeg.items = {{glyph::Video, L"Vídeo"}, {glyph::Audio, L"Audio"}};
    modeSeg.onChange = [](int i) {
        A.mode = i == 1 ? Mode::Audio : Mode::Video;
        refreshOptions();
        persist();
    };

    qualityCard.icon = glyph::Quality;
    qualityCard.title = L"Calidad";
    qualityDd.onChange = [](int i) {
        if (A.mode == Mode::Video) A.videoQuality = videoQualities()[i];
        else A.audioQuality = audioQualities()[i];
        persist();
    };

    formatCard.icon = glyph::Format;
    formatCard.title = L"Formato";
    formatDd.onChange = [](int i) {
        if (A.mode == Mode::Video) A.videoFormat = videoFormats()[i];
        else A.audioFormat = audioFormats()[i];
        refreshOptions();
        persist();
    };

    playlistCard.icon = glyph::List;
    playlistCard.title = L"Lista de reproducción completa";
    playlistCard.desc = L"Si el enlace pertenece a una lista, descargarla entera";
    playlistTg.onChange = [](bool on) { A.playlist = on; };

    thumbCard.icon = glyph::Photo;
    thumbCard.title = L"Incrustar carátula";
    thumbTg.onChange = [](bool on) {
        A.thumbnail = on;
        persist();
    };

    dirCard.icon = glyph::Folder;
    dirCard.title = L"Carpeta donde se guardan las descargas";
    dirBox.placeholder = L"Carpeta de destino";
    dirBox.onChange = [] {};
    browseBtn.label = L"Examinar";
    browseBtn.onClick = chooseFolder;
    openDirBtn.icon = glyph::OpenFolder;
    openDirBtn.onClick = [] {
        std::wstring d = trim(dirBox.value);
        std::error_code ec;
        fs::create_directories(fs::path(d), ec);
        openFolder(d);
    };

    mainBtn.inContent = false;
    secondaryBtn.inContent = false;
    mainBtn.onClick = [] {
        if (A.phase == Phase::Downloading) {
            if (A.cancel) A.cancel->cancel();
            A.statusTitle = L"Cancelando…";
            invalidate();
        } else if (A.phase == Phase::ToolsFailed) {
            startPrepare();
        } else {
            startDownload();
        }
    };

    A.widgets = {&urlBox, &pasteBtn, &modeSeg, &qualityDd, &formatDd, &playlistTg, &thumbTg,
                 &dirBox, &browseBtn, &openDirBtn, &secondaryBtn, &mainBtn};

    // Ajustes guardados
    A.mode = regDword(L"Mode", 0) == 1 ? Mode::Audio : Mode::Video;
    A.videoQuality = U(regString(L"VideoQuality", L"best"));
    A.audioQuality = U(regString(L"AudioQuality", L"best"));
    A.videoFormat = U(regString(L"VideoFormat", L"mp4"));
    A.audioFormat = U(regString(L"AudioFormat", L"mp3"));
    A.thumbnail = regDword(L"Thumbnail", 1) != 0;
    auto valid = [](const std::vector<std::string>& v, std::string& s, const char* def) {
        if (std::find(v.begin(), v.end(), s) == v.end()) s = def;
    };
    valid(videoQualities(), A.videoQuality, "best");
    valid(audioQualities(), A.audioQuality, "best");
    valid(videoFormats(), A.videoFormat, "mp4");
    valid(audioFormats(), A.audioFormat, "mp3");
    modeSeg.sel = A.mode == Mode::Audio ? 1 : 0;
    dirBox.setText(regString(L"OutputDir", downloadsFolder()));
    refreshOptions();
}

LRESULT hitTest(HWND hwnd, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd, &pt);
    int frame = GetSystemMetricsForDpi(SM_CYFRAME, A.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, A.dpi);
    if (!IsZoomed(hwnd) && pt.y < frame) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (pt.x < frame * 2) return HTTOPLEFT;
        if (pt.x > rc.right - frame * 2) return HTTOPRIGHT;
        return HTTOP;
    }
    if (pt.y < dip2px(kTitleH)) return HTCAPTION;
    return HTCLIENT;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (A.customFrame) {
        LRESULT r = 0;
        if (DwmDefWindowProc(hwnd, msg, wp, lp, &r)) return r;
    }
    switch (msg) {
        case WM_CREATE: {
            A.hwnd = hwnd;
            A.dpi = GetDpiForWindow(hwnd);
            applyTheme();
            applyBackdrop();
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            return 0;
        }
        case WM_NCCALCSIZE:
            if (wp && A.customFrame) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                int fx = GetSystemMetricsForDpi(SM_CXFRAME, A.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, A.dpi);
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, A.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, A.dpi);
                p->rgrc[0].left += fx;
                p->rgrc[0].right -= fx;
                p->rgrc[0].bottom -= fy;
                if (IsZoomed(hwnd)) p->rgrc[0].top += fy;
                return 0;
            }
            break;
        case WM_NCHITTEST:
            if (A.customFrame) {
                LRESULT d = DefWindowProcW(hwnd, msg, wp, lp);
                if (d != HTCLIENT) return d;
                return hitTest(hwnd, lp);
            }
            break;
        case WM_ACTIVATE:
            A.active = LOWORD(wp) != WA_INACTIVE;
            if (A.customFrame || A.mica) {
                MARGINS m{-1, -1, -1, -1};
                DwmExtendFrameIntoClientArea(hwnd, &m);
            }
            invalidate();
            break;
        case WM_SIZE:
            updateSize();
            invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = (LONG)dip2px(560);
            mm->ptMinTrackSize.y = (LONG)dip2px(460);
            return 0;
        }
        case WM_DPICHANGED: {
            A.dpi = HIWORD(wp);
            discardDeviceResources();
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            updateSize();
            invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) {
                applyTheme();
                discardDeviceResources();
            }
            break;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            applyTheme();
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            createDeviceResources();
            paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerCaret) {
                A.caretOn = !A.caretOn;
                invalidate();
            } else if (wp == kTimerAnim) {
                tickAnimations();
                invalidate();
                if (!anyAnimating()) KillTimer(hwnd, kTimerAnim);
            }
            return 0;
        case WM_MOUSEMOVE: {
            D2D1_POINT_2F p = mousePoint(lp);
            if (!A.trackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                A.trackingMouse = true;
            }
            Widget* w = A.captured ? A.captured : widgetAt(p);
            Widget* hot = A.captured ? (widgetAt(p) == A.captured ? A.captured : nullptr) : w;
            if (hot && !hot->enabled) hot = nullptr;
            if (hot != A.hot) {
                A.hot = hot;
                invalidate();
            }
            if (w && w->enabled) w->mouseMove(local(w, p));
            return 0;
        }
        case WM_MOUSELEAVE:
            A.trackingMouse = false;
            if (A.hot && !A.captured) {
                A.hot = nullptr;
                invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            D2D1_POINT_2F p = mousePoint(lp);
            Widget* w = widgetAt(p);
            if (w && !w->enabled) w = nullptr;
            setFocus(w && w->focusable() ? w : nullptr);
            if (w) {
                A.captured = w;
                SetCapture(hwnd);
                if (msg == WM_LBUTTONDBLCLK) w->doubleClick(local(w, p));
                else w->mouseDown(local(w, p));
                A.hot = w;
                invalidate();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            D2D1_POINT_2F p = mousePoint(lp);
            Widget* w = A.captured;
            A.captured = nullptr;
            ReleaseCapture();
            if (w) {
                bool over = widgetAt(p) == w;
                w->mouseUp(local(w, p), over);
                ensureAnimTimer();
            }
            A.hot = widgetAt(p);
            invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (A.captured && (HWND)lp != hwnd) {
                A.captured->pressed = false;
                A.captured = nullptr;
                invalidate();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            float maxScroll = std::max(0.f, A.contentH - A.viewport.bottom);
            A.scroll = std::clamp(A.scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 48.f, 0.f, maxScroll);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            A.hot = widgetAt(D2D1::Point2F(px2dip((float)pt.x), px2dip((float)pt.y)));
            invalidate();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                Widget* w = widgetAt(D2D1::Point2F(px2dip((float)pt.x), px2dip((float)pt.y)));
                SetCursor(LoadCursorW(nullptr, w && w->enabled ? w->cursor() : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_CONTEXTMENU: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT cpt = pt;
            ScreenToClient(hwnd, &cpt);
            Widget* w = widgetAt(D2D1::Point2F(px2dip((float)cpt.x), px2dip((float)cpt.y)));
            if (w && w->focusable()) {
                setFocus(w);
                w->contextMenu(pt);
                return 0;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wp == VK_TAB) {
                // Tab alterna entre los cuadros de texto.
                Widget* next = A.focus == &urlBox ? (Widget*)&dirBox : (Widget*)&urlBox;
                setFocus(next);
                if (auto* tb = dynamic_cast<TextBox*>(next)) tb->selectAll();
                scrollIntoView(next);
                return 0;
            }
            if (wp == VK_ESCAPE && A.focus) {
                setFocus(nullptr);
                return 0;
            }
            if (A.focus) A.focus->key(wp);
            return 0;
        case WM_CHAR:
            if (wp == L'\t' || wp == 27) return 0;
            if (A.focus) A.focus->chr((wchar_t)wp);
            else if (wp == 22) {  // Ctrl+V sin foco: pegar el enlace
                setFocus(&urlBox);
                urlBox.selectAll();
                urlBox.paste();
            } else if (wp == 13 && A.phase != Phase::Downloading && A.phase != Phase::Preparing) {
                startDownload();
            }
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            invalidate();
            break;
        case WM_APP_EVENT:
            handleEvent(reinterpret_cast<Event*>(lp));
            ensureAnimTimer();
            return 0;
        case WM_APP_ASK: {
            auto* q = reinterpret_cast<std::wstring*>(lp);
            return MessageBoxW(hwnd, q->c_str(), L"YtDownloader", MB_YESNO | MB_ICONQUESTION);
        }
        case WM_CLOSE:
            if (A.phase == Phase::Downloading &&
                MessageBoxW(hwnd, L"Hay una descarga en curso. ¿Quieres cancelarla y salir?", L"YtDownloader",
                            MB_YESNO | MB_ICONWARNING) != IDYES)
                return 0;
            persist();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (A.cancel) A.cancel->cancel();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int runGui() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // youtube-dl (Python) debe escribir en UTF-8 y sin búfer para poder leer el progreso en vivo.
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");

    A.inst = GetModuleHandleW(nullptr);
    A.customFrame = windowsBuild() >= 22000;
    createDeviceIndependent();
    initWidgets();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = A.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(A.inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(A.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"YtDownloaderWindow";
    RegisterClassExW(&wc);

    // Tamaño inicial: 760x780 DIP, sin pasarse del área de trabajo del monitor principal.
    POINT origin{0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT sysDpi = GetDpiForSystem();
    int w = MulDiv(760, sysDpi, 96), h = MulDiv(780, sysDpi, 96);
    int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    w = std::min(w, aw - 40);
    h = std::min(h, ah - 40);
    int x = mi.rcWork.left + (aw - w) / 2, y = mi.rcWork.top + (ah - h) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"YtDownloader", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr,
                                nullptr, A.inst, nullptr);
    if (!hwnd) return 1;
    updateSize();
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    A.animStart = nowSeconds();
    startPrepare();
    ensureAnimTimer();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    // Si queda trabajo en segundo plano (p.ej. descargando dependencias) no esperamos:
    // al salir del proceso se cierran el job y los procesos hijos de youtube-dl.
    if (A.worker.joinable()) {
        if (A.busy) A.worker.detach();
        else A.worker.join();
    }
    A.hwnd = nullptr;
    CoUninitialize();
    return 0;
}

#endif  // _WIN32
