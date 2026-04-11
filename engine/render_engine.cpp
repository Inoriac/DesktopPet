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

RenderEngine::~RenderEngine() {
    if (!gl) return;

    // 清理网格资源
    for (auto &m : meshes) {
        if (m.ebo) gl->glDeleteBuffers(1, &m.ebo);
        if (m.vbo) gl->glDeleteBuffers(1, &m.vbo);
        if (m.vao) gl->glDeleteVertexArrays(1, &m.vao);
    }
    meshes.clear();

    // 清理材质纹理资源
    for (auto &mat : materials) {
        if (mat.albedoTexID) gl->glDeleteTextures(1, &mat.albedoTexID);
        if (mat.normalTexID) gl->glDeleteTextures(1, &mat.normalTexID);
        if (mat.metallicRoughnessTexID) gl->glDeleteTextures(1, &mat.metallicRoughnessTexID);
        if (mat.aoTexID) gl->glDeleteTextures(1, &mat.aoTexID);
        if (mat.emissiveTexID) gl->glDeleteTextures(1, &mat.emissiveTexID);
    }
    materials.clear();

    // 清理默认纹理
    if (defaultWhiteTex) {
        gl->glDeleteTextures(1, &defaultWhiteTex);
        defaultWhiteTex = 0;
    }
}

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
    qDebug() << "Initialized" << boneColliders.size() << "runtime colliders (Radius fixed, no scale applied)";
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

void RenderEngine::setTrackingYawInput(float normalizedX) {
    const float x = std::clamp(normalizedX, -1.0f, 1.0f);
    constexpr float kMaxTrackingYawDeg = 28.0f;
    trackingYawTargetDeg = x * kMaxTrackingYawDeg;
}

int RenderEngine::findBoneIndexByKeywords(const Skeleton& skeleton, const QStringList& keywords) const {
    for (const auto& pair : skeleton.nameToIndex) {
        QString lowered = QString::fromStdString(pair.first).toLower();
        for (const QString& key : keywords) {
            if (lowered == key) return pair.second;
        }
    }

    for (const auto& pair : skeleton.nameToIndex) {
        QString lowered = QString::fromStdString(pair.first).toLower();
        for (const QString& key : keywords) {
            if (lowered.contains(key)) return pair.second;
        }
    }

    return -1;
}

bool RenderEngine::getHeadScreenPosition(QVector2D& outViewportPos) {
    if (!animationPlayer || viewportWidth <= 0 || viewportHeight <= 0) return false;

    const Skeleton& skeleton = animationPlayer->getSkeleton();
    const int headBoneIndex = findBoneIndexByKeywords(skeleton, {"head", "j_bip_c_head"});
    if (headBoneIndex < 0) return false;

    // 刷新一遍矩阵缓存，确保 getGlobalTransforms 返回本帧姿态。
    (void)animationPlayer->getCurrentTransforms();
    const auto& boneTransforms = animationPlayer->getGlobalTransforms();
    if (headBoneIndex >= static_cast<int>(boneTransforms.size())) return false;

    QMatrix4x4 proj;
    float aspect = (viewportHeight > 0) ? float(viewportWidth) / float(viewportHeight) : 1.0f;
    proj.perspective(45.0f, aspect, 0.1f, 100.0f);

    QMatrix4x4 view;
    view.lookAt(cameraEye, cameraCenter, QVector3D(0.0f, 1.0f, 0.0f));

    QMatrix4x4 model;
    model.rotate(trackingYawCurrentDeg, 0.0f, 1.0f, 0.0f);
    model.rotate(90.0f, 1.0f, 0.0f, 0.0f);

    float finalScale = modelScale;
    if (finalScale <= 0.0001f) finalScale = 1.0f;
    model.scale(finalScale);
    model.rotate(angleDeg, 0.0f, 1.0f, 0.0f);

    QVector4D headWorld = model * boneTransforms[headBoneIndex] * QVector4D(0.0f, 0.0f, 0.0f, 1.0f);
    QVector4D clip = proj * view * headWorld;
    if (std::fabs(clip.w()) < 1e-6f) return false;

    QVector3D ndc = clip.toVector3D() / clip.w();
    float sx = (ndc.x() * 0.5f + 0.5f) * viewportWidth;
    float sy = (1.0f - (ndc.y() * 0.5f + 0.5f)) * viewportHeight;

    outViewportPos = QVector2D(sx, sy);
    return true;
}

