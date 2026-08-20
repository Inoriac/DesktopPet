//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"
#include "statistic_manager.h"

#include "configLoader/config_manager.h"
#include "controller/pet_controller.h"
#include "emotion/emotion_engine.h"
#include "render_engine.h"
#include "render_viewport.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QDebug>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QVector3D>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

void PetWindow::contextMenuEvent(QContextMenuEvent *event) {
    if (!contextMenu) {
        contextMenu = new QMenu(this);
    }
    contextMenu->clear();

    QMenu* debugMenu = contextMenu->addMenu("调试动作 (Debug)");

    if (renderViewport && renderViewport->getRenderEngine() && renderViewport->getRenderEngine()->getAnimationPlayer()) {
        auto* player = renderViewport->getRenderEngine()->getAnimationPlayer();
        std::string currentState = player->getCurrentStateName();
        std::string currentClip = player->getCurrentClipName();
        
        QAction* infoAction = debugMenu->addAction(QString("当前状态: %1\n当前动画: %2")
                                                     .arg(currentState.c_str())
                                                     .arg(currentClip.c_str()));
        infoAction->setEnabled(false);
        debugMenu->addSeparator();

        auto* animManager = renderViewport->getAnimationManager();
        if (animManager) {
            const auto& stateMachine = animManager->getStateMachine();
            for (const auto& state : stateMachine.states) {
                QString stateName = QString::fromStdString(state.name);
                QString text = stateName + QString(" (%1)").arg(state.clipOptions.size());
                QAction* action = debugMenu->addAction(text, [this, stateName]() {
                    if (renderViewport && renderViewport->getRenderEngine()) {
                        auto* p = renderViewport->getRenderEngine()->getAnimationPlayer();
                        if (p) {
                            qDebug() << "Debug: Switching to state" << stateName;
                            p->changeState(stateName.toStdString());
                        }
                    }
                });
                if (state.clipOptions.empty()) {
                    action->setEnabled(false);
                }
            }
        }
    } else {
        debugMenu->addAction("未加载动画系统")->setEnabled(false);
    }

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

    openChatHistoryAction = contextMenu->addAction("打开聊天窗口");
    connect(openChatHistoryAction, &QAction::triggered, this, [this]() {
        openChatHistoryWindow();
    });

    debugCaptureOnlyAction = contextMenu->addAction("仅截图并保存到log(调试)");
    connect(debugCaptureOnlyAction, &QAction::triggered, this, [this]() {
        triggerScreenChat(true, "manual_capture_only");
    });

    contextMenu->addSeparator();

    closeAction = new QAction("关闭", this);
    contextMenu->addAction(closeAction);

    // 转发关闭信号至 mainwindow，确保状态一致与内存释放
    connect(closeAction, &QAction::triggered, this, [this]() {
        qDebug() << "Requesting stop from context menu";
        emit requestStop();
    });

    contextMenu->exec(event->globalPos());
}

void PetWindow::setupContextMenu() {
    contextMenu = new QMenu(this);
}

bool PetWindow::canTriggerTouch() const {
    if (!renderViewport || !renderViewport->getRenderEngine()) return false;
    auto* player = renderViewport->getRenderEngine()->getAnimationPlayer();
    if (!player) return false;

    std::string currentState = player->getCurrentStateName();

    // 简单互斥：如果当前状态以 "Touch" 开头，说明正在反应中，不可打断
    if (currentState.find("Touch") == 0) {
        return false;
    }
    return true;
}

