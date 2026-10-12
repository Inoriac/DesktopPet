//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"
#include "statistic_manager.h"

#include "configLoader/config_manager.h"
#include "controller/pet_controller.h"
#include "emotion/emotion_engine.h"
#include "presentation/avatar_presentation.h"
#include "avatar_view.h"


#include <QAction>
#include <QContextMenuEvent>
#include <QDebug>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QSystemTrayIcon>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

void PetWindow::contextMenuEvent(QContextMenuEvent *event) {
    if (!contextMenu) {
        contextMenu = new QMenu(this);
    }
    contextMenu->clear();

    QMenu* debugMenu = contextMenu->addMenu("调试动作 (Debug)");

    if (avatarPresentation) {
        const auto snapshot = avatarPresentation->snapshot();
        debugMenu->addAction(QStringLiteral("当前状态: %1").arg(snapshot["current_state"].toString()))->setEnabled(false);
        for (const auto& value : avatarPresentation->capabilities()["states"].toArray()) {
            const auto state = value.toObject(); const auto name = state["name"].toString();
            auto* action = debugMenu->addAction(name, this, [this, name] {
                const auto result = avatarPresentation->request(name, AvatarRequestSource::Debug);
                if (result["status"] == "rejected") qInfo() << "[Avatar]" << result;
            });
            action->setEnabled(state["available"].toBool());
        }
    }
    if (avatarTray) contextMenu->addAction(QStringLiteral("隐藏形象（托盘可恢复）"), this, [this] { setAvatarVisible(false); });

    contextMenu->addSeparator();

    QMenu* emotionMenu = contextMenu->addMenu(QStringLiteral("情绪系统"));
    QAction* emotionStatusAction = emotionMenu->addAction(emotionStatusText());
    emotionStatusAction->setEnabled(false);
    const bool emotionEnabled = emotionEngine && emotionEngine->isEnabled();
    QAction* toggleEmotionAction = emotionMenu->addAction(
        emotionEnabled ? QStringLiteral("关闭情绪") : QStringLiteral("启用情绪"));
    connect(toggleEmotionAction, &QAction::triggered, this, [this, emotionEnabled]() {
        setEmotionSystemEnabled(!emotionEnabled);
    });
    QAction* resetEmotionAction = emotionMenu->addAction(QStringLiteral("重置情绪"));
    resetEmotionAction->setEnabled(emotionEnabled);
    connect(resetEmotionAction, &QAction::triggered, this, &PetWindow::resetEmotionSystem);

    contextMenu->addSeparator();

    toggleBigScreenAlarmAction = contextMenu->addAction(
        isBigScreenAlarm ? "关闭 BigScreenAlarm(调试)" : "开启 BigScreenAlarm(调试)");
    connect(toggleBigScreenAlarmAction, &QAction::triggered, this, [this]() {
        setBigScreenAlarm(!isBigScreenAlarm);
        qDebug() << "BigScreenAlarm toggled:" << isBigScreenAlarm;
    });

    contextMenu->addSeparator();

    manualScreenChatAction = contextMenu->addAction("手动触发屏幕识别对话(调试)");
    connect(manualScreenChatAction, &QAction::triggered, this, [this]() {
        triggerScreenChatNow("manual_menu");
    });

    QAction* manualChatInputAction = contextMenu->addAction("聚焦聊天输入框");
    connect(manualChatInputAction, &QAction::triggered, this, [this]() {
        showBubbleInput();
    });

    QAction* manualDaydreamAction = contextMenu->addAction(QStringLiteral("整理记忆"));
    connect(manualDaydreamAction, &QAction::triggered, this,
            &PetWindow::onManualDaydreamRequested);

    openChatHistoryAction = contextMenu->addAction("在 Launcher 中打开聊天");
    connect(openChatHistoryAction, &QAction::triggered, this, [this]() {
        openLauncherChat();
    });

    debugCaptureOnlyAction = contextMenu->addAction("仅截图并保存到log(调试)");
    connect(debugCaptureOnlyAction, &QAction::triggered, this, [this]() {
        triggerScreenChat(true, "manual_capture_only");
    });

    contextMenu->addSeparator();

    closeAction = contextMenu->addAction("关闭");

    connect(closeAction, &QAction::triggered, this, [this]() {
        qDebug() << "Closing pet from context menu";
        close();
    });

    contextMenu->exec(event->globalPos());
}

