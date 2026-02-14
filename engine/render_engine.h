//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_RENDER_ENGINE_H
#define DESKTOP_PET_RENDER_ENGINE_H

#include "model_loader.h"
#include "shader_manager.h"
#include "animation/animation_player.h"
#include <unordered_map>
#include <memory>

// 代表一个几何体
struct GpuMesh {
    // 具体值是 GPU 内部资源的句柄
    unsigned int vao {0};   // 配置文件位置，用于指导如何使用资源
    unsigned int vbo {0};   // 数据存储位置
    unsigned int ebo {0};   // 网格中顶点的连接方式
    int indexCount {0};
    int materialIndex {0};  // 材质索引
};

class QOpenGLFunctions_3_3_Core;

class RenderEngine {
public:
    RenderEngine();
    ~RenderEngine();

    void initialize(QOpenGLFunctions_3_3_Core *glFuncs, ShaderManager *shaderMgr);
    void setMaterials(std::vector<MaterialData> materialDatas);

    void resize(int width, int height);
    void render();

    // 模型上传
    void addMeshFromData(const MeshData& meshData);

    void ensureDefaultWhiteTexture();
    void uploadMaterialTextures(MaterialData& material);
    GLuint getDefaultWhiteTexture() const { return defaultWhiteTex; }

    void sortMeshesByMaterial();    // 按材质索引排序，减少材质切换次数

    void clearScene();

    // 动画相关方法
    void setAnimationPlayer(std::unique_ptr<AnimationPlayer> player);
    AnimationPlayer* getAnimationPlayer() const { return animationPlayer.get(); }
    void updateAnimation(float deltaTime);

private:
    QOpenGLFunctions_3_3_Core *gl {nullptr};    // 用于提供 OpenGL 的服务接口
    ShaderManager* shaderManager {nullptr};

    // 相机位置，后续可控
    QVector3D viewPos = QVector3D(0, 1.5f, 4);

    std::vector<GpuMesh> meshes;    // GPU 资源表
    std::vector<MaterialData> materials;    // 材质列表
    std::unordered_map<std::string, GLuint> textureCache;  // 材质缓存

    int viewportWidth {0};
    int viewportHeight {0};
    float angleDeg {0.0f};  // 渲染对象旋转角度

    GLuint defaultWhiteTex {0};
    const int targetSize = 1024;

    // 动画相关
    std::unique_ptr<AnimationPlayer> animationPlayer;

    // 摄像机调试控制
    // 默认视角：用户精调后的参数
    QVector3D cameraEye {-0.5f, 1.5f, 10.0f};
    QVector3D cameraCenter {-0.5f, 3.0f, 0.0f};

public:
    QVector3D getCameraEye() const { return cameraEye; }
    QVector3D getCameraCenter() const { return cameraCenter; }
    void setCameraEye(const QVector3D& v) { cameraEye = v; }
    void setCameraCenter(const QVector3D& v) { cameraCenter = v; }
};


#endif //DESKTOP_PET_RENDER_ENGINE_H