void PetWindow::triggerTouchReaction(const std::string& tag) {
    if (!canTriggerTouch()) {
        // qDebug() << "Touch ignored: reaction playing";
        return;
    }

    if (renderViewport && renderViewport->getRenderEngine()) {
        auto* player = renderViewport->getRenderEngine()->getAnimationPlayer();
        if (player) {
            const std::string targetState = "Touch" + tag;
            player->changeState(targetState);
            StatisticManager::getInstance().recordTouchInteraction(
                modelName,
                QString::fromStdString(tag));
            if (petController) {
                petController->recordTouch(QString::fromStdString(tag));
            }
        }
    }
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

    // 射线检测
    if (renderViewport && renderViewport->getRenderEngine()) {
        auto* engine = renderViewport->getRenderEngine();

        // 获取 viewport 相对坐标
        QPoint viewPos = renderViewport->mapFrom(this, event->pos());
        // 兼容不同 Qt/平台下的坐标语义：
        // 先尝试逻辑像素坐标，若未命中再尝试物理像素坐标。
        hitPartTag = engine->checkHit(viewPos.x(), viewPos.y());
        if (hitPartTag.empty()) {
            qreal dpr = renderViewport->devicePixelRatioF();
            int physX = static_cast<int>(viewPos.x() * dpr);
            int physY = static_cast<int>(viewPos.y() * dpr);
            hitPartTag = engine->checkHit(physX, physY);
        }
        isPressingModel = !hitPartTag.empty();

        if (isPressingModel) {
            // qDebug() << "Pressed on part:" << hitPartTag.c_str();
        }
    } else {
        isPressingModel = false;
    }

    event->accept();
}

void PetWindow::mouseMoveEvent(QMouseEvent *event) {
    if (!clickThrough) {
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

        if (wasDraggingThisRound && renderViewport && renderViewport->getRenderEngine() && renderViewport->getRenderEngine()->getAnimationPlayer()) {
            renderViewport->getRenderEngine()->getAnimationPlayer()->triggerEvent("stop_drag");

            if (pendingSnapOnRelease && pendingSnapTarget) {
                snappedWindow = pendingSnapTarget;
                snapFraction = pendingSnapFraction;
                renderViewport->getRenderEngine()->getAnimationPlayer()->triggerEvent("window_sit");
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

void PetWindow::keyPressEvent(QKeyEvent *event) {
    if (!renderViewport || !renderViewport->getRenderEngine()) {
        QWidget::keyPressEvent(event);
        return;
    }

    auto* engine = renderViewport->getRenderEngine();
    QVector3D eye = engine->getCameraEye();
    QVector3D center = engine->getCameraCenter();

    // 移动速度
    float speed = 0.5f;
    if (event->modifiers() & Qt::ShiftModifier) speed *= 4.0f; // Shift 加速
    if (event->modifiers() & Qt::ControlModifier) speed *= 0.25f; // Ctrl 减速

    bool changed = false;

    // 前后: W/S (Z轴)
    if (event->key() == Qt::Key_W) { eye.setZ(eye.z() - speed); changed = true; }
    if (event->key() == Qt::Key_S) { eye.setZ(eye.z() + speed); changed = true; }
    
    // 左右: A/D (X轴，同时移动Center以保持水平平移)
    if (event->key() == Qt::Key_A) { eye.setX(eye.x() - speed); center.setX(center.x() - speed); changed = true; }
    if (event->key() == Qt::Key_D) { eye.setX(eye.x() + speed); center.setX(center.x() + speed); changed = true; }

    // 上下(升降): Q/E (Y轴)
    if (event->key() == Qt::Key_Q) { eye.setY(eye.y() + speed); changed = true; }
    if (event->key() == Qt::Key_E) { eye.setY(eye.y() - speed); changed = true; }

    // 调整观看中心高度: Up/Down 
    if (event->key() == Qt::Key_Up) { center.setY(center.y() + speed); changed = true; }
    if (event->key() == Qt::Key_Down) { center.setY(center.y() - speed); changed = true; }

    if (changed) {
        engine->setCameraEye(eye);
        engine->setCameraCenter(center);
        qDebug() << "Camera Updated -> Eye:" << eye << " Center:" << center;
        qDebug() << "Code: view.lookAt(QVector3D(" 
                 << eye.x() << "f," << eye.y() << "f," << eye.z() << "f), QVector3D("
                 << center.x() << "f," << center.y() << "f," << center.z() << "f), ...);";
    } else {
        QWidget::keyPressEvent(event);
    }
}

