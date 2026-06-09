//
// Created by Inoriac on 2025/10/15.
//

#include "stb_image.h"
#include "render_engine.h"

#include <cfloat>
#include <QOpenGLFunctions_3_3_Core>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QVector3D>
#include <QStringList>
#include <set>
#include <cmath>

#include "configLoader/config_manager.h"

RenderEngine::RenderEngine() = default;

void RenderEngine::initialize(QOpenGLFunctions_3_3_Core *glFuncs, ShaderManager *shaderMgr) {
    gl = glFuncs;
    shaderManager = shaderMgr;

    gl->glEnable(GL_DEPTH_TEST);    // 仅绘制离摄像机更近的片段
    gl->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);  // 设置多边形所有面均以实心模式绘制

    gl->glEnable(GL_BLEND);
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);  // 支持读取透明度

    // gl->glDisable(GL_CULL_FACE); // 禁用背面剔除
}

void RenderEngine::initColliders() {
    boneColliders.clear();

    const auto& configs = ConfigManager::instance().getColliderConfigs();
    for (const auto& config : configs) {
        BoneCollider collider = config; // 复制配置
        boneColliders.push_back(collider);
    }
    // qDebug() << "Initialized" << boneColliders.size() << "runtime colliders (Radius fixed, no scale applied)";
}

void RenderEngine::setMaterials(std::vector<MaterialData> materialDatas) {
    materials = materialDatas;
}

void RenderEngine::resize(int width, int height) {
    viewportWidth = width;
    viewportHeight = height;
    gl->glViewport(0, 0, width, height);
}

