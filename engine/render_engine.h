//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_RENDER_ENGINE_H
#define DESKTOP_PET_RENDER_ENGINE_H

#include "shader_manager.h"

// 代表一个几何体
struct GpuMesh {
    // 具体值是 GPU 内部资源的句柄
    unsigned int vao {0};   // 配置文件位置，用于指导如何使用资源
    unsigned int vbo {0};   // 数据存储位置
    unsigned int ebo {0};   // 网格中顶点的连接方式
    int indexCount {0};
};

class QOpenGLFunctions_3_3_Core;

class RenderEngine {
public:
    RenderEngine();
    ~RenderEngine();

    // 传入当前上下文的 QOpenGLFunctions
    void initialize(QOpenGLFunctions_3_3_Core *glFuncs, ShaderManager *shaderMgr);
    void resize(int width, int height);
    void render();

    // 模型上传
    void addMesh(const std::vector<float>& interLeavePosColor, const std::vector<unsigned int>& indices);

    void clearScene();

private:
    QOpenGLFunctions_3_3_Core *gl {nullptr};    // 用于提供 OpenGL 的服务接口
    ShaderManager* shaderManager {nullptr};

    std::vector<GpuMesh> meshes;    // GPU 资源表
    int viewportWidth {0};
    int viewportHeight {0};
    float angleDeg {0.0f};  // 渲染对象旋转角度
};

#endif //DESKTOP_PET_RENDER_ENGINE_H