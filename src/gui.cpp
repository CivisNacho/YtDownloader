// Interfaz gráfica moderna estilo Windows 11 Fluent: fondo Mica, barra de título integrada,
// controles animados en Direct2D/DirectWrite, menú desplegable propio, selector de formatos
// por fichas, panel de registro integrado y selector de tema. Sin dependencias externas.

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

std::wstring toLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
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
    t = std::clamp(t, 0.f, 1.f);
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

D2D1_RECT_F inflate(D2D1_RECT_F r, float dx, float dy) {
    return R(r.left - dx, r.top - dy, r.right + dx, r.bottom + dy);
}

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

void copyTextToClipboard(HWND hwnd, const std::wstring& s) {
    if (s.empty() || !OpenClipboard(hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

// ============================================================================ Tema

struct Theme {
    bool dark = true;
    D2D1_COLOR_F solidBg, text, text2, text3;
    D2D1_COLOR_F card, cardElevated, cardStroke, cardTopHighlight, cardShadow;
    D2D1_COLOR_F ctl, ctlHover, ctlPressed, ctlDisabled, ctlStroke, ctlStrokeBottom;
    D2D1_COLOR_F strong, inputBg, inputActive, inputHover;
    D2D1_COLOR_F accent, accentText, accentSoft, accentSoftBorder, accentDisabled, onAccent, onAccentDisabled;
    D2D1_COLOR_F subtle, subtleHover, subtlePressed, selection;
    D2D1_COLOR_F success, successSoft, warning, warningSoft, error, errorSoft;
    D2D1_COLOR_F separator, toggleOffFill, trackBg;
    D2D1_COLOR_F flyoutBg, flyoutBorder, consoleBg, consoleBorder, tooltipBg, tooltipBorder;
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
        t.cardElevated = argb(0x13FFFFFF);
        t.cardStroke = argb(0x14FFFFFF);
        t.cardTopHighlight = argb(0x18FFFFFF);
        t.cardShadow = argb(0x24000000);

        t.ctl = argb(0x0FFFFFFF);
        t.ctlHover = argb(0x16FFFFFF);
        t.ctlPressed = argb(0x08FFFFFF);
        t.ctlDisabled = argb(0x0BFFFFFF);
        t.ctlStroke = argb(0x12FFFFFF);
        t.ctlStrokeBottom = argb(0x1CFFFFFF);

        t.strong = argb(0x8BFFFFFF);
        t.inputBg = argb(0x0FFFFFFF);
        t.inputHover = argb(0x15FFFFFF);
        t.inputActive = argb(0xB31E1E1E);

        t.accent = havePal ? pal[1] : argb(0xFF60CDFF);
        t.accentText = havePal ? pal[0] : argb(0xFF99EBFF);
        t.accentSoft = withAlpha(t.accent, 0.16f);
        t.accentSoftBorder = withAlpha(t.accent, 0.45f);
        t.accentDisabled = argb(0x28FFFFFF);
        t.onAccent = argb(0xFF000000);
        t.onAccentDisabled = argb(0x87FFFFFF);

        t.subtle = argb(0x00FFFFFF);
        t.subtleHover = argb(0x0FFFFFFF);
        t.subtlePressed = argb(0x0AFFFFFF);

        t.success = argb(0xFF6CCB5F);
        t.successSoft = argb(0x206CCB5F);
        t.warning = argb(0xFFFCE100);
        t.warningSoft = argb(0x1FFCE100);
        t.error = argb(0xFFFF99A4);
        t.errorSoft = argb(0x22FF99A4);

        t.separator = argb(0x14FFFFFF);
        t.toggleOffFill = argb(0x08FFFFFF);
        t.trackBg = argb(0x1CFFFFFF);

        t.flyoutBg = argb(0xFA2C2C2C);
        t.flyoutBorder = argb(0x1FFFFFFF);
        t.consoleBg = argb(0xF21A1A1A);
        t.consoleBorder = argb(0x20FFFFFF);
        t.tooltipBg = argb(0xFA2C2C2C);
        t.tooltipBorder = argb(0x22FFFFFF);
    } else {
        t.solidBg = argb(0xFFF3F3F3);
        t.text = argb(0xE4000000);
        t.text2 = argb(0x9E000000);
        t.text3 = argb(0x5C000000);

        t.card = argb(0xB3FFFFFF);
        t.cardElevated = argb(0xDCFFFFFF);
        t.cardStroke = argb(0x0F000000);
        t.cardTopHighlight = argb(0xB8FFFFFF);
        t.cardShadow = argb(0x0A000000);

        t.ctl = argb(0xB3FFFFFF);
        t.ctlHover = argb(0x80F9F9F9);
        t.ctlPressed = argb(0x4DF9F9F9);
        t.ctlDisabled = argb(0x4DF9F9F9);
        t.ctlStroke = argb(0x0F000000);
        t.ctlStrokeBottom = argb(0x29000000);

        t.strong = argb(0x72000000);
        t.inputBg = argb(0xB3FFFFFF);
        t.inputHover = argb(0xD8F9F9F9);
        t.inputActive = argb(0xFFFFFFFF);

        t.accent = havePal ? pal[4] : argb(0xFF005FB8);
        t.accentText = havePal ? pal[5] : argb(0xFF003E92);
        t.accentSoft = withAlpha(t.accent, 0.11f);
        t.accentSoftBorder = withAlpha(t.accent, 0.42f);
        t.accentDisabled = argb(0x37000000);
        t.onAccent = argb(0xFFFFFFFF);
        t.onAccentDisabled = argb(0xFFFFFFFF);

        t.subtle = argb(0x00000000);
        t.subtleHover = argb(0x09000000);
        t.subtlePressed = argb(0x06000000);

        t.success = argb(0xFF0F7B0F);
        t.successSoft = argb(0x160F7B0F);
        t.warning = argb(0xFF9D5D00);
        t.warningSoft = argb(0x189D5D00);
        t.error = argb(0xFFC42B1C);
        t.errorSoft = argb(0x16C42B1C);

        t.separator = argb(0x0F000000);
        t.toggleOffFill = argb(0x06000000);
        t.trackBg = argb(0x16000000);

        t.flyoutBg = argb(0xFAF9F9F9);
        t.flyoutBorder = argb(0x1A000000);
        t.consoleBg = argb(0xF61F1F1F);
        t.consoleBorder = argb(0x24000000);
        t.tooltipBg = argb(0xFAFFFFFF);
        t.tooltipBorder = argb(0x1A000000);
    }
    t.selection = havePal ? pal[3] : argb(0xFF0078D4);
    return t;
}

// Glifos de Segoe Fluent Icons / Segoe MDL2 Assets.
namespace glyph {
const wchar_t* Link = L"\uE71B";
const wchar_t* Paste = L"\uE77F";
const wchar_t* Video = L"\uE714";
const wchar_t* Audio = L"\uE8D6";
const wchar_t* Quality = L"\uE9E9";
const wchar_t* Format = L"\uE7C3";
const wchar_t* List = L"\uE8FD";
const wchar_t* Photo = L"\uE91B";
const wchar_t* Folder = L"\uE8B7";
const wchar_t* OpenFolder = L"\uE838";
const wchar_t* Download = L"\uE896";
const wchar_t* Cancel = L"\uE711";
const wchar_t* Chevron = L"\uE70D";
const wchar_t* ChevronUp = L"\uE70E";
const wchar_t* Check = L"\uE73E";
const wchar_t* Info = L"\uE946";
const wchar_t* Error = L"\uEA39";
const wchar_t* Done = L"\uE930";
const wchar_t* Sync = L"\uE895";
const wchar_t* Log = L"\uE756";      // CommandPrompt / terminal
const wchar_t* Copy = L"\uE8C8";
const wchar_t* Retry = L"\uE72C";
const wchar_t* Sun = L"\uE706";
const wchar_t* Moon = L"\uE708";
const wchar_t* Contrast = L"\uE793";
const wchar_t* Clear = L"\uE894";
const wchar_t* Warning = L"\uE7BA";
const wchar_t* Sparkle = L"\uE734";
}  // namespace glyph

// ============================================================================ Estado global

constexpr UINT WM_APP_EVENT = WM_APP + 1;
constexpr UINT WM_APP_ASK = WM_APP + 2;
constexpr UINT_PTR kTimerCaret = 1;
constexpr UINT_PTR kTimerAnim = 2;
constexpr float kTitleH = 46.f;
constexpr float kMargin = 20.f;
constexpr float kFooterH = 92.f;

struct Widget;
struct Dropdown;

struct Gfx {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    IWICImagingFactory* wic = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1SolidColorBrush* brush2 = nullptr;  // texto seleccionado
    ID2D1Bitmap* icon = nullptr;
    IDWriteTextFormat* fBody = nullptr;         // 14 regular, elipsis
    IDWriteTextFormat* fBodyStrong = nullptr;   // 14 semibold, elipsis
    IDWriteTextFormat* fBodyCenter = nullptr;   // 14 regular centrado
    IDWriteTextFormat* fBodyBoldCtr = nullptr;  // 14 semibold centrado
    IDWriteTextFormat* fBodyRight = nullptr;    // 14 derecha
    IDWriteTextFormat* fCaption = nullptr;      // 12 regular, elipsis
    IDWriteTextFormat* fCaptionStrong = nullptr;// 12 semibold, elipsis
    IDWriteTextFormat* fCaptionRight = nullptr; // 12 derecha
    IDWriteTextFormat* fBadge = nullptr;        // 11 semibold centrado
    IDWriteTextFormat* fTitle = nullptr;        // 13 semibold barra de título
    IDWriteTextFormat* fInput = nullptr;        // 14 sin elipsis
    IDWriteTextFormat* fMono = nullptr;         // 12 monospace para registro
    IDWriteTextFormat* fIcon = nullptr;         // 15 iconos
    IDWriteTextFormat* fIconBig = nullptr;      // 18 iconos
    IDWriteTextFormat* fIconSmall = nullptr;    // 11 iconos
};

enum class Phase { Preparing, Ready, Downloading, Done, Failed, ToolsFailed };

struct UrlInfo {
    bool valid = false;
    bool isYoutube = false;
    bool isPlaylist = false;
    bool isShorts = false;
    std::wstring badge;
};

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
    D2D1_POINT_2F mousePos{};
    double hotSince = 0;

    // Menú desplegable Direct2D activo (overlay)
    Dropdown* openDropdown = nullptr;
    D2D1_RECT_F flyoutRc{};
    int flyoutHot = -1;
    float flyoutScroll = 0;

    // Panel de registro integrado
    bool logOpen = false;
    float logScroll = 0;

    // Animaciones
    double animStart = 0;
    float progressShown = 0;
    float copiedToastAlpha = 0;

    // Opciones
    Mode mode = Mode::Video;
    std::string videoQuality = "best", audioQuality = "best", videoFormat = "mp4", audioFormat = "mp3";
    bool playlist = false, thumbnail = true;
    UrlInfo urlInfo;

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
    while (A.log.size() > 500) A.log.pop_front();
    // Auto-scroll al final si el registro está abierto.
    if (A.logOpen) A.logScroll = 1e9f;
}

UrlInfo analyzeUrl(const std::wstring& raw) {
    UrlInfo info;
    std::wstring s = trim(raw);
    if (s.empty()) return info;
    std::wstring lo = toLower(s);
    if (lo.find(L"http://") == 0 || lo.find(L"https://") == 0 || lo.find(L"www.") == 0 ||
        lo.find(L"youtu.be/") != std::wstring::npos || lo.find(L"youtube.com") != std::wstring::npos) {
        info.valid = true;
    }
    if (lo.find(L"youtube.com") != std::wstring::npos || lo.find(L"youtu.be") != std::wstring::npos ||
        lo.find(L"music.youtube.com") != std::wstring::npos) {
        info.isYoutube = true;
        if (lo.find(L"/shorts/") != std::wstring::npos) {
            info.isShorts = true;
            info.badge = L"YouTube Shorts";
        } else if (lo.find(L"list=") != std::wstring::npos) {
            info.isPlaylist = true;
            info.badge = lo.find(L"/playlist") != std::wstring::npos ? L"Lista de YouTube" : L"Vídeo + Lista";
        } else if (lo.find(L"music.youtube.com") != std::wstring::npos) {
            info.badge = L"YouTube Music";
        } else {
            info.badge = L"Vídeo de YouTube";
        }
    } else if (info.valid) {
        info.badge = L"Enlace externo";
    }
    return info;
}

// ------------------------------------------------------------------- Primitivas de dibujo

void setColor(D2D1_COLOR_F c) { A.g.brush->SetColor(c); }

void fillRR(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    setColor(c);
    A.g.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), A.g.brush);
}

