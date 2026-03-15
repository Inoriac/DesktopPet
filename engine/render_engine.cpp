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
        // 注意：我们不再缩放半径。配置中的半径应被视为"世界空间/标准化空间"的单位。
        // 由于我们将所有模型标准化为高度~3.0，配置的半径(如0.25)应直接适用，
        // 而不应受到原始模型尺寸(modelScale)的影响。
        // collider.radius *= this->modelScale; 
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

    // 姿态修正：把躺着的模型扶正
    model.rotate(90.0f, 1.0f, 0.0f, 0.0f);

    // 缩放
    float finalScale = modelScale;
    if (finalScale <= 0.0001f) finalScale = 1.0f;
    model.scale(finalScale);

    // 绕 Y 轴旋转
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

    // 光源围绕相机位置布置（始终跟随相机）
    // 由于相机距离变远了(从4变到10)，光源也随之变远，因此需要大幅增强强度，或者让光源靠近模型
    // 这里我们选择让光源尽量靠近模型(假设模型在 0,0,0 附近)，而不是跟随相机
    // 或者我们保持跟随相机，但大幅增强强度
    
    // 方案B：让光源作为"摄影棚灯光"固定在模型周围，而非跟随相机
    // 这样无论相机在哪，模型都被照亮
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

    int glY = viewportHeight - viewY; // OpenGL 的 Y 轴是反向的

    // 生成射线
    // 视口区域 (x, y, w, h)
    QRect viewport(0, 0, viewportWidth, viewportHeight);

    QVector3D nearPoint = QVector3D(viewX, glY, 0.0f).unproject(view, proj, viewport);
    QVector3D farPoint = QVector3D(viewX, glY, 1.0f).unproject(view, proj, viewport);

    QVector3D rayOrigin = nearPoint;
    QVector3D rayDir = (farPoint - nearPoint).normalized();

    // qDebug() << "=== CheckHit Debug ===";
    // qDebug() << "Mouse:" << viewX << viewY << "GL_Y:" << glY;
    // qDebug() << "Ray Origin:" << rayOrigin;
    // qDebug() << "Ray Dir:" << rayDir;
    // qDebug() << "Colliders Count:" << boneColliders.size();

    // 遍历碰撞体
    // 获取骨骼数据
    const auto& boneTransforms = animationPlayer->getGlobalTransforms();
    const auto& skeleton = animationPlayer->getSkeleton();

    // qDebug() << "Skeleton Bones Count:" << skeleton.nameToIndex.size();

    float minDist = FLT_MAX;
    std::string hitTag = "";
    constexpr float clickToleranceScale = 2.4f;

    // 调试：打印所有骨骼名称(仅打印前5个作为示例)
    // if (!skeleton.nameToIndex.empty()) {
    //     qDebug() << "Example Bone Names in Skeleton:";
    //     int count = 0;
    //     for (const auto& pair : skeleton.nameToIndex) {
    //         qDebug() << " - " << pair.first.c_str();
    //         if (++count >= 5) break;
    //     }
    // }

    for (const auto& collider : boneColliders) {
        // 查找骨骼索引
        int boneIndex = -1;
        auto it = skeleton.nameToIndex.find(collider.boneName);
        
        if (it != skeleton.nameToIndex.end()) {
            boneIndex = it->second;
        } else {
             // 智能匹配: 尝试寻找以 "Separator + Name" 结尾的骨骼
             // 支持的分隔符: ':', '_', ' '
             // 例如配置 "Head" 可以匹配 "mixamorig:Head", "Bip001_Head", "Character Head"
             std::string targetName = collider.boneName;
             for (const auto& pair : skeleton.nameToIndex) {
                 const std::string& name = pair.first;
                 // 检查是否以 targetName 结尾
                 if (name.size() >= targetName.size() && 
                     name.compare(name.size() - targetName.size(), targetName.size(), targetName) == 0) {
                     
                     // 如果长度相等，就是精确匹配（虽然前面已经check过了，但安全起见）
                     if (name.size() == targetName.size()) {
                         boneIndex = pair.second;
                         break;
                     }
                     
                     // 检查前一个字符是否为分隔符
                     char separator = name[name.size() - targetName.size() - 1];
                     if (separator == ':' || separator == '_' || separator == ' ') {
                         boneIndex = pair.second;
                         // qDebug() << "  -> Smart match resolved:" << collider.boneName.c_str() << "->" << name.c_str();
                         break;
                     }
                 }
             }
        }

        if (boneIndex == -1) {
             // 最后的尝试：如果依然没找到，打印失败信息（降低日志频次，仅在第一次未找到时打印）
             static std::set<std::string> missingBones;
             if (missingBones.find(collider.boneName) == missingBones.end()) {
                 // qDebug() << "  -> NOT FOUND in Skeleton (checked smart match):" << collider.boneName.c_str();
                 missingBones.insert(collider.boneName);
             }
             continue; 
        }

        if (boneIndex >= boneTransforms.size()) {
            // qDebug() << "  -> Index Out of Bounds!";
            continue; // 安全检查
        }

        // 计算骨骼原点（模型空间）
        QVector3D bonePosModel = boneTransforms[boneIndex] * QVector3D(0.0f, 0.0f, 0.0f);

        // 自动寻找一个直接子骨骼，用于估计骨段方向和长度（适配 humanoid 通用骨架）
        int childIndex = -1;
        for (int i = 0; i < static_cast<int>(skeleton.bones.size()); ++i) {
            if (skeleton.bones[i].parent == boneIndex) {
                childIndex = i;
                break;
            }
        }

        QVector3D childPosModel = bonePosModel;
        if (childIndex >= 0 && childIndex < static_cast<int>(boneTransforms.size())) {
            childPosModel = boneTransforms[childIndex] * QVector3D(0.0f, 0.0f, 0.0f);
        }

        QVector3D parentPosModel = bonePosModel;
        int parentIndex = skeleton.bones[boneIndex].parent;
        if (parentIndex >= 0 && parentIndex < static_cast<int>(boneTransforms.size())) {
            parentPosModel = boneTransforms[parentIndex] * QVector3D(0.0f, 0.0f, 0.0f);
        }

        // 先按配置偏移（骨骼局部方向）
        QVector3D sphereCenterModel = boneTransforms[boneIndex] * collider.offset;

        // 再做 humanoid 自适应修正，减少不同来源骨架的偏移误差
        float boneLenToChild = (childPosModel - bonePosModel).length();
        float boneLenToParent = (bonePosModel - parentPosModel).length();

        if (collider.tag == "Head") {
            // 头部常以颈部到头部方向作为“上方”估计，中心稍微抬高
            QVector3D upDir = bonePosModel - parentPosModel;
            if (upDir.lengthSquared() > 1e-6f) {
                upDir.normalize();
                sphereCenterModel = bonePosModel + upDir * (boneLenToParent > 1e-4f ? boneLenToParent * 0.35f : 0.12f);
            } else {
                sphereCenterModel = bonePosModel;
            }
        } else if (collider.tag == "Body") {
            // 躯干取骨骼到子骨骼方向的中上部，落点更接近胸腔
            if (boneLenToChild > 1e-4f) {
                sphereCenterModel = bonePosModel + (childPosModel - bonePosModel) * 0.35f;
            } else {
                sphereCenterModel = bonePosModel;
            }
        } else if (collider.tag == "LeftHand" || collider.tag == "RightHand") {
            // 手部通常沿掌骨方向略向前，减少点到手腕却触发不到的情况
            if (boneLenToChild > 1e-4f) {
                sphereCenterModel = bonePosModel + (childPosModel - bonePosModel) * 0.25f;
            } else {
                sphereCenterModel = bonePosModel;
            }
        } else {
            sphereCenterModel = bonePosModel;
        }

        // 半径保持配置值，仅做统一点击容差放大，避免骨段估计异常导致“全屏命中”
        float radius = collider.radius;
        float testRadius = radius * clickToleranceScale;

        // debug: 使用主中心计算点到射线距离，观察偏差趋势
        // QVector3D debugCenterWorld = currentModelMatrix * sphereCenterModel;
        // QVector3D pointToOrigin = debugCenterWorld - rayOrigin;
        // float perpendicularDist = pointToOrigin.crossProduct(pointToOrigin, rayDir).length();
        
        // 仅针对 Head 打印详细调试信息，或者总是显示
        // if (collider.boneName == "Head" || collider.boneName == "Spine") {
        //      qDebug() << "  [Detail] Bone:" << collider.boneName.c_str()
        //               << "WorldPos:" << debugCenterWorld
        //               << "Radius:" << radius
        //               << "TestRadius:" << testRadius
        //               << "DistToRay:" << perpendicularDist
        //               << "(Hit if <" << testRadius << ")";
        // }

        if (collider.tag == "Body") {
            // Body 使用胶囊体：优先 bone->child，无 child 时退化为 parent->bone
            QVector3D capA = bonePosModel;
            QVector3D capB = childPosModel;
            if (boneLenToChild <= 1e-4f && boneLenToParent > 1e-4f) {
                capA = parentPosModel;
                capB = bonePosModel;
            }

            QVector3D capAWorld = currentModelMatrix * capA;
            QVector3D capBWorld = currentModelMatrix * capB;

            float dist = 0.0f;
            if (intersectRayCapsule(rayOrigin, rayDir, capAWorld, capBWorld, testRadius, dist)) {
                qDebug() << "HIT!" << collider.tag.c_str() << "Dist:" << dist;
                if (dist < minDist) {
                    minDist = dist;
                    hitTag = collider.tag;
                }
            }
        } else {
            QVector3D sphereCenterWorld = currentModelMatrix * sphereCenterModel;
            float dist = 0.0f;
            if (intersectRaySphere(rayOrigin, rayDir, sphereCenterWorld, testRadius, dist)) {
                qDebug() << "HIT!" << collider.tag.c_str() << "Dist:" << dist;
                if (dist < minDist) {
                    minDist = dist;
                    hitTag = collider.tag;
                }
            }
        }
    }

    // qDebug() << "Final Hit:" << (hitTag.empty() ? "None" : hitTag.c_str());

    return hitTag;
}

