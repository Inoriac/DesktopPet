//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"

#include "render_engine.h"
#include "render_viewport.h"

#include <QDateTime>
#include <QDebug>
#include <QTimer>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

void PetWindow::setupWindowSnapping() {
    snapFollowTimer = new QTimer(this);
    connect(snapFollowTimer, &QTimer::timeout, this, [this]() {
        updateCachedWindows();
        updateSnapZone();

        if (forceExitOnBigScreenAlarm && isBigScreenAlarm && snappedWindow) {
            exitWindowSnapping(true);
            return;
        }

        if (!snappedWindow) {
            return;
        }

        bool found = false;
        QRect snappedRect;
        for (const auto& win : cachedWindows) {
            if (win.hwnd == snappedWindow) {
                found = true;
                snappedRect = win.rect;
                break;
            }
        }

        if (!found) {
            exitWindowSnapping(true);
            return;
        }

        if (isWindowMaximized(snappedWindow) || isWindowFullscreen(snappedRect)) {
            move(lastDesktopPosition);
            exitWindowSnapping(true);
            return;
        }

        if (isDragging) {
            if (!isStillNearSnappedWindow()) {
                exitWindowSnapping(false);
                return;
            }
            followSnappedWindowWhileDragging();
            return;
        }

        followSnappedWindow();
    });
    snapFollowTimer->start(snapFollowIntervalMs);

    snapScanTimer = new QTimer(this);
    connect(snapScanTimer, &QTimer::timeout, this, [this]() {
        updateCachedWindows();
        updateSnapZone();
    });
    snapScanTimer->start(std::max(100, snapFollowIntervalMs * 4));
}

void PetWindow::updateSnapZone() {
    const QRect nativeRect = getNativeWindowRect();
    const int cx = nativeRect.left() + nativeRect.width() / 2 + snapZoneOffset.x();
    const int by = nativeRect.top() + nativeRect.height() + snapZoneOffset.y();

    pinkZoneDesktopRect = QRect(
        cx - snapZoneSize.width() / 2,
        by,
        snapZoneSize.width(),
        snapZoneSize.height());
}

bool PetWindow::trySnap() {
#ifdef Q_OS_WIN
    const QRect nativeRect = getNativeWindowRect();
    const int petCenterX = nativeRect.left() + nativeRect.width() / 2;

    for (const auto& win : cachedWindows) {
        if (!win.hwnd) continue;

        const QRect& r = win.rect;
        const int barMidX = r.left() + r.width() / 2;
        const int barY = r.top() + 2;

        POINT pt { barMidX, barY };
        HWND hit = WindowFromPoint(pt);
        HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
        if (root != reinterpret_cast<HWND>(win.hwnd)) {
            continue;
        }

        const QRect topBar(r.left(), r.top(), r.width(), 5);
        const QRect expandedTopBar = topBar.adjusted(-snapThreshold, -snapThreshold, snapThreshold, snapThreshold);
        if (!pinkZoneDesktopRect.intersects(expandedTopBar)) {
            continue;
        }

        lastDesktopPosition = nativeRect.topLeft();
        pendingSnapTarget = win.hwnd;

        const float winWidth = static_cast<float>(std::max(1, r.width()));
        pendingSnapFraction = std::clamp((petCenterX - r.left()) / winWidth, 0.0f, 1.0f);

        POINT cp {};
        if (GetCursorPos(&cp)) {
            snapCursorY = cp.y;
        }

        return true;
    }
#endif

    return false;
}

bool PetWindow::isStillNearSnappedWindow() const {
    if (!snappedWindow) {
        return false;
    }

    for (const auto& win : cachedWindows) {
        if (win.hwnd != snappedWindow) {
            continue;
        }

        const QRect topBar(win.rect.left(), win.rect.top(), win.rect.width(), 5);

#ifdef Q_OS_WIN
        if (isDragging && isDraggingAnyWindowSitState()) {
            POINT cp {};
            if (!GetCursorPos(&cp)) {
                return true;
            }

            const int vBand = std::max(4, snapZoneSize.height());
            if (std::abs(cp.y - snapCursorY) > vBand) {
                return false;
            }
        }
#endif

        return pinkZoneDesktopRect.intersects(topBar);
    }

    return false;
}

void PetWindow::followSnappedWindowWhileDragging() {
    if (!snappedWindow) return;

    for (const auto& win : cachedWindows) {
        if (win.hwnd != snappedWindow) continue;

        const QRect nativeRect = getNativeWindowRect();
        const float petCenterX = static_cast<float>(nativeRect.left() + nativeRect.width() / 2);
        const float winWidth = static_cast<float>(std::max(1, win.rect.width()));
        snapFraction = std::clamp((petCenterX - win.rect.left()) / winWidth, 0.0f, 1.0f);

        const int yOffset = nativeRect.height() + snapZoneOffset.y() + snapZoneSize.height() / 2;
        int targetY = win.rect.top() - yOffset + snapVerticalOffset;
        moveNativeWindow(nativeRect.left(), targetY);
        return;
    }
}