void RenderEngine::addMeshFromData(const MeshData &meshData) {
    GpuMesh m;

    // 添加调试信息
    qDebug() << "=== Mesh Data Debug ===";
    qDebug() << "Vertex count:" << meshData.vertices.size() / 3;
    qDebug() << "Index count:" << meshData.indices.size();
    qDebug() << "Material index:" << meshData.materialIndex;

    // 检查顶点数据范围
    if (!meshData.vertices.empty()) {
        float minX = meshData.vertices[0], maxX = meshData.vertices[0];
        float minY = meshData.vertices[1], maxY = meshData.vertices[1];
        float minZ = meshData.vertices[2], maxZ = meshData.vertices[2];

        for (size_t i = 0; i < meshData.vertices.size(); i += 3) {
            minX = qMin(minX, meshData.vertices[i]);
            maxX = qMax(maxX, meshData.vertices[i]);
            minY = qMin(minY, meshData.vertices[i+1]);
            maxY = qMax(maxY, meshData.vertices[i+1]);
            minZ = qMin(minZ, meshData.vertices[i+2]);
            maxZ = qMax(maxZ, meshData.vertices[i+2]);
        }

        qDebug() << "Vertex bounds - X:" << minX << "to" << maxX;
        qDebug() << "Vertex bounds - Y:" << minY << "to" << maxY;
        qDebug() << "Vertex bounds - Z:" << minZ << "to" << maxZ;
    }

    // 分配 GPU 资源
    gl->glGenVertexArrays(1, &m.vao);
    gl->glGenBuffers(1, &m.vbo);
    gl->glGenBuffers(1, &m.ebo);

    // 激活顶点配置上下文
    gl->glBindVertexArray(m.vao);

    // 准备顶点数据(位置 + 法线 + UV)
    std::vector<float> interleaveData;

    // 确保所有数据长度一致
    size_t vertexCount = meshData.vertices.size() / 3;
    size_t normalCount = meshData.normals.size() / 3;
    size_t uvCount = meshData.uvs.size() / 2;

    qDebug() << "Processing mesh with" << vertexCount << " vertices";
    qDebug() << "Normals:" << normalCount << "UVs:" << uvCount;

    // 交错存储
    for (size_t i = 0; i< vertexCount; i++) {
        // 位置
        interleaveData.push_back(meshData.vertices[i * 3 + 0]);
        interleaveData.push_back(meshData.vertices[i * 3 + 1]);
        interleaveData.push_back(meshData.vertices[i * 3 + 2]);

        // 法线
        if (i < normalCount) {
            interleaveData.push_back(meshData.normals[i * 3 + 0]);
            interleaveData.push_back(meshData.normals[i * 3 + 1]);
            interleaveData.push_back(meshData.normals[i * 3 + 2]);
        } else {    // 默认值
            interleaveData.push_back(0.0f);
            interleaveData.push_back(0.0f);
            interleaveData.push_back(1.0f);
        }

        // UV 坐标
        if (i < uvCount) {
            interleaveData.push_back(meshData.uvs[i * 2 + 0]);
            interleaveData.push_back(meshData.uvs[i * 2 + 1]);
        } else {
            interleaveData.push_back(0.0f);
            interleaveData.push_back(0.0f);
        }

        // 骨骼索引
        if (meshData.hasSkinning && !meshData.boneIndices.empty()) {
            interleaveData.push_back((float)meshData.boneIndices[i * 4 + 0]);
            interleaveData.push_back((float)meshData.boneIndices[i * 4 + 1]);
            interleaveData.push_back((float)meshData.boneIndices[i * 4 + 2]);
            interleaveData.push_back((float)meshData.boneIndices[i * 4 + 3]);
        } else {
            interleaveData.push_back(-1.0f);
            interleaveData.push_back(-1.0f);
            interleaveData.push_back(-1.0f);
            interleaveData.push_back(-1.0f);
        }

        // 骨骼权重
        if (meshData.hasSkinning && !meshData.boneWeights.empty()) {
            interleaveData.push_back(meshData.boneWeights[i * 4 + 0]);
            interleaveData.push_back(meshData.boneWeights[i * 4 + 1]);
            interleaveData.push_back(meshData.boneWeights[i * 4 + 2]);
            interleaveData.push_back(meshData.boneWeights[i * 4 + 3]);
        } else {
            interleaveData.push_back(0.0f);
            interleaveData.push_back(0.0f);
            interleaveData.push_back(0.0f);
            interleaveData.push_back(0.0f);
        }
    }
    // 上传顶点数据
    gl->glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    gl->glBufferData(GL_ARRAY_BUFFER,
        GLsizeiptr(interleaveData.size() * sizeof(float)),
        interleaveData.data(),
        GL_STATIC_DRAW);

    // 上传索引数据
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        GLsizeiptr(meshData.indices.size() * sizeof(unsigned int)),
        meshData.indices.data(),
        GL_STATIC_DRAW);

    // 配置顶点属性格式
    int stride = 16 * sizeof(float);

    // 位置 (Location = 0)
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    gl->glEnableVertexAttribArray(0);

    // 法线(Location = 1)
    gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    gl->glEnableVertexAttribArray(1);

    // UV 坐标(Location = 2)
    gl->glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    gl->glEnableVertexAttribArray(2);

    // 骼索引 (Location = 3)
    gl->glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(8 * sizeof(float)));
    gl->glEnableVertexAttribArray(3);

    // 骨骼权重 (Location = 4)
    gl->glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(12 * sizeof(float)));
    gl->glEnableVertexAttribArray(4);

    gl->glBindVertexArray(0);

    m.indexCount = int(meshData.indices.size());
    m.materialIndex = meshData.materialIndex;

    meshes.push_back(m);

    qDebug() << "Mesh added to GPU with" << m.indexCount << " indices";
}