bool RenderEngine::intersectRaySphere(const QVector3D &rayOrigin, const QVector3D &rayDir, const QVector3D &sphereCenter, float sphereRadius, float &outDist) {
    QVector3D m = rayOrigin - sphereCenter;
    float b = QVector3D::dotProduct(m, rayDir);
    float c = QVector3D::dotProduct(m, m) - sphereRadius * sphereRadius;
    // 射线起点在球外且指向远离球心的方向
    if (c > 0.0f && b > 0.0f) return false;

    float discr = b * b - c;
    if (discr < 0.0f) return false;

    // 计算最近交点
    float t = -b - sqrt(discr);
    if (t < 0.0f) t = 0.0f;

    outDist = t;
    return true;
}

bool RenderEngine::intersectRayCapsule(const QVector3D &rayOrigin, const QVector3D &rayDir,
                                       const QVector3D &capsuleA, const QVector3D &capsuleB,
                                       float capsuleRadius, float &outDist) {
    QVector3D ab = capsuleB - capsuleA;
    float abLenSq = QVector3D::dotProduct(ab, ab);

    // 退化为球体
    if (abLenSq < 1e-8f) {
        return intersectRaySphere(rayOrigin, rayDir, capsuleA, capsuleRadius, outDist);
    }

    QVector3D w0 = rayOrigin - capsuleA;
    float a = QVector3D::dotProduct(rayDir, rayDir); // 理论上为1
    float b = QVector3D::dotProduct(rayDir, ab);
    float c = abLenSq;
    float d = QVector3D::dotProduct(rayDir, w0);
    float e = QVector3D::dotProduct(ab, w0);

    float denom = a * c - b * b;
    float tRay = 0.0f;
    float tSeg = 0.0f;

    if (std::fabs(denom) > 1e-8f) {
        tRay = (b * e - c * d) / denom;
        tSeg = (a * e - b * d) / denom;
    } else {
        // 近平行：固定在射线起点投影
        tRay = 0.0f;
        tSeg = e / c;
    }

    // 约束到有效区间
    if (tRay < 0.0f) tRay = 0.0f;
    if (tSeg < 0.0f) tSeg = 0.0f;
    if (tSeg > 1.0f) tSeg = 1.0f;

    // 约束 segment 后重新计算 ray 参数
    tRay = -(d + b * tSeg) / a;
    if (tRay < 0.0f) {
        tRay = 0.0f;
        tSeg = e / c;
        if (tSeg < 0.0f) tSeg = 0.0f;
        if (tSeg > 1.0f) tSeg = 1.0f;
    }

    QVector3D closestRay = rayOrigin + rayDir * tRay;
    QVector3D closestSeg = capsuleA + ab * tSeg;
    QVector3D delta = closestRay - closestSeg;
    float distSq = QVector3D::dotProduct(delta, delta);

    if (distSq <= capsuleRadius * capsuleRadius) {
        outDist = tRay;
        return true;
    }

    return false;
}

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
