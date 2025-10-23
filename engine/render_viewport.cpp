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

#include "model_loader.h"

RenderViewport::RenderViewport(QWidget *parent)
    : QOpenGLWidget(parent) {
    // 每16毫秒触发一次（约60帧）
    QTimer *timer = new QTimer(this);
    // update被调用时，会发出一个“请求重绘”的信号，从而让Qt在下一个事件循环中用到 paintGL
    connect(timer, &QTimer::timeout, this, QOverload<>::of(&RenderViewport::update));
    timer->start(16);

    qDebug() << "RenderViewport timer started (60 FPS)";
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

    // 模型加载
    qDebug() << "--- Loading model ---";
    ModelLoader loader;

    size_t mem5 = getCurrentMemoryUsage();
    qDebug() << "After ModelLoader created:" << mem5 << "MB, delta:" << (mem5-mem4) << "MB";

    bool success = loader.loadModel("assets/models/milltina/model/milltina.gltf");
    // bool success = loader.loadModel("assets/models/kipfel/model/Kipfel_1.0.3.gltf");

    size_t mem6 = getCurrentMemoryUsage();
    qDebug() << "After model loaded:" << mem6 << "MB, delta:" << (mem6-mem5) << "MB ← KEY!";

    if (success) {
        qDebug() << "Model loaded successfully!";
        qDebug() << "Mesh count:" << loader.getMeshes().size();
        qDebug() << "Material count:" << loader.getMaterials().size();

        // 上传纹理材质
        for (auto& material : loader.getMaterials()) {
            renderEngine->uploadMaterialTextures(material);
        }
        qDebug() << "All materials uploaded to GPU successfully";

        size_t mem7 = getCurrentMemoryUsage();
        qDebug() << "After textures uploaded:" << mem7 << "MB, delta:" << (mem7-mem6) << "MB";

        // 上传网格数据
        for (const auto& meshData : loader.getMeshes()) {
            renderEngine->addMeshFromData(meshData);
        }
        qDebug() << "All meshes uploaded to GPU successfully";

        size_t mem8 = getCurrentMemoryUsage();
        qDebug() << "After meshes uploaded:" << mem8 << "MB, delta:" << (mem8-mem7) << "MB";


    } else {
        qWarning() << "Failed to load model";
    }

    // 同步材质信息
    renderEngine->setMaterials(loader.getMaterials());
    renderEngine->sortMeshesByMaterial();   // 索引优化

    size_t memFinal = getCurrentMemoryUsage();
    qDebug() << "=== Final memory:" << memFinal << "MB ===";
    qDebug() << "Total increase:" << (memFinal - mem0) << "MB";
}

void RenderViewport::resizeGL(int w, int h) {
    if (renderEngine) renderEngine->resize(w, h);
}

void RenderViewport::paintGL() {
    if (renderEngine) renderEngine->render();
}