void RenderEngine::ensureDefaultWhiteTexture() {
    if (defaultWhiteTex) return;
    unsigned char whitePixel[4] = { 255, 255, 255, 255 };
    gl->glGenTextures(1, &defaultWhiteTex);
    gl->glBindTexture(GL_TEXTURE_2D, defaultWhiteTex);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
}

void RenderEngine::uploadMaterialTextures(MaterialData &material) {
    ensureDefaultWhiteTexture();

    // Lambda：从原始字节数据上传纹理（用于内嵌纹理）
    auto uploadFromRaw = [&](const std::vector<unsigned char> &imageData, int width, int height, GLuint &outTexId, bool srgb=false) {
        if (imageData.empty() || width <= 0 || height <= 0) {
            outTexId = 0;
            return;
        }
        gl->glGenTextures(1, &outTexId);
        gl->glBindTexture(GL_TEXTURE_2D, outTexId);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        GLint internalFmt = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        gl->glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, imageData.data());
        gl->glGenerateMipmap(GL_TEXTURE_2D);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
    };

    // Lambda：从文件路径加载并上传纹理（用于外部纹理文件）
    auto uploadFromFile = [&](const std::string &texPath, GLuint &outTexId, bool srgb=false) {
        if (texPath.empty()) {
            outTexId = 0;
            return;
        }

        // 检查缓存是否命中
        auto it = textureCache.find(texPath);
        if (it != textureCache.end()) {
            outTexId = it->second;
            qDebug() << "[CACHED] Texture reused:" << texPath.c_str() << "ID:" << outTexId;
            return;
        }

        // 缓存未命中
        // 使用 stb_image 加载
        int originalWidth, originalHeight, channels;
        unsigned char* originalData = stbi_load(texPath.c_str(), &originalWidth, &originalHeight, &channels, 4);

        if (!originalData) {
            qWarning() << "Failed to load texture:" << texPath.c_str();
            outTexId = 0;
            return;
        }

        int finalWidth = originalWidth;
        int finalHeight = originalHeight;
        unsigned char* finalData = originalData;

        // 如果原始尺寸大于目标，进行缩放
        if (originalWidth > targetSize || originalHeight > targetSize) {
            // 这里用简单的双线性插值缩放
            finalWidth = targetSize;
            finalHeight = targetSize;

            // 分配新的缓冲区
            finalData = new unsigned char[finalWidth * finalHeight * 4];

            // 简单的最近邻采样（快速但质量较低）
            for (int y = 0; y < finalHeight; y++) {
                for (int x = 0; x < finalWidth; x++) {
                    int srcX = x * originalWidth / finalWidth;
                    int srcY = y * originalHeight / finalHeight;

                    int srcIdx = (srcY * originalWidth + srcX) * 4;
                    int dstIdx = (y * finalWidth + x) * 4;

                    finalData[dstIdx + 0] = originalData[srcIdx + 0];
                    finalData[dstIdx + 1] = originalData[srcIdx + 1];
                    finalData[dstIdx + 2] = originalData[srcIdx + 2];
                    finalData[dstIdx + 3] = originalData[srcIdx + 3];
                }
            }

            qDebug() << "Resized texture from" << originalWidth << "x" << originalHeight
                     << "to" << finalWidth << "x" << finalHeight;
        }

        // 上传到 GPU
        gl->glGenTextures(1, &outTexId);
        gl->glBindTexture(GL_TEXTURE_2D, outTexId);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        GLint internalFmt = GL_RGBA8;
        gl->glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, finalWidth, finalHeight,
                        0, GL_RGBA, GL_UNSIGNED_BYTE, finalData);
        gl->glGenerateMipmap(GL_TEXTURE_2D);
        gl->glBindTexture(GL_TEXTURE_2D, 0);

        // 立即释放内存
        if (finalData != originalData) {
            // 缩放后的数据用 delete[] 释放
            delete[] finalData;
        }
        // 原始数据用 stbi_image_free 释放
        stbi_image_free(originalData);

        // 加入缓存
        textureCache[texPath] = outTexId;

        qDebug() << "[LOADED] Texture:" << texPath.c_str() << "ID:" << outTexId
                 << "Size:" << finalWidth << "x" << finalHeight;
    };

    // === 上传 albedo 纹理（优先内嵌数据，否则从文件加载）===
    if (!material.albedoImageData.empty()) {
        uploadFromRaw(material.albedoImageData, material.albedoWidth, material.albedoHeight, material.albedoTexID, true);
    } else if (!material.albedoTexPath.empty()) {
        uploadFromFile(material.albedoTexPath, material.albedoTexID, true);
    } else {
        material.albedoTexID = 0;
    }

    // === 上传 normal 纹理 ===
    if (!material.normalImageData.empty()) {
        uploadFromRaw(material.normalImageData, material.normalWidth, material.normalHeight, material.normalTexID, false);
    } else if (!material.normalTexPath.empty()) {
        uploadFromFile(material.normalTexPath, material.normalTexID, false);
    } else {
        material.normalTexID = 0;
    }

    // === 上传 metallic-roughness 纹理 ===
    if (!material.metallicRoughnessImageData.empty()) {
        uploadFromRaw(material.metallicRoughnessImageData, material.metallicRoughnessWidth, material.metallicRoughnessHeight, material.metallicRoughnessTexID, false);
    } else if (!material.metallicRoughnessTexPath.empty()) {
        uploadFromFile(material.metallicRoughnessTexPath, material.metallicRoughnessTexID, false);
    } else {
        material.metallicRoughnessTexID = 0;
    }

    // === 上传 AO 纹理 ===
    if (!material.aoImageData.empty()) {
        uploadFromRaw(material.aoImageData, material.aoWidth, material.aoHeight, material.aoTexID, false);
    } else if (!material.aoTexPath.empty()) {
        uploadFromFile(material.aoTexPath, material.aoTexID, false);
    } else {
        material.aoTexID = 0;
    }

    // === 上传 emissive 纹理 ===
    if (!material.emissiveImageData.empty()) {
        uploadFromRaw(material.emissiveImageData, material.emissiveWidth, material.emissiveHeight, material.emissiveTexID, true);
    } else if (!material.emissiveTexPath.empty()) {
        uploadFromFile(material.emissiveTexPath, material.emissiveTexID, true);
    } else {
        material.emissiveTexID = 0;
    }

    // 如果某贴图没上传成功，保留 texID = 0
}