void RenderEngine::render() {
    static QElapsedTimer timer; // 累计记时计时器
    if (!timer.isValid()) timer.start(); // 第一次调用时初始化
    float deltaSec = timer.restart() / 1000.0f; // 毫秒转秒，方便做平滑动画

    if (!shaderManager || meshes.empty()) {
        gl->glClearColor(0.2f, 0.6f, 0.9f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return;
    }

    // 清屏
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);      // 背景透明
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);     // 清除颜色与深度缓冲

    // 旋转角度（每秒旋转 60 度）(帧率无关)
    // angleDeg += 60.0f * deltaSec;
    // if (angleDeg >= 360.0f) angleDeg -= 360.0f;

    // 构建变换矩阵
    // 投影矩阵
    QMatrix4x4 proj;
    float aspect = (viewportHeight > 0) ? float(viewportWidth) / float(viewportHeight) : 1.0f;
    proj.perspective(45.0f, aspect, 0.1f, 100.0f);  // 近大远小

    // 视图矩阵(控制相机位置与方向)
    QMatrix4x4 view;
    // 使用成员变量控制相机
    view.lookAt(cameraEye, cameraCenter, QVector3D(0.0f, 1.0f, 0.0f));

    // 模型矩阵
    QMatrix4x4 model;

    // 光标追踪驱动的整体 Y 轴旋转（平滑），用于保持正立并体现“向内/外”转向。
    const float yawAlpha = 1.0f - std::exp(-8.0f * deltaSec);
    trackingYawCurrentDeg += (trackingYawTargetDeg - trackingYawCurrentDeg) * yawAlpha;

    // 先做绕世界 Y 轴转向，再做模型朝向修正，避免轴向错位导致的错误观感。
    model.rotate(trackingYawCurrentDeg, 0.0f, 1.0f, 0.0f);

    // 姿态修正：把躺着的模型扶正
    model.rotate(90.0f, 1.0f, 0.0f, 0.0f);

    // 缩放
    float finalScale = modelScale;
    if (finalScale <= 0.0001f) finalScale = 1.0f;
    model.scale(finalScale);

    // 兼容旧调试旋转（默认 angleDeg=0）
    model.rotate(angleDeg, 0.0f, 1.0f, 0.0f);

    // 应用中心偏移
    // model.translate(modelOffset);

    currentModelMatrix = model;

    // 综合矩阵 遵循右乘 结果等价于先把顶点从模型空间变换到世界坐标，再到视图，最后进行投影
    QMatrix4x4 mvp = proj * view * model;

    auto* shader = shaderManager->getShader("pbr");
    if (!shader) {
        qWarning() << "PBR shader not fount, falling back to default";
        return;
    }
    shader->bind();



    if (animationPlayer) {
        std::vector<QMatrix4x4> transforms = animationPlayer->getCurrentTransforms();

        // DEBUG用信息
        // static int debugFrame = 0;
        // if (++debugFrame % 180 == 0 && !transforms.empty()) {
        //     QMatrix4x4 m = transforms[0]; // 打印第一根骨骼的矩阵
        //     qDebug() << "Bone[0] Matrix:" << m.row(0) << m.row(1) << m.row(2) << m.row(3);
        //
        //     // 检查是否也是单位矩阵？
        //     if (m.isIdentity()) qDebug() << "WARNING: Matrix is Identity (T-Pose)";
        //
        //     // 检查是否全0？(如果全0，屏幕上就是一坨)
        //     bool isZero = true;
        //     for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(abs(m(r,c)) > 0.0001) isZero = false;
        //     if (isZero) qDebug() << "CRITICAL: Matrix is ZERO! This causes the collapse.";
        // }

        if (!transforms.empty()) {
            // 限制最大数量防止越界（假设 Shader 里数组大小是 200）
            int count = std::min((int)transforms.size(), 200);
            shader->setUniformValueArray("finalBonesMatrices[0]", transforms.data(), count);
        } else {
            // 如果数据未就绪，传单位矩阵防止错误
            QMatrix4x4 id; shader->setUniformValue("finalBonesMatrices[0]", id);
        }
    } else {
        QMatrix4x4 id; shader->setUniformValue("finalBonesMatrices[0]", id);
    }

    // 光照与视角 - 使用当前的摄像机位置
    QVector3D viewPos = cameraEye;

    QVector3D lightPositions[4] = {
        QVector3D(3.0f, 5.0f, 5.0f),    // 主光源：右前上方
        QVector3D(-3.0f, 5.0f, 5.0f),   // 辅光源：左前上方
        QVector3D(0.0f, 2.0f, -5.0f),   // 背光/轮廓光：后方
        QVector3D(0.0f, 0.5f, 3.0f)     // 底部补光：正前下方
    };

    // PBR 光照强度调整 (修复过曝)
    // 采用"真实感"配置：降低整体亮度，主光带暖色(太阳)，辅光带冷色(天空)，轮廓光增强立体感
    QVector3D lightColors[4] = {
        QVector3D(100.0f, 96.0f, 90.0f),    // 主光：暖白 (强度约为之前的1/3)
        QVector3D(50.0f, 55.0f, 60.0f),     // 辅光：冷白 (模拟环境天光)
        QVector3D(60.0f, 60.0f, 70.0f),     // 背光：冷色，勾勒轮廓
        QVector3D(20.0f, 20.0f, 20.0f)      // 底光：微弱补光，防止死黑
    };

    int lastMaterialIndex = -1; // 记录上一个使用的材质索引，用于性能优化

    // 绘制每个 mesh
    for (const auto &mesh: meshes) {
        if (mesh.materialIndex < 0 || mesh.materialIndex >= materials.size()) {
            qWarning() << "Mesh has invalid material index:" << mesh.materialIndex;
            continue;
        }

        if (mesh.materialIndex != lastMaterialIndex) {
            // 取出对应材质
            const auto &mat = materials[mesh.materialIndex];

            // 绑定纹理
            shaderManager->bindPBRTextures(mat, shader, defaultWhiteTex);

            // 上传材质 + 矩阵 + 光照 uniform
            shaderManager->applyPBRUniforms(shader,
            model, view, proj,
            mat.baseColorFactor,
            mat.alphaFactor,
            mat.metallicFactor,
            mat.roughnessFactor,
            viewPos,
            lightPositions,
            lightColors,
            4
            );

            lastMaterialIndex = mesh.materialIndex;
        }

        // 绘制
        gl->glBindVertexArray(mesh.vao);
        gl->glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
        gl->glBindVertexArray(0);
    }

    // 清理状态
    gl->glBindVertexArray(0);
    shader->release();
}

