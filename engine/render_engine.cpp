//
// Created by Inoriac on 2025/10/15.
//

#include "render_engine.h"
#include <QOpenGLFunctions_3_3_Core>
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
    destroyTriangle();
}

void RenderEngine::initialize(QOpenGLFunctions_3_3_Core *glFuncs) {
    gl = glFuncs;
    createProgram();
    createTriangle();
    gl->glEnable(GL_DEPTH_TEST);
}

void RenderEngine::resize(int width, int height) {
    viewportWidth = width;
    viewportHeight = height;
    gl->glViewport(0, 0, width, height);
}

void RenderEngine::render() {
    // 保护
    if (!program.isLinked()) {
        gl->glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return;
    }

    gl->glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    QMatrix4x4 proj;
    float aspect = (viewportHeight > 0 ) ? float(viewportWidth) / float(viewportHeight) : 1.0f;
    proj.perspective(45.0f, aspect, 0.1f, 100.0f);
    QMatrix4x4 view;
    view.lookAt(QVector3D(0, 0, 3), QVector3D(0,0,0), QVector3D(0,1,0));
    QMatrix4x4 model;
    QMatrix4x4 mvp = proj * view * model;

    program.bind();
    program.setUniformValue("uMVP", mvp);

    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glBindVertexArray(0);

    program.release();
}

void RenderEngine::createTriangle() {
    // 位置(x,y,z) + 颜色(r,g,b)
    // 上传给 GPU 的顶点数据数组 每个点包含六个信息
    const float vertices[] = {
        0.0f,  0.6f,  0.0f,   1.0f, 0.3f, 0.3f,
       -0.6f, -0.4f,  0.0f,   0.3f, 1.0f, 0.3f,
        0.6f, -0.4f,  0.0f,   0.3f, 0.3f, 1.0f
   };

    // 分配实际的 GPU 对象
    gl->glGenVertexArrays(1, &vao);         // 生成VAO(顶点数组对象)
    gl->glGenBuffers(1, &vbo);              // 生成VBO(顶点缓冲对象)

    // 绑定
    gl->glBindVertexArray(vao);
    gl->glBindBuffer(GL_ARRAY_BUFFER, vbo); // 绑定至 GL_ARRAY_BUFFER 目标

    // 上传顶点数据至 GPU
    // GL_STATIC_DRAW 表示数据不会频繁修改，提示驱动将其放入显存中优化读取速度
    gl->glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // 配置顶点属性：位置
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    gl->glEnableVertexAttribArray(0);   // 启用索引0的顶点属性

    // 配置顶点属性：颜色
    gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    gl->glEnableVertexAttribArray(1);

    // 解绑缓冲与VAO，避免后续误操作，改写当前VAO/VBO
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glBindVertexArray(0);
}

void RenderEngine::destroyTriangle() {
    if (!gl) return;

    if (vbo) {
        gl->glDeleteBuffers(1, &vbo);
        vbo = 0;
    }

    if (vao) {
        gl->glDeleteVertexArrays(1, &vao);
        vao = 0;
    }
}

void RenderEngine::createProgram() {
    program.create();
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, VERT_SRC)) {
        qWarning() << "Vertex shader compile error:" << program.log();
        return;
    }
    if (!program.addShaderFromSourceCode(QOpenGLShader::Fragment, FRAG_SRC)) {
        qWarning() << "Fragment shader compile error:" << program.log();
        return;
    }
    if (!program.link()) {
        qWarning() << "Shader link error:" << program.log();
        return;
    }
}
