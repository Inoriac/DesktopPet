//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"
#include "liquidglasschatbubble.h"
#include "render_viewport.h"
#include "pet.h"

#include <QApplication>
#include <qboxlayout.h>
#include <QDebug>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QDateTime>
#include <QRandomGenerator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include "render_engine.h"
#include "configLoader/config_manager.h"
#include "ai/tools/animation_tools.h"
#include "ai/tools/environment_tools.h"
#include "ai/tools/music_tools.h"

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

namespace {
QUrl buildCompletionsUrlFromBase(const QString& baseUrl) {
    QString normalized = baseUrl.trimmed();
    if (normalized.endsWith('/')) {
        normalized.chop(1);
    }
    if (normalized.endsWith("/chat/completions")) {
        return QUrl(normalized);
    }
    return QUrl(normalized + "/chat/completions");
}

QString extractJsonPayload(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
        return trimmed;
    }

    QRegularExpression fenced(R"(```(?:json)?\s*(\{[\s\S]*\})\s*```)");
    QRegularExpressionMatch match = fenced.match(text);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    const int start = text.indexOf('{');
    const int end = text.lastIndexOf('}');
    if (start >= 0 && end > start) {
        return text.mid(start, end - start + 1);
    }
    return {};
}
}

PetWindow::PetWindow(const QString modelName, QWidget *parent)
    : QWidget(parent)
    , isDragging(false)
    , renderViewport(nullptr)
    , modelName(modelName)
    , sizePercent(100)
    , alwaysOnTop(true)
    , clickThrough(false)
    , contextMenu(nullptr)
    , closeAction(nullptr) {
    ConfigManager& config = ConfigManager::instance();
    snapThreshold = config.getWindowSnapThreshold();
    snapVerticalOffset = config.getWindowSnapVerticalOffset();
    snapZoneOffset = config.getWindowSnapZoneOffset();
    snapZoneSize = config.getWindowSnapZoneSize();
    snapFollowIntervalMs = config.getWindowSnapFollowIntervalMs();
    forceExitOnBigScreenAlarm = config.getWindowSnapForceExitOnBigScreenAlarm();
    totalWindowSitAnimations = config.getTotalWindowSitAnimations();

    setupWindow();
    setupRenderViewport();
    setupContextMenu();
    setupWindowSnapping();
    setupDropAnimation();
    setupScreenChat();

    aiBrain = std::make_unique<AIBrain>(this);
    connect(aiBrain.get(), &AIBrain::assistantResponseReady, this, [](const QString& content) {
        qDebug() << "[AIBrain] assistant response:" << content;
    });
    connect(aiBrain.get(), &AIBrain::proactiveResponseReady, this, [this](const QString& content) {
        qDebug() << "[AIBrain] proactive response:" << content;
        showBubbleMessage(content);
    });
    connect(aiBrain.get(), &AIBrain::toolExecuted, this, [this](const QString& toolName, bool success, const QString& payload) {
        qDebug() << "[AIBrain] tool executed:" << toolName << "success:" << success << "payload:" << payload;
        Q_UNUSED(payload);
        Q_UNUSED(success);
        // No further action required here for now; keep as diagnostic hook.
    });
}

PetWindow::~PetWindow() {
    if (snapFollowTimer) {
        snapFollowTimer->stop();
    }
    if (snapScanTimer) {
        snapScanTimer->stop();
    }
    if (dropTimer) {
        dropTimer->stop();
    }
    if (renderViewport) {
        delete renderViewport;
    }
    if (screenChatTimer) {
        screenChatTimer->stop();
    }
    if (bubbleHideTimer) {
        bubbleHideTimer->stop();
    }
    if (outputBubble) {
        outputBubble->close();
        outputBubble->deleteLater();
        outputBubble = nullptr;
    }
    if (inputBubble) {
        inputBubble->close();
        inputBubble->deleteLater();
        inputBubble = nullptr;
    }
    if (contextMenu) {
        delete contextMenu;
    }
}

