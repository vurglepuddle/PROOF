// SPDX-License-Identifier: GPL-2.0-or-later
#include "startup-splash.h"

#ifdef _WIN32
// The editor normally hides GDI declarations to avoid name collisions. This
// isolated native-window implementation needs them and includes no editor types.
#undef NOGDI
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
#include <cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <windows.h>
#undef near

namespace Inkscape::UI {

struct StartupSplash::Impl
{
    static constexpr int WIDTH = 680;
    static constexpr int HEIGHT = 440;
    static constexpr int PICTURE_HEIGHT = 384;
    static constexpr UINT UPDATE_MESSAGE = WM_APP + 1;

    std::atomic<HWND> window = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::wstring message = L"Loading PROOF\u2026";
    cairo_surface_t *picture = nullptr; // Used only by the splash thread.
    HFONT font = nullptr;
    double scale = 1;
    unsigned tick = 0;

    explicit Impl(std::string const &path)
    {
        std::promise<void> ready;
        auto future = ready.get_future();
        thread = std::thread([this, path, ready = std::move(ready)]() mutable {
            // Decode our own PNG, without involving GTK or the document renderer.
            if (auto file = g_fopen(path.c_str(), "rb")) {
                picture = cairo_image_surface_create_from_png_stream([](void *data, unsigned char *buffer, unsigned length) {
                    return std::fread(buffer, 1, length, static_cast<FILE *>(data)) == length
                        ? CAIRO_STATUS_SUCCESS : CAIRO_STATUS_READ_ERROR;
                }, file);
                std::fclose(file);
            }

            // Match GTK's logical size on the monitor where the person launched us.
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            POINT pointer;
            GetCursorPos(&pointer);
            MONITORINFO monitor{sizeof(MONITORINFO)};
            GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST), &monitor);

            auto instance = GetModuleHandleW(nullptr);
            WNDCLASSW klass{};
            klass.lpfnWndProc = procedure;
            klass.hInstance = instance;
            klass.hCursor = LoadCursor(nullptr, IDC_APPSTARTING);
            klass.lpszClassName = L"PROOFStartupSplash";
            RegisterClassW(&klass);

            auto hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, klass.lpszClassName,
                                       L"PROOF is loading", WS_POPUP, monitor.rcWork.left, monitor.rcWork.top,
                                       WIDTH, HEIGHT, nullptr, nullptr, instance, this);
            window.store(hwnd);
            if (hwnd) {
                scale = GetDpiForWindow(hwnd) / 96.0;
                // Keep the whole splash inside small work areas as well.
                scale = std::min({scale, (monitor.rcWork.right - monitor.rcWork.left) / double(WIDTH),
                                        (monitor.rcWork.bottom - monitor.rcWork.top) / double(HEIGHT)});
                int width = px(WIDTH), height = px(HEIGHT);
                font = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH, L"Segoe UI");
                // A rectangular region also prevents Windows 11 from rounding the corners.
                auto region = CreateRectRgn(0, 0, width, height);
                if (!SetWindowRgn(hwnd, region, FALSE)) DeleteObject(region);
                SetWindowPos(hwnd, nullptr,
                             monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                             monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
                             width, height, SWP_NOZORDER | SWP_NOACTIVATE);
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                UpdateWindow(hwnd);
                SetTimer(hwnd, 1, 40, nullptr);
            }
            ready.set_value(); // The first frame is painted before editor initialization resumes.