void RenderEngine::sortMeshesByMaterial() {
    std::sort(meshes.begin(), meshes.end(),
        [](const GpuMesh& a, const GpuMesh& b) {
            return a.materialIndex < b.materialIndex;
        });
    qDebug() << "Meshes sorted by material index for optimal rendering";
}

std::string RenderEngine::checkHit(int viewX, int viewY) {
    if (!animationPlayer || boneColliders.empty()) return "";

    // 获取变换矩阵
    QMatrix4x4 view;
    view.lookAt(cameraEye, cameraCenter, QVector3D(0, 1, 0));

    QMatrix4x4 proj;
    float aspect = (viewportHeight > 0) ? float(viewportWidth) / float(viewportHeight) : 1.0f;
    proj.perspective(45.0f, aspect, 0.1f, 100.0f); // 0.1f to match render

    QMatrix4x4 vpMatrix = proj * view;

    int glY = viewportHeight - viewY; // OpenGL 的 Y 轴是反向的

    QVector2D mouseScreen(static_cast<float>(viewX), static_cast<float>(glY));

    // 获取骨骼数据
    const auto& boneTransforms = animationPlayer->getGlobalTransforms();
    const auto& skeleton = animationPlayer->getSkeleton();

    float minDist2 = FLT_MAX;
    std::string hitTag = "";

    // 用于自动将半径从世界空间投影到屏幕空间的辅助 lambda
    auto worldToScreen = [&](const QVector3D& worldPos) -> QVector2D {
        QVector4D clip = vpMatrix * QVector4D(worldPos, 1.0f);
        if (std::fabs(clip.w()) < 1e-6f) return QVector2D(-1e6f, -1e6f); // 避免除以零
        QVector3D ndc = clip.toVector3D() / clip.w();  // 透视除法：clip → NDC
        float sx = (ndc.x() * 0.5f + 0.5f) * viewportWidth;
        float sy = (ndc.y() * 0.5f + 0.5f) * viewportHeight;
        return QVector2D(sx, sy);
    };

    // 相机右方向（用于投影半径）
    QVector3D camRight = QVector3D::crossProduct(
        (cameraCenter - cameraEye).normalized(),
        QVector3D(0, 1, 0)
    ).normalized();

    for (const auto& collider : boneColliders) {
        int boneIndex = -1;
        auto it = skeleton.nameToIndex.find(collider.boneName);
        if (it != skeleton.nameToIndex.end()) {
            boneIndex = it->second;
        } else {
            std::string targetName = collider.boneName;
            for (const auto& pair : skeleton.nameToIndex) {
                const std::string& name = pair.first;
                if (name.size() >= targetName.size() &&
                    name.compare(name.size() - targetName.size(), targetName.size(), targetName) == 0) {
                    if (name.size() == targetName.size()) {
                        boneIndex = pair.second;
                        break;
                    }
                    char separator = name[name.size() - targetName.size() - 1];
                    if (separator == ':' || separator == '_' || separator == ' ') {
                        boneIndex = pair.second;
                        break;
                    }
                }
            }
        }
        if (boneIndex == -1 || boneIndex >= static_cast<int>(boneTransforms.size())) continue;

        // 计算骨骼在世界空间的位置
        // 骨骼本地偏移（跟随骨骼旋转）
        QVector3D boneWorldPos = currentModelMatrix * (boneTransforms[boneIndex] * collider.offset);
        // 加上世界空间固定偏移（不跟随骨骼旋转）
        boneWorldPos += collider.worldOffset;

        // 投影到 2D 屏幕空间
        QVector2D boneScreen = worldToScreen(boneWorldPos);

        // 投影半径：世界空间 hoverRadius 在屏幕上有多大
        QVector2D edgeScreen = worldToScreen(boneWorldPos + camRight * collider.hoverRadius);
        float screenRadius2 = (boneScreen - edgeScreen).lengthSquared();

        // 2D 距离判断
        float dist2 = (mouseScreen - boneScreen).lengthSquared();

        // === DEBUG: 打印每个碰撞体的投影信息 ===
        float screenRadius = std::sqrt(screenRadius2);
        float dist = std::sqrt(dist2);
        qDebug().noquote() << QString("  [HitCheck] %1 (tag:%2) boneIdx:%3 worldPos:(%4,%5,%6) screenPos:(%7,%8) mouse:(%9,%10) screenR:%11px dist:%12px %13")
            .arg(collider.boneName.c_str())
            .arg(collider.tag.c_str())
            .arg(boneIndex)
            .arg(boneWorldPos.x(), 0, 'f', 3).arg(boneWorldPos.y(), 0, 'f', 3).arg(boneWorldPos.z(), 0, 'f', 3)
            .arg(boneScreen.x(), 0, 'f', 1).arg(boneScreen.y(), 0, 'f', 1)
            .arg(mouseScreen.x(), 0, 'f', 1).arg(mouseScreen.y(), 0, 'f', 1)
            .arg(screenRadius, 0, 'f', 1)
            .arg(dist, 0, 'f', 1)
            .arg(dist2 <= screenRadius2 ? "HIT" : "miss");

        if (dist2 <= screenRadius2) {
            // 命中，取最近的（屏幕距离最近 = 最精确匹配）
            if (dist2 < minDist2) {
                minDist2 = dist2;
                hitTag = collider.tag;
            }
        }
    }
    return hitTag;
}

