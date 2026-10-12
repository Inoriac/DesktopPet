//
// Created by Inoriac on 2025/10/16.
//

#include "render_viewport.h"
#include "render_engine.h"
#include "animation/avatar_3d_backend.h"
#include <QKeyEvent>

#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLFunctions>
#include <QDebug>
#include <QTimer>
#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <mach/mach_types.h>
#endif
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <cmath>

#include "model_loader.h"
#include "../core/configLoader/config_manager.h"

RenderViewport::RenderViewport(QWidget *parent)
    : QOpenGLWidget(parent) {
    // 从配置管理器获取默认帧率
    ConfigManager& config = ConfigManager::instance();
    int fps = config.getDefaultFPS();
    int interval = fps > 0 ? 1000 / fps : 16; // 默认60帧

    // 设置定时器
    auto *timer = &m_frameTimer;
    // update被调用时，会发出一个“请求重绘”的信号，从而让Qt在下一个事件循环中用到 paintGL
    connect(timer, &QTimer::timeout, this, QOverload<>::of(&RenderViewport::update));
    timer->setInterval(std::max(1, interval));

    qDebug() << "RenderViewport timer started (" << fps << " FPS, interval: " << interval << "ms)";
}

RenderViewport::~RenderViewport() {
    if (m_presentation) m_presentation->setBackend(nullptr);
    m_avatarBackend.reset();
}
void RenderViewport::bindPresentation(AvatarPresentation* presentation) { m_presentation = presentation; }
void RenderViewport::setRenderingEnabled(bool enabled) {
    m_renderingEnabled = enabled;
    m_frameClock.invalidate();
    if (enabled) { m_frameTimer.start(); update(); } else m_frameTimer.stop();
}
void RenderViewport::attachBackend() {
    if (!m_presentation || !renderEngine || !animationManager) return;
    QJsonObject durations, states;
    for (const auto& [name, clip] : animationManager->getClips()) durations[QString::fromStdString(name)] = clip.duration;
    for (const auto& state : animationManager->getStateMachine().states) {
        QJsonArray clips; for (const auto& option : state.clipOptions) clips.append(QString::fromStdString(option.clipName));
        states[QString::fromStdString(state.name)] = clips;
    }
    const int configuredFps = ConfigManager::instance().getDefaultFPS();
    const double frameRateFactor = configuredFps > 0 ? std::min(1.0, configuredFps * 0.05) : 1.0;
    m_avatarBackend = std::make_unique<Avatar3DBackend>(renderEngine->getAnimationPlayer(),
        QJsonObject{{"durations",durations},{"states",states},{"playbackRate", ConfigManager::instance().getAnimationSpeed() * frameRateFactor}},
        [this](bool enabled) { setRenderingEnabled(enabled); },
        [this](QPoint target, QPoint* actual) {
            if (!window()) return false;
            window()->move(target); *actual = window()->pos(); return true;
        });
    m_presentation->setBackend(m_avatarBackend.get());
}
QString RenderViewport::hitRegion(const QPoint& hostPosition) const {
    const QPoint position = mapFrom(window(), hostPosition);
    if (!renderEngine || !m_renderingEnabled) return {};
    auto tag = renderEngine->checkHit(position.x(),position.y());
    if (tag.empty()) tag = renderEngine->checkHit(qRound(position.x()*devicePixelRatioF()),qRound(position.y()*devicePixelRatioF()));
    return QString::fromStdString(tag);
}


// 添加辅助函数：获取当前进程内存占用（MB）
size_t getCurrentMemoryUsage() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    return pmc.WorkingSetSize / 1024 / 1024;  // 转换为 MB
#elif defined(Q_OS_MACOS)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    // phys_footprint 是进程实际驻留内存，等价于 Windows WorkingSet
    return info.phys_footprint / 1024 / 1024;  // 转换为 MB
#else
    return 0;  // 未支持平台：返回 0，仅为 qDebug 诊断用，不影响功能