void strokeRR(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1.f) {
    setColor(c);
    float h = w / 2.f;
    A.g.rt->DrawRoundedRectangle(D2D1::RoundedRect(R(r.left + h, r.top + h, r.right - h, r.bottom - h), rad, rad),
                                 A.g.brush, w);
}

void fillRect(D2D1_RECT_F r, D2D1_COLOR_F c) {
    setColor(c);
    A.g.rt->FillRectangle(r, A.g.brush);
}

void text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* f, D2D1_COLOR_F c) {
    if (s.empty() || !f) return;
    setColor(c);
    A.g.rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, A.g.brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float textWidth(const std::wstring& s, IDWriteTextFormat* f) {
    if (s.empty() || !f) return 0.f;
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

// Borde de control Fluent: trazo fino con la parte inferior ligeramente más definida.
void controlBorder(D2D1_RECT_F r, float rad, D2D1_COLOR_F stroke, D2D1_COLOR_F bottom) {
    strokeRR(r, rad, stroke);
    setColor(bottom);
    A.g.rt->DrawLine(D2D1::Point2F(r.left + rad, r.bottom - 0.5f), D2D1::Point2F(r.right - rad, r.bottom - 0.5f),
                     A.g.brush, 1.f);
}

// Superficie de tarjeta Fluent con sutil brillo superior y sombra suave en modo claro.
void drawSurface(D2D1_RECT_F r, float rad, bool elevated = false) {
    const Theme& t = A.th;
    if (!t.dark) {
        fillRR(R(r.left, r.top + 1.f, r.right, r.bottom + 2.f), rad, t.cardShadow);
    }
    fillRR(r, rad, elevated ? t.cardElevated : t.card);
    strokeRR(r, rad, t.cardStroke);
    setColor(t.cardTopHighlight);
    A.g.rt->DrawLine(D2D1::Point2F(r.left + rad, r.top + 0.5f), D2D1::Point2F(r.right - rad, r.top + 0.5f),
                     A.g.brush, 1.f);
}

// ============================================================================ Widgets

struct Widget {
    D2D1_RECT_F rc{};
    bool visible = true;
    bool enabled = true;
    bool inContent = true;  // se desplaza con el scroll
    bool pressed = false;
    float hoverAnim = 0.f;
    std::wstring tooltip;

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
    virtual bool animating() const {
        float target = (isHot() && enabled) ? 1.f : 0.f;
        return std::fabs(hoverAnim - target) > 0.01f;
    }
    void stepHover() {
        float target = (isHot() && enabled) ? 1.f : 0.f;
        hoverAnim += (target - hoverAnim) * 0.30f;
        if (std::fabs(hoverAnim - target) < 0.01f) hoverAnim = target;
    }
    virtual LPCWSTR cursor() const { return IDC_ARROW; }
    bool isHot() const { return A.hot == this; }
    D2D1_RECT_F screenRc() const {
        return inContent ? R(rc.left, rc.top - A.scroll, rc.right, rc.bottom - A.scroll) : rc;
    }
};

// ------------------------------------------------------------------- Botón

struct Button : Widget {
    enum class Style { Standard, Primary, Subtle, Danger };
    std::wstring label;
    const wchar_t* icon = nullptr;
    Style style = Style::Standard;
    bool activeState = false;  // p.ej. botón de registro abierto
    bool onDarkSurface = false;
    std::function<void()> onClick;

    LPCWSTR cursor() const override { return enabled ? IDC_HAND : IDC_ARROW; }

    void draw() override {
        stepHover();
        const Theme& t = A.th;
        bool hot = isHot() && enabled;
        bool down = pressed && hot;
        float rad = (style == Style::Primary) ? 7.f : 6.f;
        D2D1_COLOR_F fg = t.text;

        if (style == Style::Primary) {
            if (!enabled) {
                fillRR(rc, rad, t.accentDisabled);
                fg = t.onAccentDisabled;
            } else {
                D2D1_COLOR_F fill = down ? withAlpha(t.accent, 0.80f)
                                         : mix(t.accent, withAlpha(t.accent, 0.90f), hoverAnim);
                fillRR(rc, rad, fill);
                controlBorder(rc, rad, argb(0x14FFFFFF), argb(t.dark ? 0x24000000 : 0x66000000));
                fg = down ? withAlpha(t.onAccent, 0.72f) : t.onAccent;
            }
        } else if (style == Style::Danger) {
            D2D1_COLOR_F base = t.errorSoft;
            D2D1_COLOR_F hov = withAlpha(t.error, t.dark ? 0.28f : 0.20f);
            D2D1_COLOR_F fill = !enabled ? t.ctlDisabled : down ? withAlpha(t.error, 0.14f) : mix(base, hov, hoverAnim);
            fillRR(rc, rad, fill);
            strokeRR(rc, rad, withAlpha(t.error, enabled ? 0.50f : 0.20f));
            fg = !enabled ? t.text3 : t.error;
        } else if (style == Style::Subtle) {
            D2D1_COLOR_F base = activeState ? t.accentSoft : t.subtle;
            D2D1_COLOR_F hov = activeState ? withAlpha(t.accent, 0.22f)
                                           : (onDarkSurface ? argb(0x18FFFFFF) : t.subtleHover);
            D2D1_COLOR_F pr = onDarkSurface ? argb(0x0EFFFFFF) : t.subtlePressed;
            D2D1_COLOR_F fill = !enabled ? t.subtle : down ? pr : mix(base, hov, hoverAnim);
            if (fill.a > 0.005f) fillRR(rc, rad, fill);
            if (activeState) strokeRR(rc, rad, t.accentSoftBorder);
            if (onDarkSurface) {
                fg = !enabled ? argb(0x50FFFFFF) : down ? argb(0xB0FFFFFF) : mix(argb(0xC8E6EAF0), argb(0xFFFFFFFF), hoverAnim);
            } else {
                fg = !enabled ? t.text3 : activeState ? t.accentText : (down ? t.text2 : mix(t.text2, t.text, hoverAnim));
            }
        } else {
            D2D1_COLOR_F fill = !enabled ? t.ctlDisabled : down ? t.ctlPressed : mix(t.ctl, t.ctlHover, hoverAnim);
            fillRR(rc, rad, fill);
            controlBorder(rc, rad, t.ctlStroke, down || !enabled ? t.ctlStroke : t.ctlStrokeBottom);
            fg = !enabled ? t.text3 : down ? t.text2 : t.text;
        }

        IDWriteTextFormat* tf = (style == Style::Primary) ? A.g.fBodyBoldCtr : A.g.fBodyCenter;
        float iconW = icon ? 16.f : 0.f;
        float gap = icon && !label.empty() ? 8.f : 0.f;
        float tw = label.empty() ? 0.f : textWidth(label, tf);
        float total = iconW + gap + tw;
        float x = (rc.left + rc.right - total) / 2.f;
        float dy = down ? 0.5f : 0.f;
        if (icon) text(icon, R(x, rc.top + dy, x + iconW, rc.bottom + dy), A.g.fIcon, fg);
        if (!label.empty())
            text(label, R(x + iconW + gap - 1, rc.top + dy, x + total + 1, rc.bottom - 1 + dy), tf, fg);
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

    LPCWSTR cursor() const override { return enabled ? IDC_HAND : IDC_ARROW; }

    D2D1_RECT_F track() const {
        float cy = (rc.top + rc.bottom) / 2.f;
        return R(rc.right - 42.f, cy - 11.f, rc.right, cy + 11.f);
    }

    void draw() override {
        stepHover();
        const Theme& t = A.th;
        float target = on ? 1.f : 0.f;
        anim += (target - anim) * 0.30f;
        if (std::fabs(anim - target) < 0.01f) anim = target;

        D2D1_RECT_F tr = track();
        if (anim > 0.01f) {
            D2D1_COLOR_F offFill = mix(t.toggleOffFill, t.ctlHover, hoverAnim);
            D2D1_COLOR_F onFill = !enabled ? t.accentDisabled
                                           : (pressed ? withAlpha(t.accent, 0.80f)
                                                      : mix(t.accent, withAlpha(t.accent, 0.90f), hoverAnim));
            fillRR(tr, 11.f, mix(offFill, onFill, anim));
        } else {
            fillRR(tr, 11.f, mix(t.toggleOffFill, t.ctlHover, hoverAnim));
        }
        if (anim < 0.99f) {
            strokeRR(tr, 11.f, withAlpha(enabled ? t.strong : t.text3, 1.f - anim));
        }

        float knob = (pressed && enabled) ? 14.5f : (12.5f + 1.5f * hoverAnim);
        float x0 = tr.left + 11.f, x1 = tr.right - 11.f;
        float cx = x0 + (x1 - x0) * anim;
        if (pressed && enabled) cx += on ? -1.5f : 1.5f;
        D2D1_COLOR_F kc = mix(enabled ? t.text2 : t.text3, enabled ? t.onAccent : t.onAccentDisabled, anim);
        setColor(kc);
        A.g.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, (tr.top + tr.bottom) / 2.f), knob / 2.f, knob / 2.f),
                            A.g.brush);
    }

    bool animating() const override { return Widget::animating() || std::fabs(anim - (on ? 1.f : 0.f)) > 0.01f; }

    void click(D2D1_POINT_2F) override {
        on = !on;
        if (onChange) onChange(on);
    }
};

