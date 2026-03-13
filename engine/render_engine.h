//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_RENDER_ENGINE_H
#define DESKTOP_PET_RENDER_ENGINE_H

#include <unordered_map>
#include <memory>

#include "model_loader.h"
#include "shader_manager.h"
#include "global_types.h"
#include "animation/animation_player.h"

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
    void initColliders();
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

    QVector3D getCameraEye() const { return cameraEye; }
    QVector3D getCameraCenter() const { return cameraCenter; }
    void setCameraEye(const QVector3D& v) { cameraEye = v; }
    void setCameraCenter(const QVector3D& v) { cameraCenter = v; }

    void setModelTransform(float scale, const QVector3D& offset) {
        modelScale = scale;
        modelOffset = offset;
    }
    float getModelScale() const { return modelScale; }  // 供射线检测使用

    // 动画相关方法
    void setAnimationPlayer(std::unique_ptr<AnimationPlayer> player);
    AnimationPlayer* getAnimationPlayer() const { return animationPlayer.get(); }
    void updateAnimation(float deltaTime);

    // 碰撞检测：根据屏幕坐标检测是否点击到模型，返回对应的交互标签
    std::string checkHit(int screenX, int screenY);

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

    float modelScale = 1.0f;    // 模型缩放比例
    QVector3D modelOffset;

    std::vector<BoneCollider> boneColliders;    // 碰撞体列表
    QMatrix4x4 currentModelMatrix;              // 记录当前的 Model 矩阵，用于保持渲染和检测一致

    // 动画相关
    std::unique_ptr<AnimationPlayer> animationPlayer;

    // 默认视角
    QVector3D cameraEye {0.0f, 3.0f, 12.0f};
    QVector3D cameraCenter {0.0f, 4.0f, 0.0f};
};


#endif //DESKTOP_PET_RENDER_ENGINE_H