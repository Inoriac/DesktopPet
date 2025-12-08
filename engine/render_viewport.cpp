//
// Created by Inoriac on 2025/10/16.
//

#include "render_viewport.h"
#include "render_engine.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLFunctions>
#include <QDebug>
#include <QTimer>
#include <psapi.h>
#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>

#include "model_loader.h"
#include "../core/configLoader/config_manager.h"

RenderViewport::RenderViewport(QWidget *parent)
    : QOpenGLWidget(parent) {
    // 从配置管理器获取默认帧率
    ConfigManager& config = ConfigManager::instance();
    int fps = config.getDefaultFPS();
    int interval = fps > 0 ? 1000 / fps : 16; // 默认60帧
    
    // 设置定时器
    auto *timer = new QTimer(this);
    // update被调用时，会发出一个“请求重绘”的信号，从而让Qt在下一个事件循环中用到 paintGL
    connect(timer, &QTimer::timeout, this, QOverload<>::of(&RenderViewport::update));
    timer->start(interval);

    qDebug() << "RenderViewport timer started (" << fps << " FPS, interval: " << interval << "ms)";
}

RenderViewport::~RenderViewport() = default;

// 添加辅助函数：获取当前进程内存占用
size_t getCurrentMemoryUsage() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    return pmc.WorkingSetSize / 1024 / 1024;  // 转换为 MB
}

void RenderViewport::initializeGL() {
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

    size_t mem4 = getCurrentMemoryUsage();
    qDebug() << "After RenderEngine init:" << mem4 << "MB, delta:" << (mem4-mem3) << "MB";

    // 初始化动画管理器
    initializeAnimationManager();

    size_t memFinal = getCurrentMemoryUsage();
    qDebug() << "=== Final memory:" << memFinal << "MB ===";
    qDebug() << "Total increase:" << (memFinal - mem0) << "MB";
    
    // 发送初始化完成信号
    emit initializationCompleted();
}

void RenderViewport::resizeGL(int w, int h) {
    if (renderEngine) renderEngine->resize(w, h);
}

void RenderViewport::paintGL() {
    static QElapsedTimer timer;
    if (!timer.isValid()) {
        timer.start();
    }
    
    // 计算deltatime（秒）
    qint64 msDelta = timer.restart();
    float deltaTime = static_cast<float>(msDelta) / 1000.0f;

    // 应用动画速度设置
    ConfigManager& config = ConfigManager::instance();
    deltaTime *= config.getAnimationSpeed();
    
    if (renderEngine) {
        // 更新动画
        renderEngine->updateAnimation(deltaTime);
        renderEngine->render();
    }
}

void RenderViewport::clearModel() {
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

        // TODO：现在采用的是通过路径加载，后续可能需要更改
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

        return true;
    } else {
        qWarning() << "Failed to load model:" << modelPath;
        return false;
    }
}
