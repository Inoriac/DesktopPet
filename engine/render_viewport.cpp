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

    auto *f = QOpenGLContext::currentContext()->functions();
    qDebug() << "Vendor:"   << reinterpret_cast<const char*>(f->glGetString(GL_VENDOR));
    qDebug() << "Renderer:" << reinterpret_cast<const char*>(f->glGetString(GL_RENDERER));
    qDebug() << "Version:"  << reinterpret_cast<const char*>(f->glGetString(GL_VERSION));
    qDebug() << "GLSL:"     << reinterpret_cast<const char*>(f->glGetString(GL_SHADING_LANGUAGE_VERSION));


    renderEngine = std::make_unique<RenderEngine>();
    renderEngine->initialize(glCore.get());

    // 简单三角形（pos+color 交错），索引绘制
    std::vector<float> tri = {
        // x, y, z,    r, g, b
        0.0f,  0.6f, 0.0f, 1.0f,0.3f,0.3f,
       -0.6f, -0.4f, 0.0f, 0.3f,1.0f,0.3f,
        0.6f, -0.4f, 0.0f, 0.3f,0.3f,1.0f
   };
    std::vector<unsigned int> idx = {0,1,2};
    renderEngine->addMesh(tri, idx);
}

void RenderViewport::resizeGL(int w, int h) {
    if (renderEngine) renderEngine->resize(w, h);
}

void RenderViewport::paintGL() {
    if (renderEngine) renderEngine->render();
}