void PetWindow::applySettings(int sizePercent,
                              bool alwaysOnTop,
                              bool clickThrough,
                              bool aiEnabled,
                              const ScreenChatConfig& screenChatConfig) {
    this->alwaysOnTop = alwaysOnTop;
    this->clickThrough = clickThrough;

    // 更新窗口标志
    updateWindowFlags(alwaysOnTop, clickThrough);

    applyRuntimeSettings(sizePercent, aiEnabled, screenChatConfig);
}

void PetWindow::applyRuntimeSettings(int sizePercent,
                                     bool aiEnabled,
                                     const ScreenChatConfig& screenChatConfig) {
    this->sizePercent = sizePercent;
    this->aiEnabled = aiEnabled;
    this->screenChatConfig = screenChatConfig;

    // 更新大小
    int baseSize = 400;
    int newSize = baseSize * sizePercent / 100;
    if (size() != QSize(newSize, newSize)) {
        resize(newSize, newSize);
    }

    qDebug() << "PetWindow runtime setting applied - size:" << newSize;
    qDebug() << "AI enabled:" << this->aiEnabled;
    qDebug() << "Screen chat enabled:" << this->screenChatConfig.enabled;

    if (outputBubble) {
        outputBubble->applyScreenChatConfig(this->screenChatConfig);
    }
    if (inputBubble) {
        inputBubble->applyScreenChatConfig(this->screenChatConfig);
    }
    updateBubblePositions();
    updateScreenChatSchedule();
}