            if (hwnd) {
                MSG event;
                while (GetMessageW(&event, nullptr, 0, 0) > 0) {
                    TranslateMessage(&event);
                    DispatchMessageW(&event);
                }
            }
            if (font) DeleteObject(font);
            if (picture) cairo_surface_destroy(picture);
            UnregisterClassW(klass.lpszClassName, instance);
        });
        future.wait();
    }

    ~Impl()
    {
        if (auto hwnd = window.load()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (thread.joinable()) thread.join();
    }

    int px(double value) const { return std::lround(value * scale); }

    void paint(HWND hwnd, HDC print_dc = nullptr)
    {
        PAINTSTRUCT paint;
        auto dc = print_dc ? print_dc : BeginPaint(hwnd, &paint);
        auto buffer = CreateCompatibleDC(dc);
        auto bitmap = CreateCompatibleBitmap(dc, px(WIDTH), px(HEIGHT));
        auto previous_bitmap = SelectObject(buffer, bitmap);

        RECT bounds{0, 0, px(WIDTH), px(HEIGHT)};
        auto ground = CreateSolidBrush(RGB(20, 22, 28));
        FillRect(buffer, &bounds, ground);
        DeleteObject(ground);

        if (picture && cairo_surface_status(picture) == CAIRO_STATUS_SUCCESS) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = cairo_image_surface_get_width(picture);
            info.bmiHeader.biHeight = -cairo_image_surface_get_height(picture);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(buffer, HALFTONE);
            SetBrushOrgEx(buffer, 0, 0, nullptr);
            StretchDIBits(buffer, 0, 0, px(WIDTH), px(PICTURE_HEIGHT), 0, 0,
                          info.bmiHeader.biWidth, -info.bmiHeader.biHeight,
                          cairo_image_surface_get_data(picture), &info, DIB_RGB_COLORS, SRCCOPY);
        }

        auto previous_font = SelectObject(buffer, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(buffer, TRANSPARENT);
        SetTextColor(buffer, RGB(242, 240, 233));
        if (!picture || cairo_surface_status(picture) != CAIRO_STATUS_SUCCESS) {
            // A missing picture must never prevent the editor from opening.
            RECT title{px(28), px(42), px(652), px(90)};
            DrawTextW(buffer, L"PROOF", -1, &title, DT_LEFT | DT_SINGLELINE);
        }
        std::wstring current_message;
        {
            std::lock_guard lock(mutex);
            current_message = message;
        }
        RECT label{px(28), px(398), px(652), px(419)};
        DrawTextW(buffer, current_message.c_str(), -1, &label, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT track{px(28), px(427), px(652), px(430)};
        auto muted = CreateSolidBrush(RGB(49, 52, 63));
        FillRect(buffer, &track, muted);
        DeleteObject(muted);
        // An indeterminate sweep: startup does not provide a measurable percentage.
        double phase = (tick % 75) / 74.0;
        double start = -112 + phase * (624 + 112);
        RECT bar{px(28 + std::max(0.0, start)), track.top,
                 px(28 + std::min(624.0, start + 112)), track.bottom};
        auto cobalt = CreateSolidBrush(RGB(83, 120, 255));
        FillRect(buffer, &bar, cobalt);
        DeleteObject(cobalt);

        BitBlt(dc, 0, 0, bounds.right, bounds.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previous_font);
        SelectObject(buffer, previous_bitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        if (!print_dc) EndPaint(hwnd, &paint);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT event, WPARAM wparam, LPARAM lparam)
    {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (event == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            switch (event) {
                case WM_PAINT: self->paint(hwnd); return 0;
                case WM_PRINTCLIENT: self->paint(hwnd, reinterpret_cast<HDC>(wparam)); return 0;
                case WM_ERASEBKGND: return 1; // Painting is double buffered.
                case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
                case WM_TIMER: ++self->tick; [[fallthrough]];
                case UPDATE_MESSAGE: {
                    RECT footer{0, self->px(PICTURE_HEIGHT), self->px(WIDTH), self->px(HEIGHT)};
                    InvalidateRect(hwnd, &footer, FALSE);
                    return 0;
                }
                case WM_DESTROY:
                    self->window.store(nullptr);
                    PostQuitMessage(0);
                    return 0;
            }
        }
        return DefWindowProcW(hwnd, event, wparam, lparam);
    }
};

StartupSplash::StartupSplash(std::string const &picture)
{
    try {
        _impl = std::make_unique<Impl>(picture);
    } catch (std::system_error const &) {
        // A failed optional UI thread must not stop startup.
    }
}

StartupSplash::~StartupSplash() = default;

void StartupSplash::set_message(std::string const &message)
{
    if (!_impl) return;
    auto wide = g_utf8_to_utf16(message.c_str(), -1, nullptr, nullptr, nullptr);
    if (!wide) return;
    {
        std::lock_guard lock(_impl->mutex);
        _impl->message = reinterpret_cast<wchar_t *>(wide);
    }
    g_free(wide);
    if (auto hwnd = _impl->window.load()) PostMessageW(hwnd, Impl::UPDATE_MESSAGE, 0, 0);
}

} // namespace Inkscape::UI

#else
namespace Inkscape::UI {
struct StartupSplash::Impl {};
StartupSplash::StartupSplash(std::string const &) {}
StartupSplash::~StartupSplash() = default;
void StartupSplash::set_message(std::string const &) {}
} // namespace Inkscape::UI
#endif
