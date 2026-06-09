//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#ifdef Q_OS_WIN
namespace {
struct NativeWindowCandidate {
    void* hwnd = nullptr;
    QRect rect;
};

struct WinEnumContext {
    HWND self = nullptr;
    std::vector<NativeWindowCandidate>* out = nullptr;
};

bool isTaskbarClassName(const QString& cls) {
    return cls == "Shell_TrayWnd" || cls == "Shell_SecondaryTrayWnd";
}

BOOL CALLBACK EnumWindowsProcForSnap(HWND hWnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WinEnumContext*>(lParam);
    if (!ctx || !ctx->out) return TRUE;

    if (!IsWindowVisible(hWnd)) return TRUE;

    RECT r {};
    if (!GetWindowRect(hWnd, &r)) return TRUE;

    const int width = r.right - r.left;
    const int height = r.bottom - r.top;

    wchar_t className[256] = {0};
    GetClassNameW(hWnd, className, 255);
    const QString cls = QString::fromWCharArray(className);

    const bool isTaskbar = isTaskbarClassName(cls);
    if (!isTaskbar) {
        if (width < 100 || height < 100) return TRUE;
        if (GetParent(hWnd) != nullptr) return TRUE;
        if (GetWindowTextLengthW(hWnd) == 0) return TRUE;
        if (cls == "Progman" || cls == "WorkerW" || cls == "DV2ControlHost" || cls == "MsgrIMEWindowClass" ||
            cls.startsWith('#') || cls.contains("Desktop", Qt::CaseInsensitive)) {
            return TRUE;
        }
    }

    if (hWnd == ctx->self) return TRUE;

    NativeWindowCandidate entry;
    entry.hwnd = reinterpret_cast<void*>(hWnd);
    entry.rect = QRect(r.left, r.top, width, height);
    ctx->out->push_back(entry);
    return TRUE;
}

bool isWindowMaximizedByPlacement(HWND hwnd) {
    WINDOWPLACEMENT placement {};
    placement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &placement)) return false;
    return placement.showCmd == SW_MAXIMIZE;
}

void enableBlurBehindWindow(HWND hwnd) {
    if (!hwnd) return;
    HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    if (!dwmapi) return;

    using DwmEnableBlurBehindWindowFn = HRESULT (WINAPI*)(HWND, const DWM_BLURBEHIND*);
    auto fn = reinterpret_cast<DwmEnableBlurBehindWindowFn>(
        GetProcAddress(dwmapi, "DwmEnableBlurBehindWindow"));
    if (!fn) {
        FreeLibrary(dwmapi);
        return;
    }

    DWM_BLURBEHIND bb {};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = TRUE;
    bb.hRgnBlur = nullptr;
    (void)fn(hwnd, &bb);

    FreeLibrary(dwmapi);
}

}
#endif

void PetWindow::updateCachedWindows() {
    cachedWindows.clear();

#ifdef Q_OS_WIN
    std::vector<NativeWindowCandidate> windows;
    WinEnumContext ctx;
    ctx.self = reinterpret_cast<HWND>(winId());
    ctx.out = &windows;

    EnumWindows(EnumWindowsProcForSnap, reinterpret_cast<LPARAM>(&ctx));

    cachedWindows.reserve(windows.size());
    for (const auto& w : windows) {
        NativeWindowEntry e;
        e.hwnd = w.hwnd;
        e.rect = w.rect;
        cachedWindows.push_back(e);
    }
#endif
}

bool PetWindow::isWindowMaximized(void* hwnd) const {
#ifdef Q_OS_WIN
    return isWindowMaximizedByPlacement(reinterpret_cast<HWND>(hwnd));
#else
    Q_UNUSED(hwnd);
    return false;
#endif
}

bool PetWindow::isWindowFullscreen(const QRect& rect) const {
    QScreen* screen = QGuiApplication::screenAt(rect.center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) return false;

    const QRect screenRect = screen->geometry();
    const int tolerance = 2;
    return std::abs(rect.width() - screenRect.width()) <= tolerance &&
           std::abs(rect.height() - screenRect.height()) <= tolerance;
}

void PetWindow::refreshTopMostByState() {
    const bool shouldTopMost = (!snappedWindow) && alwaysOnTop;
    applyTopMostRuntime(shouldTopMost);
}

void PetWindow::applyTopMostRuntime(bool topMost) {
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        return;
    }

    SetWindowPos(
        hwnd,
        topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    updateWindowFlags(topMost, clickThrough);
#endif
}

QRect PetWindow::getNativeWindowRect() const {
#ifdef Q_OS_WIN
    RECT r {};
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd && GetWindowRect(hwnd, &r)) {
        return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
#endif
    return frameGeometry();
}

QRect PetWindow::getMovementBounds() const {
#ifdef Q_OS_WIN
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int widthPx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int heightPx = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (widthPx > 0 && heightPx > 0) {
        return QRect(left, top, widthPx, heightPx);
    }
#endif

    QScreen* primary = QGuiApplication::primaryScreen();
    if (!primary) {
        return QRect(0, 0, width(), height());
    }
    return primary->virtualGeometry();
}

void PetWindow::moveNativeWindow(int x, int y) {
    const QRect nativeRect = getNativeWindowRect();
    const QRect bounds = getMovementBounds();

    int clampedX = x;
    int clampedY = y;

    const int maxX = bounds.right() - nativeRect.width() + 1;
    const int maxY = bounds.bottom() - nativeRect.height() + 1;
    clampedX = std::clamp(clampedX, bounds.left(), maxX);
    clampedY = std::clamp(clampedY, bounds.top(), maxY);

#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
        SetWindowPos(
            hwnd,
            nullptr,
            clampedX,
            clampedY,
            nativeRect.width(),
            nativeRect.height(),
            SWP_NOZORDER | SWP_NOACTIVATE);
        updateBubblePositions();
        return;
    }
#endif
    move(clampedX, clampedY);
    updateBubblePositions();
}