// bool RenderEngine::intersectRaySphere(const QVector3D &rayOrigin, const QVector3D &rayDir, const QVector3D &sphereCenter, float sphereRadius, float &outDist) {
//     QVector3D m = rayOrigin - sphereCenter;
//     float b = QVector3D::dotProduct(m, rayDir);
//     float c = QVector3D::dotProduct(m, m) - sphereRadius * sphereRadius;
//     // 射线起点在球外且指向远离球心的方向
//     if (c > 0.0f && b > 0.0f) return false;
//
//     float discr = b * b - c;
//     if (discr < 0.0f) return false;
//
//     // 计算最近交点
//     float t = -b - sqrt(discr);
//     if (t < 0.0f) t = 0.0f;
//
//     outDist = t;
//     return true;
// }
//
// bool RenderEngine::intersectRayCapsule(const QVector3D &rayOrigin, const QVector3D &rayDir,
//                                        const QVector3D &capsuleA, const QVector3D &capsuleB,
//                                        float capsuleRadius, float &outDist) {
//     QVector3D ab = capsuleB - capsuleA;
//     float abLenSq = QVector3D::dotProduct(ab, ab);
//
//     // 退化为球体
//     if (abLenSq < 1e-8f) {
//         return intersectRaySphere(rayOrigin, rayDir, capsuleA, capsuleRadius, outDist);
//     }
//
//     QVector3D w0 = rayOrigin - capsuleA;
//     float a = QVector3D::dotProduct(rayDir, rayDir); // 理论上为1
//     float b = QVector3D::dotProduct(rayDir, ab);
//     float c = abLenSq;
//     float d = QVector3D::dotProduct(rayDir, w0);
//     float e = QVector3D::dotProduct(ab, w0);
//
//     float denom = a * c - b * b;
//     float tRay = 0.0f;
//     float tSeg = 0.0f;
//
//     if (std::fabs(denom) > 1e-8f) {
//         tRay = (b * e - c * d) / denom;
//         tSeg = (a * e - b * d) / denom;
//     } else {
//         // 近平行：固定在射线起点投影
//         tRay = 0.0f;
//         tSeg = e / c;
//     }
//
//     // 约束到有效区间
//     if (tRay < 0.0f) tRay = 0.0f;
//     if (tSeg < 0.0f) tSeg = 0.0f;
//     if (tSeg > 1.0f) tSeg = 1.0f;
//
//     // 约束 segment 后重新计算 ray 参数
//     tRay = -(d + b * tSeg) / a;
//     if (tRay < 0.0f) {
//         tRay = 0.0f;
//         tSeg = e / c;
//         if (tSeg < 0.0f) tSeg = 0.0f;
//         if (tSeg > 1.0f) tSeg = 1.0f;
//     }
//
//     QVector3D closestRay = rayOrigin + rayDir * tRay;
//     QVector3D closestSeg = capsuleA + ab * tSeg;
//     QVector3D delta = closestRay - closestSeg;
//     float distSq = QVector3D::dotProduct(delta, delta);
//
//     if (distSq <= capsuleRadius * capsuleRadius) {
//         outDist = tRay;
//         return true;
//     }
//
//     return false;
// }