#endif
}

void RenderViewport::initializeGL() {
    // Qt can recreate the GL context without selecting a new avatar. Restore
    // the already selected asset internally; the public load entry remains locked.
    const QString selectedModel = currentModelPath;
    const bool restoringContext = m_modelSelected && !selectedModel.isEmpty();
    if (m_presentation) m_presentation->setBackend(nullptr);
    m_avatarBackend.reset();
    qDebug() << "=== Memory Analysis Start ===";
    size_t mem0 = getCurrentMemoryUsage();
    qDebug() << "Initial memory:" << mem0 << "MB";

    glCore = std::make_unique<QOpenGLFunctions_3_3_Core>();
    if (!glCore->initializeOpenGLFunctions()) {
        qWarning() << "Failed to initialize 3.3 core functions";
        return;
    }

    size_t mem1 = getCurrentMemoryUsage();
    qDebug() << "After OpenGL init:" << mem1 << "MB, delta:" << (mem1-mem0) << "MB";

    // GPU 信息
    auto *f = QOpenGLContext::currentContext()->functions();
    qDebug() << "Vendor:"   << reinterpret_cast<const char*>(f->glGetString(GL_VENDOR));
    qDebug() << "Renderer:" << reinterpret_cast<const char*>(f->glGetString(GL_RENDERER));
    qDebug() << "Version:"  << reinterpret_cast<const char*>(f->glGetString(GL_VERSION));
    qDebug() << "GLSL:"     << reinterpret_cast<const char*>(f->glGetString(GL_SHADING_LANGUAGE_VERSION));

    // 初始化着色器
    shaderManager = std::make_unique<ShaderManager>();
    shaderManager->initialize(glCore.get());

    size_t mem2 = getCurrentMemoryUsage();
    qDebug() << "After ShaderManager init:" << mem2 << "MB, delta:" << (mem2-mem1) << "MB";

    // 加载 PBR 着色器
    if (shaderManager->loadShader("pbr", "assets/shaders/pbr.vert", "assets/shaders/pbr.frag")) qDebug() << "PBR shader loaded successfully";

    size_t mem3 = getCurrentMemoryUsage();
    qDebug() << "After shader load:" << mem3 << "MB, delta:" << (mem3-mem2) << "MB";

    // 初始化渲染引擎
    renderEngine = std::make_unique<RenderEngine>();
    renderEngine->initialize(glCore.get(), shaderManager.get());

    // 从配置中加载默认相机位置
    ConfigManager& config = ConfigManager::instance();
    renderEngine->setCameraCenter(config.getDefaultCameraCenter());
    renderEngine->setCameraEye(config.getDefaultCameraEye());

    size_t mem4 = getCurrentMemoryUsage();
    qDebug() << "After RenderEngine init:" << mem4 << "MB, delta:" << (mem4-mem3) << "MB";

    // 初始化动画管理器
    initializeAnimationManager();

    size_t memFinal = getCurrentMemoryUsage();
    qDebug() << "=== Final memory:" << memFinal << "MB ===";
    qDebug() << "Total increase:" << (memFinal - mem0) << "MB";

    // 发送初始化完成信号
    if (restoringContext) {
        m_modelSelected = false;
        loadModel(selectedModel);
    } else emit initializationCompleted();
}

void RenderViewport::resizeGL(int w, int h) {
    if (renderEngine) renderEngine->resize(w, h);
}

