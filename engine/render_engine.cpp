//
// Created by Inoriac on 2025/10/15.
//

#include "render_engine.h"
#include <QOpenGLFunctions_3_3_Core>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QVector3D>

static const char *VERT_SRC = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
out vec3 vColor;
uniform mat4 uMVP;
void main() {
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

static const char *FRAG_SRC = R"(#version 330 core
in vec3 vColor;
out vec4 FragColor;
void main() {
    FragColor = vec4(vColor, 1.0);
}
)";

RenderEngine::RenderEngine() = default;

RenderEngine::~RenderEngine() {
    if (!gl) return;
    for (auto &m : meshes) {
        if (m.ebo) gl->glDeleteBuffers(1, &m.ebo);
        if (m.vbo) gl->glDeleteBuffers(1, &m.vbo);
        if (m.vao) gl->glDeleteVertexArrays(1, &m.vao);
    }
    meshes.clear();
}

void RenderEngine::initialize(QOpenGLFunctions_3_3_Core *glFuncs) {
    gl = glFuncs;
    createProgram();

    gl->glEnable(GL_DEPTH_TEST);
    // gl->glDisable(GL_DEPTH_TEST);
    gl->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

}

void RenderEngine::resize(int width, int height) {
    viewportWidth = width;
    viewportHeight = height;
    gl->glViewport(0, 0, width, height);
}

void RenderEngine::render() {
    static QElapsedTimer timer;
    if (!timer.isValid()) timer.start(); // 第一次调用时初始化
    float deltaSec = timer.restart() / 1000.0f; // 毫秒转秒

    if (!program.isLinked() || meshes.empty()) {
        gl->glClearColor(0.2f, 0.6f, 0.9f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return;
    }

    // 清屏
    gl->glClearColor(0.2f, 0.6f, 0.9f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // 旋转角度（每秒旋转 60 度）
    angleDeg += 60.0f * deltaSec;
    if (angleDeg >= 360.0f) angleDeg -= 360.0f;

    // 构建变换矩阵
    float aspect = (viewportHeight > 0) ? float(viewportWidth) / float(viewportHeight) : 1.0f;

    QMatrix4x4 proj;
    proj.perspective(45.0f, aspect, 0.1f, 100.0f);

    QMatrix4x4 view;
    view.lookAt(QVector3D(0, 0, 3), QVector3D(0, 0, 0), QVector3D(0, 1, 0));

    QMatrix4x4 model;
    model.rotate(angleDeg, 0.0f, 1.0f, 0.0f); // 绕 Y 轴旋转

    QMatrix4x4 mvp = proj * view * model;

    // 绘制
    program.bind();
    program.setUniformValue("uMVP", mvp);

    for (const auto &m : meshes) {
        gl->glBindVertexArray(m.vao);
        if (m.indexCount > 0) {
            gl->glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    gl->glBindVertexArray(0);
    program.release();
}

void RenderEngine::addMesh(const std::vector<float> &interLeavePosColor,
                            const std::vector<unsigned int> &indices) {
    GpuMesh m;

    // 分配 GPU 资源区域
    gl->glGenVertexArrays(1, &m.vao);
    gl->glGenBuffers(1, &m.vbo);
    gl->glGenBuffers(1, &m.ebo);

    gl->glBindVertexArray(m.vao);

    gl->glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    gl->glBufferData(GL_ARRAY_BUFFER,
        GLsizeiptr(interLeavePosColor.size() * sizeof(float)),
        interLeavePosColor.data(),
        GL_STATIC_DRAW);

    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        GLsizeiptr(indices.size() * sizeof(unsigned int)),
        indices.data(),
        GL_STATIC_DRAW);

    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    gl->glEnableVertexAttribArray(1);

    gl->glBindVertexArray(0);

    m.indexCount = int(indices.size());
    meshes.push_back(m);
}

void RenderEngine::clearScene() {
    if (!gl) return;
    for (auto &m : meshes) {
        if (m.ebo) gl->glDeleteBuffers(1, &m.ebo);
        if (m.vbo) gl->glDeleteBuffers(1, &m.vbo);
        if (m.vao) gl->glDeleteVertexArrays(1, &m.vao);
    }
    meshes.clear();
}

void RenderEngine::createProgram() {
    program.create();
    // program.addShaderFromSourceCode(QOpenGLShader::Vertex, VERT_SRC);
    // program.addShaderFromSourceCode(QOpenGLShader::Fragment, FRAG_SRC);
    // program.link();
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, VERT_SRC)) {
        qWarning() << "Vertex compile:" << program.log();
        return;
    }
    if (!program.addShaderFromSourceCode(QOpenGLShader::Fragment, FRAG_SRC)) {
        qWarning() << "Fragment compile:" << program.log();
        return;
    }
    if (!program.link()) {
        qWarning() << "Program link:" << program.log();
        return;
    }
}