// ------------------------------------------------------------------- Selector segmentado animado

struct Segmented : Widget {
    std::vector<std::pair<const wchar_t*, std::wstring>> items;
    int sel = 0;
    float animPos = 0.f;
    int hotIdx = -1, pressIdx = -1;
    std::function<void(int)> onChange;

    LPCWSTR cursor() const override { return enabled ? IDC_HAND : IDC_ARROW; }

    D2D1_RECT_F itemRect(float idx) const {
        float pad = 3.f;
        float w = (rc.right - rc.left - pad * 2) / std::max<size_t>(1, items.size());
        return R(rc.left + pad + w * idx, rc.top + pad, rc.left + pad + w * (idx + 1), rc.bottom - pad);
    }

    int indexAt(D2D1_POINT_2F p) const {
        for (int i = 0; i < (int)items.size(); ++i)
            if (inside(itemRect((float)i), p)) return i;
        return -1;
    }

    bool animating() const override { return Widget::animating() || std::fabs(animPos - (float)sel) > 0.005f; }

    void draw() override {
        stepHover();
        const Theme& t = A.th;
        animPos += ((float)sel - animPos) * 0.28f;
        if (std::fabs(animPos - (float)sel) < 0.005f) animPos = (float)sel;

        fillRR(rc, 6.f, t.ctl);
        controlBorder(rc, 6.f, t.ctlStroke, t.ctlStrokeBottom);

        // Hover en elemento no seleccionado
        if (isHot() && hotIdx >= 0 && hotIdx != sel) {
            fillRR(itemRect((float)hotIdx), 4.f, pressIdx == hotIdx ? t.subtlePressed : t.subtleHover);
        }

        // Indicador activo deslizante (color de acento de Windows 11)
        D2D1_RECT_F activeR = itemRect(animPos);
        fillRR(activeR, 4.f, t.accent);

        for (int i = 0; i < (int)items.size(); ++i) {
            D2D1_RECT_F r = itemRect((float)i);
            float dist = std::clamp(1.f - std::fabs(animPos - (float)i), 0.f, 1.f);
            D2D1_COLOR_F fg = mix(t.text2, t.onAccent, dist);
            if (i != sel && isHot() && hotIdx == i) fg = mix(t.text, t.onAccent, dist);
            IDWriteTextFormat* tf = (i == sel) ? A.g.fBodyBoldCtr : A.g.fBodyCenter;
            float tw = textWidth(items[i].second, tf);
            float total = 15.f + 7.f + tw;
            float x = (r.left + r.right - total) / 2.f;
            text(items[i].first, R(x, r.top, x + 15.f, r.bottom), A.g.fIcon, fg);
            text(items[i].second, R(x + 22.f, r.top, x + total + 1, r.bottom - 1), tf, fg);
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

// ------------------------------------------------------------------- Grupo de fichas (Formatos)

struct ChipGroup : Widget {
    std::vector<std::wstring> items;
    int sel = 0;
    int hotIdx = -1, pressIdx = -1;
    std::function<void(int)> onChange;

    LPCWSTR cursor() const override { return enabled ? IDC_HAND : IDC_ARROW; }

    D2D1_RECT_F itemRect(int i) const {
        if (items.empty()) return rc;
        float gap = 6.f;
        float w = (rc.right - rc.left - gap * (items.size() - 1)) / (float)items.size();
        float x = rc.left + i * (w + gap);
        return R(x, rc.top, x + w, rc.bottom);
    }

    int indexAt(D2D1_POINT_2F p) const {
        for (int i = 0; i < (int)items.size(); ++i)
            if (inside(itemRect(i), p)) return i;
        return -1;
    }

    void draw() override {
        const Theme& t = A.th;
        for (int i = 0; i < (int)items.size(); ++i) {
            D2D1_RECT_F r = itemRect(i);
            bool active = i == sel;
            bool hot = isHot() && hotIdx == i;
            bool down = pressed && pressIdx == i && hot;

            if (active) {
                fillRR(r, 5.f, t.accentSoft);
                strokeRR(r, 5.f, t.accentSoftBorder, 1.25f);
                text(items[i], R(r.left, r.top, r.right, r.bottom - 0.5f), A.g.fBodyBoldCtr, t.accentText);
            } else {
                D2D1_COLOR_F fill = down ? t.ctlPressed : hot ? t.ctlHover : t.ctl;
                fillRR(r, 5.f, fill);
                controlBorder(r, 5.f, t.ctlStroke, down ? t.ctlStroke : t.ctlStrokeBottom);
                text(items[i], R(r.left, r.top, r.right, r.bottom - 0.5f), A.g.fBodyCenter, hot ? t.text : t.text2);
            }
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

// ------------------------------------------------------------------- Desplegable Fluent (Direct2D)

struct Dropdown : Widget {
    std::vector<std::wstring> items;
    int sel = 0;
    std::function<void(int)> onChange;

    LPCWSTR cursor() const override { return enabled ? IDC_HAND : IDC_ARROW; }

    void draw() override {
        stepHover();
        const Theme& t = A.th;
        bool open = A.openDropdown == this;
        bool hot = (isHot() || open) && enabled;
        D2D1_COLOR_F fill = !enabled ? t.ctlDisabled : (pressed || open) ? t.ctlHover : mix(t.ctl, t.ctlHover, hoverAnim);
        fillRR(rc, 6.f, fill);
        if (open) {
            strokeRR(rc, 5.f, t.accentSoftBorder, 1.25f);
        } else {
            controlBorder(rc, 5.f, t.ctlStroke, pressed ? t.ctlStroke : t.ctlStrokeBottom);
        }
        D2D1_COLOR_F fg = enabled ? t.text : t.text3;
        if (sel >= 0 && sel < (int)items.size())
            text(items[sel], R(rc.left + 12, rc.top, rc.right - 34, rc.bottom - 1), A.g.fBody, fg);
        float dy = pressed ? 1.f : 0.f;
        text(open ? glyph::ChevronUp : glyph::Chevron, R(rc.right - 30, rc.top + dy, rc.right - 10, rc.bottom + dy),
             A.g.fIconSmall, enabled ? (hot ? t.text : t.text2) : t.text3);
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
    bool showClearBtn = false;
    bool clearHot = false;
    std::function<void()> onEnter;
    std::function<void()> onChange;

    static constexpr float kPad = 12.f;

    bool focusable() const override { return true; }
    bool focused() const { return A.focus == this; }

    D2D1_RECT_F clearRect() const {
        float cy = (rc.top + rc.bottom) / 2.f;
        return R(rc.right - 30.f, cy - 12.f, rc.right - 6.f, cy + 12.f);
    }

    bool hasClear() const { return showClearBtn && !value.empty(); }

    LPCWSTR cursor() const override { return (hasClear() && clearHot) ? IDC_HAND : IDC_IBEAM; }

    D2D1_RECT_F textArea() const {
        return R(rc.left + kPad, rc.top, rc.right - (hasClear() ? 34.f : kPad), rc.bottom);
    }

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
        if (onChange) onChange();
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
        copyTextToClipboard(A.hwnd, value.substr(selStart(), selEnd() - selStart()));
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
        stepHover();
        const Theme& t = A.th;
        bool f = focused();

        D2D1_COLOR_F bg = f ? t.inputActive : mix(t.inputBg, t.inputHover, hoverAnim);
        fillRR(rc, 5.f, bg);
        strokeRR(rc, 5.f, t.ctlStroke);
        setColor(f ? t.accent : mix(t.ctlStrokeBottom, t.strong, hoverAnim));
        float bw = f ? 2.f : 1.f;
        A.g.rt->DrawLine(D2D1::Point2F(rc.left + 4, rc.bottom - bw / 2.f),
                         D2D1::Point2F(rc.right - 4, rc.bottom - bw / 2.f), A.g.brush, bw);

        D2D1_RECT_F ta = textArea();
        A.g.rt->PushAxisAlignedClip(ta, D2D1_ANTIALIAS_MODE_ALIASED);
        if (value.empty()) {
            text(placeholder, R(ta.left, ta.top, ta.right, ta.bottom - 1), A.g.fBody, t.text3);
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
                    for (auto& m : ms) fillRect(R(m.left, rc.top + 7, m.left + m.width, rc.bottom - 7), t.selection);
                }
                A.g.brush2->SetColor(argb(0xFFFFFFFF));
                l->SetDrawingEffect(A.g.brush2, DWRITE_TEXT_RANGE{(UINT32)selStart(), (UINT32)(selEnd() - selStart())});
            }
            setColor(t.text);
            A.g.rt->DrawTextLayout(D2D1::Point2F(ox, oy), l, A.g.brush);
            if (f && A.caretOn && A.active) {
                float cx = std::floor(ox + caretX(l, caret)) + 0.5f;
                setColor(t.text);
                A.g.rt->DrawLine(D2D1::Point2F(cx, rc.top + 9), D2D1::Point2F(cx, rc.bottom - 9), A.g.brush, 1.f);
            }
            l->Release();
        }
        A.g.rt->PopAxisAlignedClip();

        if (hasClear()) {
            D2D1_RECT_F cr = clearRect();
            if (isHot() && clearHot) fillRR(cr, 4.f, t.subtleHover);
            text(glyph::Cancel, cr, A.g.fIconSmall, (isHot() && clearHot) ? t.text : t.text2);
        }
    }

    void mouseDown(D2D1_POINT_2F p) override {
        if (hasClear() && inside(clearRect(), p)) {
            setText(L"");
            selecting = false;
            invalidate();
            return;
        }
        caret = posAt(p);
        if (!(GetKeyState(VK_SHIFT) & 0x8000)) anchor = caret;
        selecting = true;
        A.caretOn = true;
        ensureCaretVisible();
        invalidate();
    }

    void doubleClick(D2D1_POINT_2F p) override {
        if (hasClear() && inside(clearRect(), p)) {
            setText(L"");
            return;
        }
        selectAll();
        selecting = false;
        invalidate();
    }

    void mouseMove(D2D1_POINT_2F p) override {
        bool ch = hasClear() && inside(clearRect(), p);
        if (ch != clearHot) {
            clearHot = ch;
            invalidate();
        }
        if (!selecting) return;
        caret = posAt(p);
        ensureCaretVisible();
        invalidate();
    }

    void mouseUp(D2D1_POINT_2F, bool) override { selecting = false; }

    void key(WPARAM vk) override {
        bool shift = GetKeyState(VK_SHIFT) & 0x8000;
        bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
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

// ============================================================================ Flyout Direct2D para Dropdown

constexpr float kFlyoutItemH = 32.f;
constexpr float kFlyoutPad = 5.f;

void computeFlyoutRect(Dropdown* dd) {
    D2D1_RECT_F sr = dd->screenRc();
    float w = std::max(sr.right - sr.left, 210.f);
    float fullH = dd->items.size() * kFlyoutItemH + kFlyoutPad * 2;
    float maxH = std::min(fullH, 280.f);
    float spaceBelow = A.height - sr.bottom - 12.f;
    float spaceAbove = sr.top - kTitleH - 8.f;
    float top = sr.bottom + 4.f;
    if (spaceBelow < maxH && spaceAbove > spaceBelow) {
        maxH = std::min(maxH, spaceAbove);
        top = sr.top - 4.f - maxH;
    } else {
        maxH = std::min(maxH, std::max(96.f, spaceBelow));
    }
    float left = std::clamp(sr.left, 8.f, std::max(8.f, A.width - w - 8.f));
    A.flyoutRc = R(left, top, left + w, top + maxH);
    float maxScroll = std::max(0.f, fullH - maxH);
    float selY = dd->sel * kFlyoutItemH;
    A.flyoutScroll = std::clamp(selY - maxH / 2.f + kFlyoutItemH / 2.f, 0.f, maxScroll);
}

void closeDropdown() {
    if (A.openDropdown) {
        A.openDropdown = nullptr;
        A.flyoutHot = -1;
        invalidate();
    }
}

void Dropdown::click(D2D1_POINT_2F) {
    if (A.openDropdown == this) {
        closeDropdown();
        return;
    }
    A.openDropdown = this;
    A.flyoutHot = sel;
    computeFlyoutRect(this);
    invalidate();
}

int flyoutIndexAt(D2D1_POINT_2F p) {
    if (!A.openDropdown || !inside(A.flyoutRc, p)) return -1;
    float y = p.y - (A.flyoutRc.top + kFlyoutPad) + A.flyoutScroll;
    int idx = (int)std::floor(y / kFlyoutItemH);
    if (idx < 0 || idx >= (int)A.openDropdown->items.size()) return -1;
    return idx;
}

// ============================================================================ Controles concretos

TextBox urlBox, dirBox;
Button pasteBtn, browseBtn, openDirBtn, mainBtn, secondaryBtn;
Button logBtn, logCopyBtn, logCloseBtn;
Segmented modeSeg;
Dropdown qualityDd;
ChipGroup formatChips;
Toggle playlistTg, thumbTg;

// Regiones de tarjetas y secciones
D2D1_RECT_F heroCardRc{}, configCardRc{}, destCardRc{}, footerRc{};
D2D1_RECT_F logPanelRc{}, logViewRc{};
D2D1_RECT_F modeLabelRc{}, qualityLabelRc{}, formatLabelRc{}, formatHintRc{};
D2D1_RECT_F div1Rc{}, div2Rc{}, toggleDivRc{};
D2D1_RECT_F playlistRowRc{}, thumbRowRc{};

bool showQualityOption() {
    if (A.mode == Mode::Video) return true;
    return A.audioFormat != "original" && A.audioFormat != "flac" && A.audioFormat != "wav";
}

bool showThumbOption() {
    return A.mode == Mode::Audio && A.audioFormat == "mp3";
}

std::wstring qualityLabel(const std::string& v, Mode m) {
    if (m == Mode::Video) {
        if (v == "best") return L"La mejor disponible";
        if (v == "worst") return L"La más ligera";
        if (v == "2160") return L"2160p  ·  4K Ultra HD";
        if (v == "1440") return L"1440p  ·  2K QHD";
        if (v == "1080") return L"1080p  ·  Full HD";
        if (v == "720") return L"720p  ·  HD";
        return W(v) + L"p";
    }
    if (v == "best") return L"La mejor (VBR)";
    return W(v) + L" kbps";
}

std::wstring formatLabel(const std::string& v) {
    if (v == "original") return L"Original";
    std::wstring s = W(v);
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

std::wstring formatHintText() {
    if (A.mode == Mode::Video) {
        if (A.videoFormat == "original") return L"Sin recodificar · mantiene el contenedor original";
        if (A.videoFormat == "mp4") return L"H.264 + AAC · máxima compatibilidad";
        if (A.videoFormat == "mkv") return L"Contenedor flexible sin pérdida de calidad";
        if (A.videoFormat == "webm") return L"VP9 + Opus · ligero para web";
        if (A.videoFormat == "avi") return L"Requiere recodificación con ffmpeg";
        return L"Formato clásico FLV";
    }
    if (A.audioFormat == "original") return L"Sin recodificar · audio original de YouTube";
    if (A.audioFormat == "mp3") return L"Compatible con carátula incrustada";
    if (A.audioFormat == "m4a") return L"AAC · excelente relación calidad/peso";
    if (A.audioFormat == "opus") return L"Muy eficiente a bitrates bajos";
    if (A.audioFormat == "flac" || A.audioFormat == "wav") return L"Audio sin pérdida";
    return L"Formato de audio " + formatLabel(A.audioFormat);
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

void layout();

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

    formatChips.items.clear();
    formatChips.sel = 0;
    for (size_t i = 0; i < fs.size(); ++i) {
        formatChips.items.push_back(formatLabel(fs[i]));
        if (fs[i] == f) formatChips.sel = (int)i;
    }

    bool qOk = showQualityOption();
    qualityDd.visible = qOk;
    if (!qOk && A.openDropdown == &qualityDd) closeDropdown();

    bool thumbOk = showThumbOption();
    thumbTg.visible = thumbOk;
    thumbTg.enabled = thumbOk;
    thumbTg.on = thumbOk && A.thumbnail;
    thumbTg.anim = thumbTg.on ? 1.f : 0.f;
    if (A.width > 0) layout();
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
void layout();

void joinWorker() {
    if (A.worker.joinable()) A.worker.join();
}

void startPrepare() {
    joinWorker();
    setStatus(Phase::Preparing, L"Preparando componentes…", L"Comprobando youtube-dl y ffmpeg");
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
        static const std::wregex rePct(
            LR"(^\[download\]\s+([\d.]+)%\s+of\s+~?\s*([\d.]+\s*\w+)(?:\s+at\s+(\S+))?(?:\s+ETA\s+(\S+))?)");
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
            std::wstring detail = m[2].str();
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
    closeDropdown();
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
    setStatus(Phase::Downloading, L"Obteniendo información…", warnings.empty() ? L"Conectando con YouTube…" : W(warnings.front()));
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

void toggleLogPanel(bool open) {
    A.logOpen = open;
    logBtn.activeState = open;
    logCopyBtn.visible = open;
    logCloseBtn.visible = open;
    if (open) A.logScroll = 1e9f;
    layout();
    invalidate();
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
            mainBtn.style = Button::Style::Primary;
            mainBtn.enabled = false;
            break;
        case Phase::Downloading:
            mainBtn.label = L"Cancelar";
            mainBtn.icon = glyph::Cancel;
            mainBtn.style = Button::Style::Danger;
            break;
        case Phase::ToolsFailed:
            mainBtn.label = L"Reintentar";
            mainBtn.icon = glyph::Retry;
            mainBtn.style = Button::Style::Primary;
            break;
        default:
            mainBtn.label = L"Descargar";
            mainBtn.icon = glyph::Download;
            mainBtn.style = Button::Style::Primary;
            break;
    }
    if (A.phase == Phase::Done) {
        secondaryBtn.visible = true;
        secondaryBtn.label = L"Abrir carpeta";
        secondaryBtn.icon = glyph::OpenFolder;
        secondaryBtn.style = Button::Style::Standard;
        secondaryBtn.onClick = [] { openFolder(A.lastOutputDir); };
    } else if (A.phase == Phase::Failed && !A.log.empty()) {
        secondaryBtn.visible = true;
        secondaryBtn.label = L"Ver registro";
        secondaryBtn.icon = glyph::Log;
        secondaryBtn.style = Button::Style::Standard;
        secondaryBtn.onClick = [] { toggleLogPanel(true); };
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
                          L"Calidad limitada y sin conversión de formatos");
            break;
        case Event::ToolsFail:
            setStatus(Phase::ToolsFailed, L"No se pudo preparar youtube-dl", e->detail);
            break;
        case Event::Finished:
            if (e->code == 0) {
                setStatus(Phase::Done, L"Descarga completada", e->file.empty() ? A.lastFile : e->file, 100.f);
            } else if (e->code == -2) {
                setStatus(Phase::Ready, L"Descarga cancelada", L"Puedes volver a intentarlo cuando quieras");
            } else if (e->code == -1) {
                setStatus(Phase::Failed, L"No se pudo ejecutar youtube-dl", L"Comprueba que el antivirus no lo bloquea");
            } else {
                setStatus(Phase::Failed, L"No se pudo completar la descarga",
                          e->detail.empty() ? L"youtube-dl terminó con el código " + std::to_wstring(e->code)
                                            : e->detail);
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
    float x0 = m, x1 = std::max(m + 420.f, W_ - m);
    float innerW = x1 - x0;

    // Botón de registro en la barra de título (a la izquierda de los botones nativos de ventana)
    float titleRight = A.customFrame ? (W_ - 152.f) : (W_ - 12.f);
    logBtn.rc = R(titleRight - 32.f, 7.f, titleRight, 39.f);

    // Pie fijo inferior
    footerRc = R(x0, H_ - m - kFooterH + 6.f, x1, H_ - m + 6.f);
    if (footerRc.top < kTitleH + 40.f) footerRc = R(x0, kTitleH + 40.f, x1, kTitleH + 40.f + kFooterH);

    // Panel de registro (si está abierto, se superpone sobre la zona inferior del contenido)
    float contentBottom = footerRc.top - 12.f;
    if (A.logOpen) {
        float panelH = std::clamp((footerRc.top - kTitleH) * 0.44f, 150.f, 240.f);
        logPanelRc = R(x0, footerRc.top - 10.f - panelH, x1, footerRc.top - 10.f);
        logCloseBtn.rc = R(logPanelRc.right - 34.f, logPanelRc.top + 6.f, logPanelRc.right - 6.f, logPanelRc.top + 32.f);
        logCopyBtn.rc = R(logCloseBtn.rc.left - 84.f, logPanelRc.top + 6.f, logCloseBtn.rc.left - 6.f, logPanelRc.top + 32.f);
        logViewRc = R(logPanelRc.left + 12.f, logPanelRc.top + 38.f, logPanelRc.right - 12.f, logPanelRc.bottom - 10.f);
        contentBottom = logPanelRc.top - 10.f;
    }

    A.viewport = R(0, kTitleH, W_, std::max(kTitleH + 20.f, contentBottom));

    float y = kTitleH + 8.f;

    // 1. Tarjeta Hero: Enlace de YouTube
    heroCardRc = R(x0, y, x1, y + 102.f);
    urlBox.rc = R(x0 + 16.f, y + 48.f, x1 - 16.f - 104.f, y + 86.f);
    pasteBtn.rc = R(x1 - 16.f - 96.f, y + 48.f, x1 - 16.f, y + 86.f);
    y += 102.f + 14.f;

    // 2. Tarjeta unificada de Configuración
    float cfgTop = y;
    float pad = 16.f;
    float colGap = 16.f;
    float halfW = (innerW - pad * 2 - colGap) / 2.f;

    // Fila 1: Tipo (+ Calidad si aplica al formato elegido)
    float r1Top = cfgTop + 14.f;
    float col2X = x0 + pad + halfW + colGap;
    if (showQualityOption()) {
        modeLabelRc = R(x0 + pad, r1Top, x0 + pad + halfW, r1Top + 18.f);
        modeSeg.rc = R(x0 + pad, r1Top + 24.f, x0 + pad + halfW, r1Top + 62.f);
        qualityLabelRc = R(col2X, r1Top, x1 - pad, r1Top + 18.f);
        qualityDd.rc = R(col2X, r1Top + 24.f, x1 - pad, r1Top + 62.f);
    } else {
        modeLabelRc = R(x0 + pad, r1Top, x1 - pad, r1Top + 18.f);
        modeSeg.rc = R(x0 + pad, r1Top + 24.f, x1 - pad, r1Top + 62.f);
        qualityLabelRc = R(0, 0, 0, 0);
        qualityDd.rc = R(0, 0, 0, 0);
    }

    // Divisor 1
    float d1Y = r1Top + 78.f;
    div1Rc = R(x0 + pad, d1Y, x1 - pad, d1Y + 1.f);

    // Fila 2: Formato (fichas de 1 clic)
    float r2Top = d1Y + 14.f;
    formatLabelRc = R(x0 + pad, r2Top, x0 + pad + 180.f, r2Top + 18.f);
    formatHintRc = R(x0 + pad + 180.f, r2Top, x1 - pad, r2Top + 18.f);
    formatChips.rc = R(x0 + pad, r2Top + 24.f, x1 - pad, r2Top + 58.f);

    // Divisor 2
    float d2Y = r2Top + 74.f;
    div2Rc = R(x0 + pad, d2Y, x1 - pad, d2Y + 1.f);

    // Fila 3: Interruptores disponibles
    float r3Top = d2Y + 8.f;
    float r3H = 54.f;
    if (showThumbOption()) {
        playlistRowRc = R(x0 + pad, r3Top, x0 + pad + halfW - 4.f, r3Top + r3H);
        playlistTg.rc = R(playlistRowRc.right - 46.f, r3Top + 11.f, playlistRowRc.right, r3Top + 43.f);

        float midX = x0 + innerW / 2.f;
        toggleDivRc = R(midX, r3Top + 8.f, midX + 1.f, r3Top + r3H - 8.f);

        thumbRowRc = R(col2X + 4.f, r3Top, x1 - pad, r3Top + r3H);
        thumbTg.rc = R(thumbRowRc.right - 46.f, r3Top + 11.f, thumbRowRc.right, r3Top + 43.f);
    } else {
        playlistRowRc = R(x0 + pad, r3Top, x1 - pad, r3Top + r3H);
        playlistTg.rc = R(playlistRowRc.right - 46.f, r3Top + 11.f, playlistRowRc.right, r3Top + 43.f);
        toggleDivRc = R(0, 0, 0, 0);
        thumbRowRc = R(0, 0, 0, 0);
        thumbTg.rc = R(0, 0, 0, 0);
    }

    configCardRc = R(x0, cfgTop, x1, r3Top + r3H + 10.f);
    y = configCardRc.bottom + 14.f;

    // 3. Tarjeta de Destino
    destCardRc = R(x0, y, x1, y + 92.f);
    dirBox.rc = R(x0 + 16.f, y + 42.f, x1 - 16.f - 104.f - 44.f, y + 78.f);
    browseBtn.rc = R(x1 - 16.f - 96.f - 44.f, y + 42.f, x1 - 16.f - 44.f, y + 78.f);
    openDirBtn.rc = R(x1 - 16.f - 36.f, y + 42.f, x1 - 16.f, y + 78.f);
    y += 92.f + 12.f;

    A.contentH = y;
    urlBox.relayout();
    dirBox.relayout();
    float maxScroll = std::max(0.f, A.contentH - A.viewport.bottom);
    A.scroll = std::clamp(A.scroll, 0.f, maxScroll);

    // Botones del pie
    float by = (footerRc.top + footerRc.bottom) / 2.f - 19.f;
    mainBtn.rc = R(footerRc.right - 16.f - 138.f, by, footerRc.right - 16.f, by + 38.f);
    secondaryBtn.rc = R(mainBtn.rc.left - 8.f - 138.f, by, mainBtn.rc.left - 8.f, by + 38.f);

    if (A.openDropdown) computeFlyoutRect(A.openDropdown);
}

// ============================================================================ Pintado

void drawTitleBar() {
    const Theme& t = A.th;
    D2D1_COLOR_F fg = A.active ? t.text : t.text3;

    if (A.g.icon) {
        A.g.rt->DrawBitmap(A.g.icon, R(16.f, 13.f, 36.f, 33.f), A.active ? 1.f : 0.6f);
    }
    text(L"YtDownloader", R(44.f, 0.f, A.width - 240.f, kTitleH), A.g.fTitle, fg);
}

void drawHeroCard() {
    const Theme& t = A.th;
    drawSurface(heroCardRc, 8.f, true);

    D2D1_RECT_F badge = R(heroCardRc.left + 16.f, heroCardRc.top + 12.f, heroCardRc.left + 44.f, heroCardRc.top + 40.f);
    fillRR(badge, 6.f, t.accentSoft);
    text(glyph::Link, badge, A.g.fIcon, t.accentText);

    text(L"Enlace de YouTube", R(heroCardRc.left + 54.f, heroCardRc.top + 12.f, heroCardRc.right - 200.f, heroCardRc.top + 40.f),
         A.g.fBodyStrong, t.text);

    std::wstring hint = !A.urlInfo.badge.empty() ? A.urlInfo.badge : L"Ctrl+V para pegar · Intro para bajar";
    D2D1_COLOR_F hc = A.urlInfo.isPlaylist ? t.warning : (!A.urlInfo.badge.empty() ? t.accentText : t.text3);
    text(hint, R(heroCardRc.left + 200.f, heroCardRc.top + 12.f, heroCardRc.right - 16.f, heroCardRc.top + 40.f),
         A.g.fCaptionRight, hc);
}

void drawConfigCard() {
    const Theme& t = A.th;
    drawSurface(configCardRc, 8.f, false);

    // Etiquetas fila 1
    text(L"Tipo de descarga", modeLabelRc, A.g.fCaptionStrong, t.text2);
    if (showQualityOption()) {
        text(A.mode == Mode::Video ? L"Resolución máxima" : L"Calidad de audio", qualityLabelRc, A.g.fCaptionStrong, t.text2);
    }

    // Divisor 1
    fillRect(div1Rc, t.separator);

    // Etiquetas fila 2
    text(L"Formato de salida", formatLabelRc, A.g.fCaptionStrong, t.text2);
    text(formatHintText(), formatHintRc, A.g.fCaptionRight, t.text3);

    // Divisor 2
    fillRect(div2Rc, t.separator);

    // Fila 3: Lista de reproducción
    bool plHighlight = A.urlInfo.isPlaylist && !A.playlist;
    if (plHighlight) {
        fillRR(inflate(playlistRowRc, 4.f, -2.f), 6.f, withAlpha(t.warning, t.dark ? 0.10f : 0.08f));
    }
    D2D1_RECT_F plIcon = R(playlistRowRc.left, playlistRowRc.top + 13.f, playlistRowRc.left + 28.f, playlistRowRc.top + 41.f);
    fillRR(plIcon, 6.f, plHighlight ? t.warningSoft : t.ctl);
    text(glyph::List, plIcon, A.g.fIcon, plHighlight ? t.warning : t.text2);

    float plTx = playlistRowRc.left + 38.f;
    float plRight = playlistTg.rc.left - 8.f;
    text(L"Lista completa", R(plTx, playlistRowRc.top + 8.f, plRight, playlistRowRc.top + 28.f), A.g.fBodyStrong, t.text);
    text(plHighlight ? L"Lista detectada en el enlace" : L"Si el enlace pertenece a una lista, descargarla entera",
         R(plTx, playlistRowRc.top + 27.f, plRight, playlistRowRc.top + 45.f), A.g.fCaption,
         plHighlight ? t.warning : t.text2);

    // Carátula MP3 (solo se muestra cuando está disponible)
    if (showThumbOption()) {
        fillRect(toggleDivRc, t.separator);

        D2D1_RECT_F thIcon = R(thumbRowRc.left, thumbRowRc.top + 13.f, thumbRowRc.left + 28.f, thumbRowRc.top + 41.f);
        fillRR(thIcon, 6.f, t.ctl);
        text(glyph::Photo, thIcon, A.g.fIcon, t.text2);

        float thTx = thumbRowRc.left + 38.f;
        float thRight = thumbTg.rc.left - 8.f;
        text(L"Incrustar carátula", R(thTx, thumbRowRc.top + 8.f, thRight, thumbRowRc.top + 28.f), A.g.fBodyStrong, t.text);
        text(L"Miniatura como portada MP3", R(thTx, thumbRowRc.top + 27.f, thRight, thumbRowRc.top + 45.f), A.g.fCaption, t.text2);
    }
}

void drawDestCard() {
    const Theme& t = A.th;
    drawSurface(destCardRc, 8.f, false);

    D2D1_RECT_F iconR = R(destCardRc.left + 16.f, destCardRc.top + 11.f, destCardRc.left + 40.f, destCardRc.top + 35.f);
    fillRR(iconR, 6.f, t.ctl);
    text(glyph::Folder, iconR, A.g.fIconSmall, t.text2);
    text(L"Carpeta de destino", R(destCardRc.left + 48.f, destCardRc.top + 11.f, destCardRc.right - 16.f, destCardRc.top + 35.f),
         A.g.fCaptionStrong, t.text2);
}

void drawFooter() {
    const Theme& t = A.th;
    D2D1_RECT_F r = footerRc;
    drawSurface(r, 8.f, true);

    const wchar_t* icon = glyph::Info;
    D2D1_COLOR_F ic = t.accentText;
    D2D1_COLOR_F badgeBg = t.accentSoft;
    switch (A.phase) {
        case Phase::Done:
            icon = glyph::Done;
            ic = t.success;
            badgeBg = t.successSoft;
            break;
        case Phase::Failed:
        case Phase::ToolsFailed:
            icon = glyph::Error;
            ic = t.error;
            badgeBg = t.errorSoft;
            break;
        case Phase::Preparing:
        case Phase::Downloading:
            icon = A.percent >= 0 ? glyph::Download : glyph::Sync;
            ic = t.accentText;
            badgeBg = t.accentSoft;
            break;
        default:
            icon = glyph::Info;
            ic = t.accentText;
            badgeBg = t.accentSoft;
            break;
    }

    float right = (secondaryBtn.visible ? secondaryBtn.rc.left : mainBtn.rc.left) - 18.f;
    bool working = A.phase == Phase::Preparing || A.phase == Phase::Downloading;
    float cy = working ? r.top + 34.f : (r.top + r.bottom) / 2.f;

    D2D1_RECT_F iconBg = R(r.left + 18.f, cy - 18.f, r.left + 54.f, cy + 18.f);
    fillRR(iconBg, 18.f, badgeBg);
    text(icon, iconBg, A.g.fIconBig, ic);

    float tx = r.left + 66.f;
    float titleRight = right;
    if (working && A.percent >= 0) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.1f %%", std::clamp(A.progressShown, 0.f, 100.f));
        std::wstring pctStr(buf);
        std::replace(pctStr.begin(), pctStr.end(), L'.', L',');
        float pw = textWidth(pctStr, A.g.fBodyStrong) + 8.f;
        text(pctStr, R(right - pw, cy - 20.f, right, cy + 2.f), A.g.fBodyRight, t.accentText);
        titleRight = right - pw - 6.f;
    }

    text(A.statusTitle, R(tx, cy - 20.f, titleRight, cy + 2.f), A.g.fBodyStrong, t.text);
    text(A.statusDetail, R(tx, cy + 2.f, right, cy + 20.f), A.g.fCaption, t.text2);

    if (working) {
        // Barra de progreso Fluent de Windows 11 (4px redondeada con color de acento del sistema)
        float px0 = tx, px1 = right, py = r.bottom - 18.f;
        fillRR(R(px0, py - 1.5f, px1, py + 1.5f), 1.5f, t.trackBg);
        float w = px1 - px0;
        if (w > 4.f) {
            if (A.percent >= 0) {
                float v = std::clamp(A.progressShown / 100.f, 0.f, 1.f);
                if (v > 0.f) {
                    float fillW = std::max(4.f, w * v);
                    fillRR(R(px0, py - 2.f, px0 + fillW, py + 2.f), 2.f, t.accent);
                }
            } else {
                double ph = std::fmod(nowSeconds() - A.animStart, 1.5) / 1.5;
                float seg = w * 0.32f;
                float sx = (float)(px0 - seg + (w + seg) * ph);
                float a = std::max(px0, sx), b = std::min(px1, sx + seg);
                if (b > a) fillRR(R(a, py - 2.f, b, py + 2.f), 2.f, t.accent);
            }
        }
    }
}

void drawLogPanel() {
    if (!A.logOpen) return;
    const Theme& t = A.th;
    fillRR(logPanelRc, 10.f, t.consoleBg);
    strokeRR(logPanelRc, 10.f, t.consoleBorder);

    // Cabecera de consola
    text(glyph::Log, R(logPanelRc.left + 12.f, logPanelRc.top + 8.f, logPanelRc.left + 32.f, logPanelRc.top + 30.f),
         A.g.fIconSmall, argb(0xFF9AA4B2));
    text(L"Registro de youtube-dl",
         R(logPanelRc.left + 34.f, logPanelRc.top + 8.f, logCopyBtn.rc.left - 12.f, logPanelRc.top + 30.f),
         A.g.fCaptionStrong, argb(0xFFE6EAF0));

    if (A.copiedToastAlpha > 0.01f) {
        text(L"Copiado",
             R(logCopyBtn.rc.left - 76.f, logPanelRc.top + 8.f, logCopyBtn.rc.left - 10.f, logPanelRc.top + 30.f),
             A.g.fCaptionRight, withAlpha(t.success, A.copiedToastAlpha));
    }

    fillRect(R(logPanelRc.left + 1.f, logPanelRc.top + 36.f, logPanelRc.right - 1.f, logPanelRc.top + 37.f),
             argb(0x1AFFFFFF));

    // Líneas de registro
    constexpr float lineH = 18.f;
    float viewH = logViewRc.bottom - logViewRc.top;
    float totalH = A.log.size() * lineH;
    float maxScroll = std::max(0.f, totalH - viewH);
    A.logScroll = std::clamp(A.logScroll, 0.f, maxScroll);

    A.g.rt->PushAxisAlignedClip(logViewRc, D2D1_ANTIALIAS_MODE_ALIASED);
    if (A.log.empty()) {
        text(L"(Sin salida todavía — inicia una descarga para ver el registro en vivo)",
             R(logViewRc.left + 4.f, logViewRc.top + 4.f, logViewRc.right - 4.f, logViewRc.top + 26.f),
             A.g.fMono, argb(0x70FFFFFF));
    } else {
        size_t first = (size_t)std::max(0.f, std::floor(A.logScroll / lineH));
        size_t last = std::min(A.log.size(), first + (size_t)std::ceil(viewH / lineH) + 2);
        for (size_t i = first; i < last; ++i) {
            float y = logViewRc.top + i * lineH - A.logScroll;
            const std::wstring& ln = A.log[i];
            D2D1_COLOR_F col = argb(0xD8DCE3EC);
            if (ln.rfind(L"ERROR:", 0) == 0) col = argb(0xFFFF7A85);
            else if (ln.rfind(L"Aviso:", 0) == 0 || ln.rfind(L"WARNING:", 0) == 0) col = argb(0xFFFFC155);
            else if (ln.rfind(L"[ffmpeg]", 0) == 0 || ln.rfind(L"[ExtractAudio]", 0) == 0) col = argb(0xFF7CD4FD);
            else if (ln.rfind(L"[download]", 0) == 0) col = argb(0xFF7EE787);
            text(ln, R(logViewRc.left + 2.f, y, logViewRc.right - 10.f, y + lineH), A.g.fMono, col);
        }
    }
    A.g.rt->PopAxisAlignedClip();

    if (maxScroll > 0.f) {
        float th = std::max(22.f, viewH * viewH / totalH);
        float ty = logViewRc.top + (viewH - th) * (A.logScroll / maxScroll);
        fillRR(R(logViewRc.right - 4.f, ty, logViewRc.right, ty + th), 2.f, argb(0x55FFFFFF));
    }

    logCopyBtn.draw();
    logCloseBtn.draw();
}

void drawDropdownFlyout() {
    if (!A.openDropdown) return;
    const Theme& t = A.th;
    Dropdown* dd = A.openDropdown;
    D2D1_RECT_F r = A.flyoutRc;

    // Sombra suave multicapa
    fillRR(R(r.left - 3, r.top + 2, r.right + 3, r.bottom + 8), 11.f, argb(t.dark ? 0x35000000 : 0x12000000));
    fillRR(R(r.left - 1, r.top + 1, r.right + 1, r.bottom + 4), 9.f, argb(t.dark ? 0x45000000 : 0x16000000));

    fillRR(r, 8.f, t.flyoutBg);
    strokeRR(r, 8.f, t.flyoutBorder);

    D2D1_RECT_F clip = R(r.left + 2, r.top + kFlyoutPad, r.right - 2, r.bottom - kFlyoutPad);
    A.g.rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < (int)dd->items.size(); ++i) {
        float iy = r.top + kFlyoutPad + i * kFlyoutItemH - A.flyoutScroll;
        if (iy + kFlyoutItemH < clip.top || iy > clip.bottom) continue;
        D2D1_RECT_F ir = R(r.left + 5.f, iy + 1.f, r.right - 5.f, iy + kFlyoutItemH - 1.f);
        bool selected = i == dd->sel;
        bool hot = i == A.flyoutHot;
        if (selected || hot) {
            fillRR(ir, 4.f, t.subtleHover);
        }
        if (selected) {
            fillRR(R(ir.left + 1.f, ir.top + 7.f, ir.left + 4.f, ir.bottom - 7.f), 1.5f, t.accent);
            text(glyph::Check, R(ir.right - 26.f, ir.top, ir.right - 8.f, ir.bottom), A.g.fIconSmall, t.accentText);
        }
        text(dd->items[i], R(ir.left + 12.f, ir.top, ir.right - 28.f, ir.bottom - 0.5f),
             selected ? A.g.fBodyStrong : A.g.fBody, selected ? t.text : t.text);
    }
    A.g.rt->PopAxisAlignedClip();

    float totalH = dd->items.size() * kFlyoutItemH + kFlyoutPad * 2;
    float viewH = r.bottom - r.top;
    if (totalH > viewH) {
        float th = std::max(24.f, viewH * viewH / totalH);
        float maxS = totalH - viewH;
        float ty = r.top + 4.f + (viewH - 8.f - th) * (A.flyoutScroll / maxS);
        fillRR(R(r.right - 5.f, ty, r.right - 2.f, ty + th), 1.5f, t.strong);
    }
}

void drawTooltip() {
    if (!A.hot || A.hot->tooltip.empty() || A.openDropdown || A.captured) return;
    if (nowSeconds() - A.hotSince < 0.45) return;
    const Theme& t = A.th;
    float tw = textWidth(A.hot->tooltip, A.g.fCaption);
    float pw = tw + 18.f;
    float ph = 26.f;
    D2D1_RECT_F sr = A.hot->screenRc();
    float cx = (sr.left + sr.right) / 2.f;
    float x = std::clamp(cx - pw / 2.f, 8.f, std::max(8.f, A.width - pw - 8.f));
    float y = sr.bottom + 6.f;
    if (y + ph > A.height - 8.f) y = sr.top - ph - 6.f;
    D2D1_RECT_F tr = R(x, y, x + pw, y + ph);
    fillRR(R(tr.left, tr.top + 2, tr.right, tr.bottom + 3), 6.f, argb(t.dark ? 0x38000000 : 0x16000000));
    fillRR(tr, 6.f, t.tooltipBg);
    strokeRR(tr, 6.f, t.tooltipBorder);
    text(A.hot->tooltip, R(tr.left + 9.f, tr.top, tr.right - 9.f, tr.bottom - 0.5f), A.g.fCaption, t.text);
}

void paint() {
    if (!A.g.rt) return;
    const Theme& t = A.th;
    A.g.rt->BeginDraw();
    A.g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    A.g.rt->Clear(A.mica ? D2D1::ColorF(0, 0, 0, 0) : t.solidBg);

    drawTitleBar();
    logBtn.draw();

    // Contenido desplazable
    A.g.rt->PushAxisAlignedClip(A.viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    A.g.rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -A.scroll));
    drawHeroCard();
    drawConfigCard();
    drawDestCard();
    for (Widget* w : A.widgets)
        if (w->visible && w->inContent) w->draw();
    A.g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    A.g.rt->PopAxisAlignedClip();

    // Indicador de scroll
    float maxScroll = A.contentH - A.viewport.bottom;
    if (maxScroll > 0.f) {
        float vh = A.viewport.bottom - A.viewport.top;
        float total = A.contentH - A.viewport.top;
        float th = std::max(24.f, vh * vh / total);
        float ty = A.viewport.top + (vh - th) * (A.scroll / maxScroll);
        fillRR(R(A.width - 7.f, ty + 2.f, A.width - 4.f, ty + th - 2.f), 1.5f, t.strong);
    }

    drawLogPanel();
    drawFooter();
    for (Widget* w : A.widgets) {
        if (!w->visible || w->inContent) continue;
        if (w == &logBtn || w == &logCopyBtn || w == &logCloseBtn) continue;
        w->draw();
    }

    drawDropdownFlyout();
    drawTooltip();

    HRESULT hr = A.g.rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        release(A.g.icon);
        release(A.g.brush);
        release(A.g.brush2);
        release(A.g.rt);
    }
}

bool anyAnimating() {
    if (A.phase == Phase::Preparing || A.phase == Phase::Downloading) return true;
    if (A.copiedToastAlpha > 0.01f) return true;
    if (A.hot && !A.hot->tooltip.empty() && !A.openDropdown && (nowSeconds() - A.hotSince < 0.55)) return true;
    for (Widget* w : A.widgets)
        if (w->visible && w->animating()) return true;
    return false;
}

void tickAnimations() {
    if (A.percent >= 0) {
        A.progressShown += (A.percent - A.progressShown) * 0.25f;
        if (std::fabs(A.progressShown - A.percent) < 0.1f) A.progressShown = A.percent;
    }
    if (A.copiedToastAlpha > 0.f) {
        A.copiedToastAlpha = std::max(0.f, A.copiedToastAlpha - 0.025f);
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
    const wchar_t* icons = fontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const wchar_t* mono = fontExists(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";

    A.g.fBody = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fBodyStrong = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fBodyCenter = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    A.g.fBodyBoldCtr = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    A.g.fBodyRight = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING, false);
    A.g.fCaption = makeFormat(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fCaptionStrong = makeFormat(ui, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fCaptionRight = makeFormat(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    A.g.fBadge = makeFormat(ui, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    A.g.fTitle = makeFormat(ui, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fInput = makeFormat(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    A.g.fMono = makeFormat(mono, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    A.g.fIcon = makeFormat(icons, 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    A.g.fIconBig = makeFormat(icons, 18, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    A.g.fIconSmall = makeFormat(icons, 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
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
        HICON hi = (HICON)LoadImageW(A.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, (int)dip2px(20), (int)dip2px(20), 0);
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

D2D1_POINT_2F local(Widget* w, D2D1_POINT_2F p) {
    return w && w->inContent ? D2D1::Point2F(p.x, p.y + A.scroll) : p;
}

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
    // Barra de título
    logBtn.inContent = false;
    logBtn.style = Button::Style::Subtle;
    logBtn.icon = glyph::Log;
    logBtn.tooltip = L"Mostrar u ocultar registro (Ctrl+L)";
    logBtn.onClick = [] { toggleLogPanel(!A.logOpen); };

    logCopyBtn.inContent = false;
    logCopyBtn.visible = false;
    logCopyBtn.style = Button::Style::Subtle;
    logCopyBtn.onDarkSurface = true;
    logCopyBtn.icon = glyph::Copy;
    logCopyBtn.label = L"Copiar";
    logCopyBtn.onClick = [] {
        std::wstring all;
        for (const auto& l : A.log) all += l + L"\r\n";
        copyTextToClipboard(A.hwnd, all);
        A.copiedToastAlpha = 1.f;
        ensureAnimTimer();
        invalidate();
    };

    logCloseBtn.inContent = false;
    logCloseBtn.visible = false;
    logCloseBtn.style = Button::Style::Subtle;
    logCloseBtn.onDarkSurface = true;
    logCloseBtn.icon = glyph::Cancel;
    logCloseBtn.tooltip = L"Cerrar registro";
    logCloseBtn.onClick = [] { toggleLogPanel(false); };

    // Hero URL
    urlBox.placeholder = L"https://www.youtube.com/watch?v=…";
    urlBox.showClearBtn = true;
    urlBox.onChange = [] {
        A.urlInfo = analyzeUrl(urlBox.value);
        invalidate();
    };
    urlBox.onEnter = [] {
        if (A.phase != Phase::Downloading && A.phase != Phase::Preparing) startDownload();
    };

    pasteBtn.label = L"Pegar";
    pasteBtn.icon = glyph::Paste;
    pasteBtn.tooltip = L"Pegar enlace desde el portapapeles (Ctrl+V)";
    pasteBtn.onClick = [] {
        urlBox.setText(trim(TextBox::clipboardText()));
        setFocus(&urlBox);
    };

    // Configuración
    modeSeg.items = {{glyph::Video, L"Vídeo"}, {glyph::Audio, L"Audio"}};
    modeSeg.onChange = [](int i) {
        A.mode = i == 1 ? Mode::Audio : Mode::Video;
        closeDropdown();
        refreshOptions();
        persist();
    };

    qualityDd.onChange = [](int i) {
        if (A.mode == Mode::Video) A.videoQuality = videoQualities()[i];
        else A.audioQuality = audioQualities()[i];
        persist();
    };

    formatChips.onChange = [](int i) {
        if (A.mode == Mode::Video) A.videoFormat = videoFormats()[i];
        else A.audioFormat = audioFormats()[i];
        refreshOptions();
        persist();
    };

    playlistTg.onChange = [](bool on) { A.playlist = on; };
    thumbTg.onChange = [](bool on) {
        A.thumbnail = on;
        persist();
    };

    // Destino
    dirBox.placeholder = L"Carpeta donde guardar las descargas";
    browseBtn.label = L"Examinar";
    browseBtn.onClick = chooseFolder;

    openDirBtn.icon = glyph::OpenFolder;
    openDirBtn.tooltip = L"Abrir carpeta en el Explorador";
    openDirBtn.onClick = [] {
        std::wstring d = trim(dirBox.value);
        std::error_code ec;
        fs::create_directories(fs::path(d), ec);
        openFolder(d);
    };

    // Pie
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

    A.widgets = {&urlBox,  &pasteBtn,  &modeSeg,    &qualityDd,   &formatChips, &playlistTg, &thumbTg,
                 &dirBox,  &browseBtn, &openDirBtn, &secondaryBtn, &mainBtn,    &logBtn,     &logCopyBtn,
                 &logCloseBtn};

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
    modeSeg.animPos = (float)modeSeg.sel;
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
    if (pt.y < dip2px(kTitleH)) {
        D2D1_POINT_2F dipPt = D2D1::Point2F(px2dip((float)pt.x), px2dip((float)pt.y));
        if (inside(logBtn.rc, dipPt)) return HTCLIENT;
        return HTCAPTION;
    }
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
            if (!A.active) closeDropdown();
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
            mm->ptMinTrackSize.x = (LONG)dip2px(600);
            mm->ptMinTrackSize.y = (LONG)dip2px(520);
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
            A.mousePos = p;
            if (!A.trackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                A.trackingMouse = true;
            }
            if (A.openDropdown) {
                int fh = flyoutIndexAt(p);
                if (fh != A.flyoutHot) {
                    A.flyoutHot = fh;
                    invalidate();
                }
                return 0;
            }
            Widget* w = A.captured ? A.captured : widgetAt(p);
            Widget* hot = A.captured ? (widgetAt(p) == A.captured ? A.captured : nullptr) : w;
            if (hot && !hot->enabled) hot = nullptr;
            if (hot != A.hot) {
                A.hot = hot;
                A.hotSince = nowSeconds();
                ensureAnimTimer();
                invalidate();
            }
            if (w && w->enabled) w->mouseMove(local(w, p));
            return 0;
        }
        case WM_MOUSELEAVE:
            A.trackingMouse = false;
            if (A.openDropdown) {
                A.flyoutHot = -1;
                invalidate();
            }
            if (A.hot && !A.captured) {
                A.hot = nullptr;
                ensureAnimTimer();
                invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            D2D1_POINT_2F p = mousePoint(lp);
            if (A.openDropdown) {
                if (inside(A.flyoutRc, p)) {
                    int idx = flyoutIndexAt(p);
                    if (idx >= 0) {
                        Dropdown* dd = A.openDropdown;
                        closeDropdown();
                        if (idx != dd->sel) {
                            dd->sel = idx;
                            if (dd->onChange) dd->onChange(idx);
                        }
                        invalidate();
                    }
                    return 0;
                }
                Dropdown* prev = A.openDropdown;
                closeDropdown();
                // Si el clic fue en el mismo dropdown, simplemente lo cerramos.
                if (widgetAt(p) == prev) return 0;
            }

            Widget* w = widgetAt(p);
            if (!w && inside(A.viewport, p)) {
                // Permitir hacer clic en toda la fila del interruptor para alternarlo.
                D2D1_POINT_2F cp = D2D1::Point2F(p.x, p.y + A.scroll);
                if (inside(playlistRowRc, cp) && playlistTg.visible && playlistTg.enabled) w = &playlistTg;
                else if (inside(thumbRowRc, cp) && thumbTg.visible && thumbTg.enabled) w = &thumbTg;
            }
            if (w && !w->enabled) w = nullptr;
            setFocus(w && w->focusable() ? w : nullptr);
            if (w) {
                A.captured = w;
                SetCapture(hwnd);
                if (msg == WM_LBUTTONDBLCLK) w->doubleClick(local(w, p));
                else w->mouseDown(local(w, p));
                A.hot = w;
                ensureAnimTimer();
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
                bool over = (widgetAt(p) == w);
                if (!over && inside(A.viewport, p)) {
                    D2D1_POINT_2F cp = D2D1::Point2F(p.x, p.y + A.scroll);
                    if (w == &playlistTg && inside(playlistRowRc, cp)) over = true;
                    else if (w == &thumbTg && inside(thumbRowRc, cp)) over = true;
                }
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
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            D2D1_POINT_2F dp = D2D1::Point2F(px2dip((float)pt.x), px2dip((float)pt.y));
            float delta = GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 48.f;

            if (A.openDropdown && inside(A.flyoutRc, dp)) {
                float totalH = A.openDropdown->items.size() * kFlyoutItemH + kFlyoutPad * 2;
                float maxS = std::max(0.f, totalH - (A.flyoutRc.bottom - A.flyoutRc.top));
                A.flyoutScroll = std::clamp(A.flyoutScroll - delta, 0.f, maxS);
                A.flyoutHot = flyoutIndexAt(dp);
                invalidate();
                return 0;
            }
            if (A.logOpen && inside(logPanelRc, dp)) {
                float totalH = A.log.size() * 18.f;
                float maxS = std::max(0.f, totalH - (logViewRc.bottom - logViewRc.top));
                A.logScroll = std::clamp(A.logScroll - delta, 0.f, maxS);
                invalidate();
                return 0;
            }
            closeDropdown();
            float maxScroll = std::max(0.f, A.contentH - A.viewport.bottom);
            A.scroll = std::clamp(A.scroll - delta, 0.f, maxScroll);
            A.hot = widgetAt(dp);
            invalidate();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                D2D1_POINT_2F dp = D2D1::Point2F(px2dip((float)pt.x), px2dip((float)pt.y));
                if (A.openDropdown) {
                    SetCursor(LoadCursorW(nullptr, inside(A.flyoutRc, dp) ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                Widget* w = widgetAt(dp);
                if (!w && inside(A.viewport, dp)) {
                    D2D1_POINT_2F cp = D2D1::Point2F(dp.x, dp.y + A.scroll);
                    if ((inside(playlistRowRc, cp) && playlistTg.visible && playlistTg.enabled) ||
                        (inside(thumbRowRc, cp) && thumbTg.visible && thumbTg.enabled)) {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
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
            if (A.openDropdown) {
                Dropdown* dd = A.openDropdown;
                if (wp == VK_ESCAPE) {
                    closeDropdown();
                } else if (wp == VK_DOWN) {
                    A.flyoutHot = std::min((int)dd->items.size() - 1, A.flyoutHot + 1);
                    invalidate();
                } else if (wp == VK_UP) {
                    A.flyoutHot = std::max(0, A.flyoutHot - 1);
                    invalidate();
                } else if (wp == VK_RETURN && A.flyoutHot >= 0) {
                    int idx = A.flyoutHot;
                    closeDropdown();
                    if (idx != dd->sel) {
                        dd->sel = idx;
                        if (dd->onChange) dd->onChange(idx);
                    }
                    invalidate();
                }
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'L') {
                toggleLogPanel(!A.logOpen);
                return 0;
            }
            if (wp == VK_TAB) {
                Widget* next = A.focus == &urlBox ? (Widget*)&dirBox : (Widget*)&urlBox;
                setFocus(next);
                if (auto* tb = dynamic_cast<TextBox*>(next)) tb->selectAll();
                scrollIntoView(next);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (A.logOpen) toggleLogPanel(false);
                else if (A.focus) setFocus(nullptr);
                return 0;
            }
            if (A.focus) A.focus->key(wp);
            return 0;
        case WM_CHAR:
            if (wp == L'\t' || wp == 27 || wp == 12 /*Ctrl+L*/) return 0;
            if (A.openDropdown) return 0;
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

    // Tamaño inicial equilibrado: 740x670 DIP, todo visible sin necesidad de scroll.
    POINT origin{0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT sysDpi = GetDpiForSystem();
    int w = MulDiv(740, sysDpi, 96), h = MulDiv(670, sysDpi, 96);
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

    if (A.worker.joinable()) {
        if (A.busy) A.worker.detach();
        else A.worker.join();
    }
    A.hwnd = nullptr;
    CoUninitialize();
    return 0;
}

#endif  // _WIN32
