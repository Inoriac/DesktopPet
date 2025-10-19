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

void RenderViewport::initializeGL() {
    glCore = std::make_unique<QOpenGLFunctions_3_3_Core>();
    if (!glCore->initializeOpenGLFunctions()) {
        qWarning() << "Failed to initialize 3.3 core functions";
        return;
    }

    // GPU 信息
    auto *f = QOpenGLContext::currentContext()->functions();
    qDebug() << "Vendor:"   << reinterpret_cast<const char*>(f->glGetString(GL_VENDOR));
    qDebug() << "Renderer:" << reinterpret_cast<const char*>(f->glGetString(GL_RENDERER));
    qDebug() << "Version:"  << reinterpret_cast<const char*>(f->glGetString(GL_VERSION));
    qDebug() << "GLSL:"     << reinterpret_cast<const char*>(f->glGetString(GL_SHADING_LANGUAGE_VERSION));


    // 初始化着色器
    shaderManager = std::make_unique<ShaderManager>();
    shaderManager->initialize(glCore.get());

    // 加载 PBR 着色器
    if (shaderManager->loadShader("pbr", "assets/shaders/pbr.vert", "assets/shaders/pbr.frag")) qDebug() << "PBR shader loaded successfully";

    // 初始化渲染引擎
    renderEngine = std::make_unique<RenderEngine>();
    renderEngine->initialize(glCore.get(), shaderManager.get());

    // 模型加载
    ModelLoader loader;
    bool success = loader.loadModel("assets/models/milltina/model/milltina.gltf");
    // bool success = loader.loadModel("assets/models/kipfel/model/Kipfel_1.0.3.gltf");

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

    } else {
        qWarning() << "Failed to load model";

        // 使用默认三角形
        std::vector<float> tri = {
            // x, y, z,    r, g, b,    u, v
            0.0f,  0.6f, 0.0f, 1.0f,0.3f,0.3f, 0.5f, 1.0f,  // 顶点0：顶部，UV(0.5, 1.0)
           -0.6f, -0.4f, 0.0f, 0.3f,1.0f,0.3f, 0.0f, 0.0f,  // 顶点1：左下，UV(0.0, 0.0)
            0.6f, -0.4f, 0.0f, 0.3f,0.3f,1.0f, 1.0f, 0.0f   // 顶点2：右下，UV(1.0, 0.0)
        };
        std::vector<unsigned int> idx = {0,1,2};
        renderEngine->addMesh(tri, idx);
    }

    // 同步材质信息
    renderEngine->setMaterials(loader.getMaterials());
    renderEngine->sortMeshesByMaterial();   // 索引优化
}

void RenderViewport::resizeGL(int w, int h) {
    if (renderEngine) renderEngine->resize(w, h);
}

void RenderViewport::paintGL() {
    if (renderEngine) renderEngine->render();
}
