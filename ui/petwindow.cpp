//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"
#include "render_viewport.h"
#include "pet.h"

#include <QApplication>
#include <qboxlayout.h>
#include <QDebug>
#include <QKeyEvent>

#include "render_engine.h"
#include "configLoader/config_manager.h"
#include "ai/tools/animation_tools.h"
#include "ai/tools/environment_tools.h"

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
    setupWindow();
    setupRenderViewport();
    setupContextMenu();

    aiBrain = std::make_unique<AIBrain>(this);
    connect(aiBrain.get(), &AIBrain::assistantResponseReady, this, [this](const QString& content) {
        qDebug() << "[AIBrain] assistant response:" << content;
    });
    connect(aiBrain.get(), &AIBrain::toolExecuted, this, [this](const QString& toolName, bool success, const QString& payload) {
        qDebug() << "[AIBrain] tool executed:" << toolName << "success:" << success << "payload:" << payload;
    });
}

PetWindow::~PetWindow() {
    if (renderViewport) {
        delete renderViewport;
    }
    if (contextMenu) {
        delete contextMenu;
    }
}

void PetWindow::applySettings(int sizePercent, bool alwaysOnTop, bool clickThrough, bool aiEnabled) {
    this->sizePercent = sizePercent;
    this->alwaysOnTop = alwaysOnTop;
    this->clickThrough = clickThrough;
    this->aiEnabled = aiEnabled;

    // 更新窗口标志
    updateWindowFlags(alwaysOnTop, clickThrough);

    // 更新大小
    int baseSize = 400;
    int newSize = baseSize * sizePercent / 100;
    resize(newSize, newSize);

    qDebug() << "PetWindow setting applied - size:" << newSize;
    qDebug() << "AI enabled:" << this->aiEnabled;

    if (aiBrain) {
        aiBrain->setEnabled(this->aiEnabled);
        if (this->aiEnabled) {
            aiBrain->start();
        } else {
            aiBrain->stop();
        }
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
        // 初始化状态
        isDragging = false;
        dragStartPosition = event->globalPosition().toPoint();
        pressLocalPosition = event->pos();
        pressTimer.start();
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
        int threshold = ConfigManager::instance().getDragThreshold();

        if (isDragging) {
            move(pos() + (currentPosition - dragStartPosition));
            dragStartPosition = currentPosition;

            // TODO: 这里添加边界检查(防止拖到太边缘的位置)与打断现在的动作的逻辑
            // TODO：吸附窗口部分的逻辑也可以在这里进行添加
        } else if ((currentPosition - dragStartPosition).manhattanLength() > threshold) {
            isDragging = true;
            isPressingModel = false;
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

        isDragging = false;
        isPressingModel = false;
        hitPartTag.clear();
        event->accept();
    }
}

void PetWindow::closeEvent(QCloseEvent *event) {
    qDebug() << "PetWindow closing...";
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
    // setAttribute(Qt::WA_TranslucentBackground, true);
    // setAttribute(Qt::WA_NoSystemBackground, true);
    // setAttribute(Qt::WA_TransparentForMouseEvents, true);

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
    if (!player) {
        qWarning() << "[AIBrain] AnimationPlayer not ready, skip setup";
        return;
    }

    aiToolRegistry = std::make_unique<ToolRegistry>();
    aiToolRegistry->registerTool(std::make_unique<PlayAnimationTool>(player));
    aiToolRegistry->registerTool(std::make_unique<GetCurrentAnimationTool>(player));
    aiToolRegistry->registerTool(std::make_unique<GetCurrentTimeTool>());

    aiBrain->setPetName(modelName);
    aiBrain->setToolRegistry(aiToolRegistry.get());
    aiBrain->setEnabled(aiEnabled);

    if (aiEnabled) {
        aiBrain->start();
        qDebug() << "[AIBrain] started for pet:" << modelName;
    }
}

void PetWindow::updateWindowFlags(bool alwaysOnTop, bool clickThrough) {
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;

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