void PetWindow::previewBubble(const QString& message) {
    const QString previewText = message.trimmed().isEmpty()
        ? QStringLiteral("气泡预览：位置和样式会实时生效")
        : message.trimmed();

    showBubbleMessage(previewText);
    if (bubbleHideTimer) {
        bubbleHideTimer->start(2200);
    }
}

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
        qDebug() << "Touch ignored: reaction playing";
        return;
    }

    if (renderViewport && renderViewport->getRenderEngine()) {
        auto* player = renderViewport->getRenderEngine()->getAnimationPlayer();
        if (player) {
            std::string targetState = "Touch" + tag;

            // === DEBUG: 暂时禁用状态转换，仅输出诊断信息 ===
            qDebug() << "=== Touch Debug Info ===";
            qDebug() << "  Hit tag:" << tag.c_str();
            qDebug() << "  Would transition to:" << targetState.c_str();
            qDebug() << "  Current state:" << player->getCurrentStateName().c_str();
            qDebug() << "  Current clip:" << player->getCurrentClipName().c_str();

            // 打印所有碰撞体的骨骼绑定情况
            auto* engine = renderViewport->getRenderEngine();
            const auto& skeleton = player->getSkeleton();
            qDebug() << "  --- Bone binding check ---";
            qDebug() << "  Total bones in skeleton:" << skeleton.bones.size();
            for (const auto& pair : skeleton.nameToIndex) {
                // 只打印可能相关的骨骼（包含配置中常用关键词的）
                const std::string& name = pair.first;
                if (name.find("Head") != std::string::npos ||
                    name.find("Spine") != std::string::npos ||
                    name.find("Hips") != std::string::npos ||
                    name.find("Hand") != std::string::npos ||
                    name.find("head") != std::string::npos ||
                    name.find("spine") != std::string::npos ||
                    name.find("hand") != std::string::npos) {
                    qDebug() << "    Bone:" << name.c_str() << " -> index:" << pair.second;
                }
            }
            qDebug() << "=== End Touch Debug ===";

            // TODO: 调试完毕后取消下面的注释以恢复状态转换
            // player->changeState(targetState);
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
            qDebug() << "Pressed on part:" << hitPartTag.c_str();
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
                    qDebug() << "Short press (no model hit)";
                }
            } else {
                qDebug() << "Long press (ignored or open menu)";
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

void PetWindow::closeEvent(QCloseEvent *event) {
    qDebug() << "PetWindow closing...";
    hideBubbleMessage();
    if (aiBrain) {
        aiBrain->stop();
    }
    unloadModel();
    emit aboutToClose();  // 可以发送信号给 MainWindow
    event->accept();
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

void PetWindow::setupWindow() {
    // 设置窗口属性
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // 设置窗口标志
    updateWindowFlags(alwaysOnTop, clickThrough);
    resize(400, 400);

    // 设置到右下角
    QScreen *screen = QApplication::primaryScreen();
    QRect screenGeometry = screen->availableGeometry();
    int x = screenGeometry.right() - width() - 20;   // 距离右边20像素
    int y = screenGeometry.bottom() - height() - 20; // 距离底部20像素
    move(x, y);

    qDebug() << "PetWindow create with model:" << Pet::instance().getModelPath(modelName);
}

void PetWindow::setupRenderViewport() {
    renderViewport = new RenderViewport(this);
    renderViewport->setMinimumHeight(400);

    // 设置布局
    auto layout = std::make_unique<QVBoxLayout>(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(renderViewport);

    layout.release();
    
    // 连接渲染视口的初始化完成信号，确保只有在初始化完成后才加载模型
    connect(renderViewport, &RenderViewport::initializationCompleted, this, [this]() {
        qDebug() << "RenderViewport initialization completed, now loading model...";
        if (!renderViewport->loadModel(Pet::instance().getModelPath(modelName))) {
            qWarning() << "Failed to load model:" << Pet::instance().getModelPath(modelName);
        }

        setupAiBrain();
    });
}

void PetWindow::setupAiBrain() {
    if (!aiBrain || !renderViewport || !renderViewport->getRenderEngine()) {
        return;
    }

    auto* player = renderViewport->getRenderEngine()->getAnimationPlayer();
    auto* animationManager = renderViewport->getAnimationManager();
    if (!player) {
        qWarning() << "[AIBrain] AnimationPlayer not ready, skip setup";
        return;
    }
    if (!animationManager) {
        qWarning() << "[AIBrain] AnimationManager not ready, skip setup";
        return;
    }

    aiToolRegistry = std::make_unique<ToolRegistry>();
    aiToolRegistry->registerTool(std::make_unique<PlayAnimationTool>(player));
    aiToolRegistry->registerTool(std::make_unique<GetCurrentAnimationTool>(player));
    aiToolRegistry->registerTool(std::make_unique<GetIdleTransitionCandidatesTool>(player, animationManager));
    aiToolRegistry->registerTool(std::make_unique<GetActionTransitionStatusTool>(player));
    aiToolRegistry->registerTool(std::make_unique<RequestIdleTransitionTool>(player, animationManager));
    aiToolRegistry->registerTool(std::make_unique<GetCurrentTimeTool>());
    aiToolRegistry->registerTool(std::make_unique<MusicNextTrackTool>());
    aiToolRegistry->registerTool(std::make_unique<MusicPlaySongTool>());
    aiToolRegistry->registerTool(std::make_unique<MusicSwitchPlaylistTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicStatusTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicPlayTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicPauseTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicSkipNextTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicSkipPrevTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicLyricTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicVolumeTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicSearchPlayTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicListPlaylistsTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicPlaylistSongsTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicPlayPlaylistTool>());

    aiBrain->setPetName(modelName);
    aiBrain->setToolRegistry(aiToolRegistry.get());
    aiBrain->setEnabled(aiEnabled);

    if (aiEnabled) {
        aiBrain->start();
        qDebug() << "[AIBrain] started for pet:" << modelName;
    }
}

void PetWindow::setupScreenChat() {
    screenChatTimer = new QTimer(this);
    screenChatTimer->setSingleShot(true);
    connect(screenChatTimer, &QTimer::timeout, this, [this]() {
        triggerScreenChat(false, "timer");
    });

    bubbleHideTimer = new QTimer(this);
    bubbleHideTimer->setSingleShot(true);
    connect(bubbleHideTimer, &QTimer::timeout, this, &PetWindow::hideBubbleMessage);

    outputBubble = new LiquidGlassChatBubble(nullptr);
    outputBubble->applyScreenChatConfig(screenChatConfig);
    outputBubble->hide();

    inputBubble = new LiquidGlassChatBubble(nullptr);
    inputBubble->applyScreenChatConfig(screenChatConfig);
    inputBubble->showInput("输入后按 Enter 发送...", false);
    updateInputBubblePosition();
    inputBubble->refreshGlass();
    connect(inputBubble, &LiquidGlassChatBubble::messageSubmitted, this, [this](const QString& text) {
        if (!aiBrain || !aiBrain->isEnabled()) {
            qWarning() << "[AIBrain] user input ignored, AI disabled";
            return;
        }
        aiBrain->triggerThink(text, "user_request");
    });

#ifdef Q_OS_WIN
    outputBubble->winId();
    inputBubble->winId();
#endif
}

void PetWindow::updateScreenChatSchedule() {
    if (!screenChatTimer) {
        return;
    }

    if (!screenChatConfig.enabled) {
        screenChatTimer->stop();
        return;
    }

    scheduleNextScreenChat();
}

void PetWindow::scheduleNextScreenChat() {
    if (!screenChatTimer || !screenChatConfig.enabled) {
        return;
    }

    const int minMs = std::max(1000, screenChatConfig.minIntervalMs);
    const int maxMs = std::max(minMs, screenChatConfig.maxIntervalMs);
    const int nextMs = QRandomGenerator::global()->bounded(minMs, maxMs + 1);
    screenChatTimer->start(nextMs);
    qDebug() << "[ScreenChat] next trigger in ms:" << nextMs;
}

void PetWindow::triggerScreenChatNow(const QString& reason) {
    triggerScreenChat(false, reason);
}

QString PetWindow::captureDesktopScreenshot(bool debugKeepCopy, QString* debugCopyPath) const {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) {
        qWarning() << "[ScreenChat] primary screen not found";
        return {};
    }

    const QPixmap shot = screen->grabWindow(0);
    if (shot.isNull()) {
        qWarning() << "[ScreenChat] grabWindow failed";
        return {};
    }

    const QString fileName = QString("desktop_pet_capture_%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString tempPath = QDir::temp().absoluteFilePath(fileName);
    if (!shot.save(tempPath, "PNG")) {
        qWarning() << "[ScreenChat] failed to save temp screenshot:" << tempPath;
        return {};
    }

    if (debugKeepCopy && debugCopyPath) {
        QDir logDir("log");
        if (!logDir.exists()) {
            logDir.mkpath(".");
        }
        const QString debugPath = logDir.absoluteFilePath(fileName);
        if (QFile::copy(tempPath, debugPath)) {
            *debugCopyPath = debugPath;
        }
    }

    return tempPath;
}

void PetWindow::triggerScreenChat(bool debugSaveScreenshotOnly, const QString& reason) {
    if (screenChatBusy) {
        qDebug() << "[ScreenChat] skip, request already in-flight";
        if (screenChatConfig.enabled) {
            scheduleNextScreenChat();
        }
        return;
    }

    QString debugCopyPath;
    const QString screenshotPath = captureDesktopScreenshot(debugSaveScreenshotOnly, &debugCopyPath);
    if (screenshotPath.isEmpty()) {
        if (screenChatConfig.enabled) {
            scheduleNextScreenChat();
        }
        return;
    }

    if (debugSaveScreenshotOnly) {
        QFile::remove(screenshotPath);
        const QString message = debugCopyPath.isEmpty()
            ? QString("截图已完成，但保存到log失败")
            : QString("调试截图已保存: %1").arg(debugCopyPath);
        showBubbleMessage(message);
        return;
    }

    requestVisionSummary(screenshotPath, reason, false);
}

void PetWindow::requestVisionSummary(const QString& screenshotPath,
                                     const QString& reason,
                                     bool debugSaveScreenshotOnly) {
    Q_UNUSED(debugSaveScreenshotOnly);

    const LlmConfig& llmCfg = ConfigManager::instance().getLlmConfig();
    if (!llmCfg.enabled) {
        qWarning() << "[ScreenChat] skipped, LLM disabled";
        QFile::remove(screenshotPath);
        if (screenChatConfig.enabled) {
            scheduleNextScreenChat();
        }
        return;
    }

    QFile imageFile(screenshotPath);
    if (!imageFile.open(QIODevice::ReadOnly)) {
        qWarning() << "[ScreenChat] failed to open screenshot" << screenshotPath;
        QFile::remove(screenshotPath);
        if (screenChatConfig.enabled) {
            scheduleNextScreenChat();
        }
        return;
    }

    const QByteArray imageBytes = imageFile.readAll();
    imageFile.close();
    const QString imageDataUrl = QString("data:image/png;base64,%1").arg(QString::fromLatin1(imageBytes.toBase64()));

    const QString selectedModel = llmCfg.visualModel.trimmed().isEmpty() ? llmCfg.model : llmCfg.visualModel;
    const QString styleHint = QString("请根据宠物性别(%1)生成偏日常、自然口吻的一句话，不要过度夸张。")
        .arg(screenChatConfig.petGender);

    QJsonArray contentArr;
    contentArr.append(QJsonObject{{"type", "text"}, {"text",
        QString("你是桌宠视觉助手。请识别图片主要内容，并输出JSON，格式严格为"
                " {\"main_content\":\"...\",\"pet_reply\":\"...\"}。"
                "要求：main_content不超过20字；pet_reply不超过24字；仅输出JSON，无其它文字。%1")
            .arg(styleHint)}});
    contentArr.append(QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", imageDataUrl}}}});

    QJsonArray messages;
    messages.append(QJsonObject{{"role", "user"}, {"content", contentArr}});

    QNetworkRequest request(buildCompletionsUrlFromBase(llmCfg.baseUrl));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(llmCfg.apiKey).toUtf8());
    request.setTransferTimeout(llmCfg.timeoutMs);

    QJsonObject payload;
    payload["model"] = selectedModel;
    payload["messages"] = messages;
    payload["max_tokens"] = 300;
    payload["temperature"] = 0.6;
    payload["stream"] = false;

    screenChatBusy = true;
    QNetworkReply* reply = visionNetwork.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, screenshotPath, reason]() {
        const QByteArray responseBytes = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        QString bubbleText;

        if (error != QNetworkReply::NoError) {
            qWarning() << "[ScreenChat] network error" << error << reply->errorString();
            bubbleText = "刚刚看了一眼屏幕，网络有点忙呢";
        } else {
            const QJsonDocument responseDoc = QJsonDocument::fromJson(responseBytes);
            const QJsonObject root = responseDoc.object();
            const QJsonArray choices = root.value("choices").toArray();
            if (!choices.isEmpty()) {
                const QString content = choices.first().toObject().value("message").toObject().value("content").toString();
                const QString jsonPayload = extractJsonPayload(content);
                const QJsonDocument resultDoc = QJsonDocument::fromJson(jsonPayload.toUtf8());
                if (resultDoc.isObject()) {
                    const QJsonObject resultObj = resultDoc.object();
                    bubbleText = resultObj.value("pet_reply").toString().trimmed();
                    const QString mainContent = resultObj.value("main_content").toString().trimmed();
                    qDebug() << "[ScreenChat] reason=" << reason << "main_content=" << mainContent << "pet_reply=" << bubbleText;
                }
            }
        }

        if (bubbleText.isEmpty()) {
            bubbleText = "我看到你在忙，要记得休息呀";
        }

        showBubbleMessage(bubbleText);
        QFile::remove(screenshotPath);
        screenChatBusy = false;
        if (screenChatConfig.enabled) {
            scheduleNextScreenChat();
        }

        reply->deleteLater();
    });
}

void PetWindow::showBubbleMessage(const QString& message) {
    if (!outputBubble) {
        return;
    }

    outputBubble->applyScreenChatConfig(screenChatConfig);
    outputBubble->setMessage(message);
    updateOutputBubblePosition();
    outputBubble->refreshGlass();
    outputBubble->showMessage(message);

    if (bubbleHideTimer) {
        bubbleHideTimer->start(std::max(1000, screenChatConfig.bubbleDurationMs));
    }
}

void PetWindow::showBubbleInput() {
    if (!inputBubble) {
        return;
    }

    inputBubble->applyScreenChatConfig(screenChatConfig);
    updateInputBubblePosition();
    inputBubble->refreshGlass();
    inputBubble->showInput("输入后按 Enter 发送...");
}

void PetWindow::hideBubbleMessage() {
    if (outputBubble) {
        outputBubble->hideBubble();
    }
}

void PetWindow::updateBubblePositions() {
    updateOutputBubblePosition();
    updateInputBubblePosition();
}

void PetWindow::updateOutputBubblePosition() {
    if (!outputBubble) {
        return;
    }

    const QRect rect = frameGeometry();
    const QSize bubbleSize = outputBubble->sizeHint();
    int targetX = rect.left() + (rect.width() - bubbleSize.width()) / 2 + screenChatConfig.bubbleOffsetX;
    int targetY = rect.top() - bubbleSize.height() + screenChatConfig.bubbleOffsetY;
    outputBubble->resize(bubbleSize);
    outputBubble->move(targetX, targetY);
}

void PetWindow::updateInputBubblePosition() {
    if (!inputBubble) {
        return;
    }

    const QRect rect = frameGeometry();
    const QSize bubbleSize = inputBubble->sizeHint();
    int targetX = rect.left() + (rect.width() - bubbleSize.width()) / 2 + screenChatConfig.bubbleOffsetX;
    int targetY = rect.bottom() + 8;
    inputBubble->resize(bubbleSize);
    inputBubble->move(targetX, targetY);
}

void PetWindow::updateWindowFlags(bool alwaysOnTop, bool clickThrough) {
    Qt::WindowFlags flags = Qt::FramelessWindowHint;

    if (alwaysOnTop) {
        flags |= Qt::WindowStaysOnTopHint;
    }

    if (clickThrough) {
        flags |= Qt::Tool;
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    } else {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
    }

    setWindowFlags(flags);

    // 强制更新窗口
    if (isVisible()) {
        hide();
        show();
    }
}

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

bool PetWindow::loadModel(const QString &modelPath) {
    if (!renderViewport) {
        qWarning() << "RenderViewport not initialized";
        return false;
    }
    
    return renderViewport->loadModel(modelPath);
}

void PetWindow::unloadModel() {
    if (renderViewport) {
        renderViewport->clearModel();
        qDebug() << "Unloading model:" << modelName;
    }
}

void PetWindow::playAnimation() {
    // 动画播放控制逻辑
    qDebug() << "Playing animation";
}

void PetWindow::pauseAnimation() {
    // 动画暂停控制逻辑
    qDebug() << "Pausing animation";
}

void PetWindow::stopAnimation() {
    // 动画停止控制逻辑
    qDebug() << "Stopping animation";
}

void PetWindow::setAnimationSpeed(float speed) {
    // 设置动画速度逻辑
    qDebug() << "Setting animation speed:" << speed;
}

void PetWindow::setAnimationLoop(bool loop) {
    // 设置动画循环逻辑
    qDebug() << "Setting animation loop:" << loop;
}
