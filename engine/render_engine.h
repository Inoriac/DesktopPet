//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_RENDER_ENGINE_H
#define DESKTOP_PET_RENDER_ENGINE_H

#include <QOpenGLShaderProgram>

class QOpenGLFunctions_3_3_Core;

class RenderEngine {
public:
    RenderEngine();
    ~RenderEngine();

    // 传入当前上下文的 QOpenGLFunctions
    void initialize(QOpenGLFunctions_3_3_Core *glFuncs);
    void resize(int width, int height);
    void render();

private:
    void createTriangle();  // 将顶点组上传至GPU内，并设置绘制所需的缓冲与状态
    void destroyTriangle();
    void createProgram();

    QOpenGLFunctions_3_3_Core *gl {nullptr};    // 用于提供 OpenGL 的服务接口
    QOpenGLShaderProgram program;   // 着色器

    // 具体值是 GPU 内部资源的句柄
    unsigned int vao {0};   // 配置文件位置，用于指导如何使用资源
    unsigned int vbo {0};   // 数据存储位置

    int viewportWidth {0};
    int viewportHeight {0};
};

#endif //DESKTOP_PET_RENDER_ENGINE_H