void PetWindow::followSnappedWindow() {
    if (!snappedWindow) return;

    for (const auto& win : cachedWindows) {
        if (win.hwnd != snappedWindow) continue;

        const float winWidth = static_cast<float>(std::max(1, win.rect.width()));
        const float newCenterX = win.rect.left() + snapFraction * winWidth;
        const QRect nativeRect = getNativeWindowRect();
        const QRect bounds = getMovementBounds();
        int targetX = static_cast<int>(std::lround(newCenterX - nativeRect.width() * 0.5f));

        const int yOffset = nativeRect.height() + snapZoneOffset.y() + snapZoneSize.height() / 2;
        const int targetY = win.rect.top() - yOffset + snapVerticalOffset;

        // 已经顶到屏幕上边，且目标继续向上时，退出吸附并落到底部。
        if (nativeRect.top() <= bounds.top() && targetY < bounds.top()) {
            const int clampedX = std::clamp(targetX, bounds.left(), bounds.right() - nativeRect.width() + 1);
            exitWindowSnapping(true);
            startDropAnimation(clampedX);
            return;
        }

        moveNativeWindow(targetX, targetY);
        qDebug() << "[WindowSnap] follow target=" << QPoint(targetX, targetY);
        return;
    }

    exitWindowSnapping(true);
}

void PetWindow::exitWindowSnapping(bool triggerWindowStand) {
    if (!snappedWindow) return;

    snappedWindow = nullptr;
    pendingSnapOnRelease = false;
    pendingSnapTarget = nullptr;
    refreshTopMostByState();

    if (triggerWindowStand && renderViewport && renderViewport->getRenderEngine() && renderViewport->getRenderEngine()->getAnimationPlayer()) {
        renderViewport->getRenderEngine()->getAnimationPlayer()->triggerEvent("window_stand");
    }
}

void PetWindow::setupDropAnimation() {
    dropTimer = new QTimer(this);
    dropTimer->setInterval(16);

    connect(dropTimer, &QTimer::timeout, this, [this]() {
        if (!isDropping) {
            return;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        float dt = 0.016f;
        if (dropLastTickMs > 0) {
            dt = static_cast<float>(nowMs - dropLastTickMs) / 1000.0f;
            dt = std::clamp(dt, 0.005f, 0.05f);
        }
        dropLastTickMs = nowMs;

        dropVelocity += dropAccel * dt;
        dropPosY += dropVelocity * dt;

        int currentY = static_cast<int>(std::lround(dropPosY));
        if (currentY >= dropTargetY) {
            currentY = dropTargetY;
            moveNativeWindow(dropFixedX, currentY);
            stopDropAnimation();
            qDebug() << "[WindowSnap] drop finished at" << QPoint(dropFixedX, currentY);
            return;
        }

        moveNativeWindow(dropFixedX, currentY);
    });
}

void PetWindow::startDropAnimation(int targetX) {
    const QRect nativeRect = getNativeWindowRect();
    const QRect bounds = getMovementBounds();

    dropFixedX = std::clamp(targetX, bounds.left(), bounds.right() - nativeRect.width() + 1);
    dropTargetY = bounds.bottom() - nativeRect.height() + 1;
    dropPosY = static_cast<float>(nativeRect.top());
    dropVelocity = 0.0f;

    const float distance = std::max(0.0f, static_cast<float>(dropTargetY) - dropPosY);
    constexpr float kDropDurationSec = 2.0f;
    dropAccel = (kDropDurationSec > 0.0f) ? (2.0f * distance) / (kDropDurationSec * kDropDurationSec) : 0.0f;

    isDropping = true;
    dropLastTickMs = QDateTime::currentMSecsSinceEpoch();
    if (dropTimer) {
        dropTimer->start();
    }

    qDebug() << "[WindowSnap] drop start from" << QPoint(nativeRect.left(), nativeRect.top())
             << "to" << QPoint(dropFixedX, dropTargetY)
             << "duration~2s accel=" << dropAccel;
}

void PetWindow::stopDropAnimation() {
    isDropping = false;
    dropVelocity = 0.0f;
    dropAccel = 0.0f;
    dropLastTickMs = 0;
    if (dropTimer) {
        dropTimer->stop();
    }
}

void PetWindow::beginDragAnimation() {
    if (!renderViewport || !renderViewport->getRenderEngine()) {
        return;
    }
    auto* player = renderViewport->getRenderEngine()->getAnimationPlayer();
    if (!player) {
        return;
    }

    const std::string before = player->getCurrentStateName();
    player->triggerEvent("start_drag");
    const std::string after = player->getCurrentStateName();

    if (after == before || after != "Drag") {
        player->changeState("Drag", 0.1);
        qDebug() << "[WindowSnap] force change to Drag from" << before.c_str();
    }
}

bool PetWindow::isDraggingAnyWindowSitState() const {
    if (!renderViewport || !renderViewport->getRenderEngine() || !renderViewport->getRenderEngine()->getAnimationPlayer()) {
        return false;
    }

    const std::string state = renderViewport->getRenderEngine()->getAnimationPlayer()->getCurrentStateName();
    return state == "WindowSit" || state == "Drag";
}

void PetWindow::setBigScreenAlarm(bool on) {
    isBigScreenAlarm = on;
    if (forceExitOnBigScreenAlarm && isBigScreenAlarm && snappedWindow) {
        exitWindowSnapping(true);
    }
}