void RenderViewport::paintGL() {
    if (!m_renderingEnabled || !isVisible()) return;
    float deltaTime = 0;
    if (m_frameClock.isValid()) deltaTime = std::min(0.05f, static_cast<float>(m_frameClock.restart()) / 1000.0f);
    else m_frameClock.start();

    // 应用动画速度设置
    ConfigManager& config = ConfigManager::instance();
    deltaTime *= config.getAnimationSpeed();

    if (renderEngine) {
        // 更新动画
        renderEngine->updateAnimation(deltaTime);

        if (auto* player = renderEngine->getAnimationPlayer()) {
            QVector2D lookVector(0.0f, 0.0f);
            QVector2D headViewportPos;

            if (renderEngine->getHeadScreenPosition(headViewportPos)) {
                const QPoint cursorGlobal = QCursor::pos();
                const QPoint headLocal(static_cast<int>(std::lround(headViewportPos.x())),
                                       static_cast<int>(std::lround(headViewportPos.y())));
                const QPoint headGlobal = mapToGlobal(headLocal);
                const QPoint delta = cursorGlobal - headGlobal;

                QScreen* screen = QGuiApplication::screenAt(cursorGlobal);
                if (!screen) screen = QGuiApplication::primaryScreen();

                if (screen) {
                    const QRect g = screen->geometry();
                    const float halfW = std::max(1.0f, g.width() * 0.5f);
                    const float halfH = std::max(1.0f, g.height() * 0.5f);

                    // 屏幕坐标系：右为 x 正；上为 y 正。
                    lookVector.setX(std::clamp(delta.x() / halfW, -1.0f, 1.0f));
                    lookVector.setY(std::clamp(-delta.y() / halfH, -1.0f, 1.0f));
                }
            }

            player->setScreenLookVector(lookVector);
            renderEngine->setTrackingYawInput(lookVector.x());
        }

        renderEngine->render();
    }
}

void RenderViewport::clearModel() {
    if (m_presentation) m_presentation->setBackend(nullptr);
    m_avatarBackend.reset();
    if (renderEngine) {
        renderEngine->clearScene();
        qDebug() << "Model cleared from GPU memory";
    }

    if (animationManager) {
        animationManager->clear();
        qDebug() << "Animations cleared";
    }

    currentModelPath.clear();
    currentAnimationPath.clear();
}

void RenderViewport::initializeAnimationManager() {
    animationManager = std::make_unique<AnimationManager>();
    animationManager->initialize();
    qDebug() << "AnimationManager initialized";
}

bool RenderViewport::loadModel(const QString& modelPath) {
    if (!renderEngine || !animationManager) {
        qWarning() << "RenderViewport not properly initialized";
        return false;
    }

    if (m_modelSelected) {
        qWarning() << "[Avatar] replacing a selected avatar requires restart";
        return false;
    }
    m_modelSelected = true;
    // 清除当前模型
    clearModel();
    currentModelPath = modelPath;

    qDebug() << "--- Loading model:" << modelPath << "---";
    ModelLoader loader;

    bool success = loader.loadModel(modelPath.toStdString());

    if (success) {
        qDebug() << "Model loaded successfully!";
        qDebug() << "Mesh count:" << loader.getMeshes().size();
        qDebug() << "Material count:" << loader.getMaterials().size();

        // 传递归一化参数到渲染引擎
        renderEngine->setModelTransform(loader.getNormalizationScale(), loader.getCenterOffset());

        // 上传纹理材质
        for (auto& material : loader.getMaterials()) {
            renderEngine->uploadMaterialTextures(material);
        }
        qDebug() << "All materials uploaded to GPU successfully";

        // 上传网格数据
        for (const auto& meshData : loader.getMeshes()) {
            renderEngine->addMeshFromData(meshData);
        }
        qDebug() << "All meshes uploaded to GPU successfully";

        // 同步材质信息
        renderEngine->setMaterials(loader.getMaterials());
        renderEngine->sortMeshesByMaterial();   // 索引优化

        // 初始化碰撞体
        renderEngine->initColliders();

        // 加载动画
        bool loadedAny = false;
        for (const auto& state : animationManager->getStateMachine().states) {
            if (animationManager->loadAnimations(state.name, loader.getSkeleton())) {
                qDebug() << "state:" + state.name + "'s animations loaded success";
                loadedAny = true;
            }
        }

        // for (const QString& p : tryPaths) {
        //     QDir d(p);
        //     if (!d.exists()) continue;
        //     qDebug() << "Attempting to load animations from:" << p;
        //     if (animationManager->loadAnimations(p.toStdString(), loader.getSkeleton())) {
        //         loadedAny = true;
        //         qDebug() << "Loaded animations from:" << p;
        //         break;
        //     } else {
        //         qDebug() << "No valid animations loaded from:" << p;
        //     }
        // }

        if (!loadedAny) {
            qWarning() << "No animations found in any candidate paths; model will be loaded without animations.";
        } else {
            // 创建动画播放器（前提：已经有 clips 并且 skeleton 可用）
            auto animPlayer = animationManager->createAnimationPlayer(loader.releaseSkeleton());
            if (animPlayer) {
                const auto& stateMachine = animationManager->getStateMachine();
                // 优先使用默认状态；若缺失，则选择第一个有 clipOptions 的状态
                std::string targetState;
                if (!stateMachine.defaultState.empty()) {
                    targetState = stateMachine.defaultState;
                } else {
                    for (const auto& st : stateMachine.states) {
                        if (!st.clipOptions.empty()) { targetState = st.name; break; }
                    }
                }

                if (!targetState.empty()) {
                    animPlayer->changeState(targetState);
                    qDebug() << "Animation player default state set to:" << QString::fromStdString(targetState);
                } else {
                    qDebug() << "No state with clipOptions available to set as default state.";
                }

                renderEngine->setAnimationPlayer(std::move(animPlayer));
                qDebug() << "Animation player created and attached to render engine";
            } else {
                qWarning() << "AnimationManager produced no AnimationPlayer (no clips or missing skeleton)";
            }
        }

        attachBackend();
        return true;
    } else {
        qWarning() << "Failed to load model:" << modelPath;
        return false;
    }
}