void PetWindow::setupContextMenu() {
    contextMenu = new QMenu(this);
}

void PetWindow::triggerTouchReaction(const std::string& tag) {
    if (avatarPresentation) avatarPresentation->interact(QStringLiteral("touch"), QString::fromStdString(tag));
}

void PetWindow::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        stopDropAnimation();
        // 初始化状态
        isDragging = false;
        wasDragging = false;
        dragStartPosition = event->globalPosition().toPoint();
#ifdef Q_OS_WIN
        POINT cp {};
        if (GetCursorPos(&cp)) {
            dragStartPosition = QPoint(cp.x, cp.y);
        }
#endif
        pressLocalPosition = event->pos();
        pressTimer.start();
        pendingSnapOnRelease = false;
        pendingSnapTarget = nullptr;

#ifdef Q_OS_WIN
        if (snappedWindow && isDraggingAnyWindowSitState()) {
            POINT cp {};
            if (GetCursorPos(&cp)) {
                snapCursorY = cp.y;
            }
        }
#endif
    }

    hitPartTag = avatarView ? avatarView->hitRegion(event->pos()).toStdString() : std::string{};
    isPressingModel = !hitPartTag.empty();

    event->accept();
}

void PetWindow::mouseMoveEvent(QMouseEvent *event) {
    // Mouse tracking also delivers hover moves, which must not start a drag.
    if (!clickThrough && event->buttons().testFlag(Qt::LeftButton)) {
        QPoint currentPosition = event->globalPosition().toPoint();
#ifdef Q_OS_WIN
        POINT cp {};
        if (GetCursorPos(&cp)) {
            currentPosition = QPoint(cp.x, cp.y);
        }
#endif
        int threshold = ConfigManager::instance().getDragThreshold();

        if (isDragging) {
            const QPoint delta = currentPosition - dragStartPosition;
            const QRect nativeRect = getNativeWindowRect();
            moveNativeWindow(nativeRect.left() + delta.x(), nativeRect.top() + delta.y());
            dragStartPosition = currentPosition;

            updateCachedWindows();
            updateSnapZone();

            if (!snappedWindow) {
                pendingSnapOnRelease = trySnap();
                if (!pendingSnapOnRelease) {
                    pendingSnapTarget = nullptr;
                }
            } else {
                if (!isStillNearSnappedWindow()) {
                    exitWindowSnapping(false);
                } else {
                    followSnappedWindowWhileDragging();
                }
            }

            // TODO: 这里添加边界检查(防止拖到太边缘的位置)与打断现在的动作的逻辑
        } else if ((currentPosition - dragStartPosition).manhattanLength() > threshold) {
            isDragging = true;
            isPressingModel = false;
            beginDragAnimation();
        }

        event->accept();
    } else {
        QWidget::mouseMoveEvent(event);
    }
}

void PetWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        int timeout = ConfigManager::instance().getClickTimeout();

        if (!isDragging) {
            if (pressTimer.elapsed() < timeout) {
                if (isPressingModel && !hitPartTag.empty()) {
                    triggerTouchReaction(hitPartTag);
                } else {
                    // qDebug() << "Short press (no model hit)";
                }
            } else {
                // qDebug() << "Long press (ignored or open menu)";
            }
        }

        const bool wasDraggingThisRound = isDragging;
        isDragging = false;

        if (wasDraggingThisRound && avatarPresentation) {
            avatarPresentation->interact("stop_drag");

            if (pendingSnapOnRelease && pendingSnapTarget) {
                snappedWindow = pendingSnapTarget;
                snapFraction = pendingSnapFraction;
                avatarPresentation->interact("window_sit");
                refreshTopMostByState();
                qDebug() << "[WindowSnap] committed on release, fraction=" << snapFraction;
            }
            pendingSnapOnRelease = false;
            pendingSnapTarget = nullptr;
        }

        isPressingModel = false;
        hitPartTag.clear();
        event->accept();
    }
}

void PetWindow::keyPressEvent(QKeyEvent* event) {
    if (!avatarView || !avatarView->handleCameraKey(event->key(), event->modifiers())) QWidget::keyPressEvent(event);
}
