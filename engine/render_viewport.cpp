//
// Created by Inoriac on 2025/10/16.
//

#include "render_viewport.h"
#include "render_engine.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLFunctions>
#include <QDebug>

RenderViewport::RenderViewport(QWidget *parent) : QOpenGLWidget(parent) {}

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
}

void RenderViewport::resizeGL(int w, int h) {
    if (renderEngine) renderEngine->resize(w, h);
}

void RenderViewport::paintGL() {
    if (renderEngine) renderEngine->render();
}