void RenderEngine::clearScene() {
    if (!gl) return;

    // 清理网格资源
    for (auto &m : meshes) {
        if (m.ebo) gl->glDeleteBuffers(1, &m.ebo);
        if (m.vbo) gl->glDeleteBuffers(1, &m.vbo);
        if (m.vao) gl->glDeleteVertexArrays(1, &m.vao);
    }
    meshes.clear();

    // 清理材质纹理资源
    for (auto &mat :materials) {
        if (mat.albedoTexID) gl->glDeleteTextures(1, &mat.albedoTexID);
        if (mat.normalTexID) gl->glDeleteTextures(1, &mat.normalTexID);
        if (mat.metallicRoughnessTexID) gl->glDeleteTextures(1, &mat.metallicRoughnessTexID);
        if (mat.aoTexID) gl->glDeleteTextures(1, &mat.aoTexID);
        if (mat.emissiveTexID) gl->glDeleteTextures(1, &mat.emissiveTexID);
    }
    materials.clear();

    // 清理动画播放器
    animationPlayer.reset();
}

void RenderEngine::setAnimationPlayer(std::unique_ptr<AnimationPlayer> player) {
    animationPlayer = std::move(player);
}

void RenderEngine::updateAnimation(float deltaTime) {
    if (animationPlayer) {
        animationPlayer->update(deltaTime);
    }
}