// Touch to force rebuild

bool RenderViewport::handleCameraKey(int key, Qt::KeyboardModifiers modifiers) {
    if (!renderEngine) return false;

    auto* engine = renderEngine.get();
    QVector3D eye = engine->getCameraEye();
    QVector3D center = engine->getCameraCenter();

    // 移动速度
    float speed = 0.5f;
    if (modifiers & Qt::ShiftModifier) speed *= 4.0f; // Shift 加速
    if (modifiers & Qt::ControlModifier) speed *= 0.25f; // Ctrl 减速

    bool changed = false;

    // 前后: W/S (Z轴)
    if (key == Qt::Key_W) { eye.setZ(eye.z() - speed); changed = true; }
    if (key == Qt::Key_S) { eye.setZ(eye.z() + speed); changed = true; }

    // 左右: A/D (X轴，同时移动Center以保持水平平移)
    if (key == Qt::Key_A) { eye.setX(eye.x() - speed); center.setX(center.x() - speed); changed = true; }
    if (key == Qt::Key_D) { eye.setX(eye.x() + speed); center.setX(center.x() + speed); changed = true; }

    // 上下(升降): Q/E (Y轴)
    if (key == Qt::Key_Q) { eye.setY(eye.y() + speed); changed = true; }
    if (key == Qt::Key_E) { eye.setY(eye.y() - speed); changed = true; }

    // 调整观看中心高度: Up/Down
    if (key == Qt::Key_Up) { center.setY(center.y() + speed); changed = true; }
    if (key == Qt::Key_Down) { center.setY(center.y() - speed); changed = true; }

    if (changed) {
        engine->setCameraEye(eye);
        engine->setCameraCenter(center);
        qDebug() << "Camera Updated -> Eye:" << eye << " Center:" << center;
        qDebug() << "Code: view.lookAt(QVector3D("
                 << eye.x() << "f," << eye.y() << "f," << eye.z() << "f), QVector3D("
                 << center.x() << "f," << center.y() << "f," << center.z() << "f), ...);";
    } else {
        return false;
    }
    return true;
}
