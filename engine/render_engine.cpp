//
// Created by Inoriac on 2025/10/15.
//

#include "render_engine.h"
#include <QOpenGLFunctions_3_3_Core>
#include <QElapsedTimer>
#include <qimage.h>
#include <QMatrix4x4>
#include <QVector3D>

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

void RenderEngine::initialize(QOpenGLFunctions_3_3_Core *glFuncs, ShaderManager *shaderMgr) {
    gl = glFuncs;
    shaderManager = shaderMgr;

    gl->glEnable(GL_DEPTH_TEST);    // 仅绘制离摄像机更近的片段
    gl->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);  // 设置多边形所有面均以实心模式绘制
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
    gl->glClearColor(0.2f, 0.6f, 0.9f, 1.0f);   // 背景色
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // 清除颜色与深度缓冲

    // 旋转角度（每秒旋转 60 度）(帧率无关)
    angleDeg += 60.0f * deltaSec;
    if (angleDeg >= 360.0f) angleDeg -= 360.0f;

    // 构建变换矩阵
    // 投影矩阵
    QMatrix4x4 proj;
    float aspect = (viewportHeight > 0) ? float(viewportWidth) / float(viewportHeight) : 1.0f;
    proj.perspective(45.0f, aspect, 0.1f, 100.0f);  // 近大远小

    // 视图矩阵(控制相机位置与方向)
    QMatrix4x4 view;
    view.lookAt(QVector3D(0, 1.5f, 4), QVector3D(0, 1, 0), QVector3D(0, 1, 0));
    // 模型矩阵
    QMatrix4x4 model;
    model.scale(0.05f);  // 缩小
    model.rotate(angleDeg, 0.0f, 1.0f, 0.0f); // 绕 Y 轴旋转

    // 综合矩阵 遵循右乘 结果等价于先把顶点从模型空间变换到世界坐标，再到视图，最后进行投影
    QMatrix4x4 mvp = proj * view * model;

    auto* shader = shaderManager->getShader("pbr");
    if (!shader) {
        qWarning() << "PBR shader not fount, falling back to default";
        return;
    }

    // 绘制
    shader->bind();
    shader->setUniformValue("model", model);
    shader->setUniformValue("view", view);
    shader->setUniformValue("projection", proj);

    // 设置光照
    shader->setUniformValue("lightPositions[0]", QVector3D(2.0f, 2.0f, 2.0f));
    shader->setUniformValue("lightColors[0]", QVector3D(1.0f, 1.0f, 1.0f));
    shader->setUniformValue("viewPos", QVector3D(0, 0, 3));

    // 绘制网格
    for (const auto &mesh: meshes) {

        if (mesh.materialIndex < 0 || mesh.materialIndex >= materials.size()) {
            qWarning() << "Mesh has invalid material index:" << mesh.materialIndex;
            continue;
        }

        // 取出对应材质
        const auto &mat = materials[mesh.materialIndex];
        gl->glBindVertexArray(mesh.vao);

        // 绑定不同的纹理槽位
        if (mat.albedoTexID) {
            gl->glActiveTexture(GL_TEXTURE0);
            gl->glBindTexture(GL_TEXTURE_2D, mat.albedoTexID);
        }
        if (mat.normalTexID) {
            gl->glActiveTexture(GL_TEXTURE1);
            gl->glBindTexture(GL_TEXTURE_2D, mat.normalTexID);
        }
        if (mat.metallicRoughnessTexID) {
            gl->glActiveTexture(GL_TEXTURE2);
            gl->glBindTexture(GL_TEXTURE_2D, mat.metallicRoughnessTexID);
        }

        gl->glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
        gl->glBindVertexArray(0);
    }

    // 清理状态
    gl->glBindVertexArray(0);
    shader->release();
}

void RenderEngine::addMesh(const std::vector<float> &interLeavePosColor,
                            const std::vector<unsigned int> &indices) {
    GpuMesh m;

    // 分配 GPU 资源区域 获取 GPU 对象 ID
    gl->glGenVertexArrays(1, &m.vao);
    gl->glGenBuffers(1, &m.vbo);
    gl->glGenBuffers(1, &m.ebo);

    // 激活顶点配置上下文，存储所有的配置信息
    gl->glBindVertexArray(m.vao);

    // 上传顶点数据
    gl->glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    gl->glBufferData(GL_ARRAY_BUFFER,
        GLsizeiptr(interLeavePosColor.size() * sizeof(float)),
        interLeavePosColor.data(),
        GL_STATIC_DRAW);

    // 上传索引数据 indices[i] 表示第 i 个顶点在 VBO 中的序号
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        GLsizeiptr(indices.size() * sizeof(unsigned int)),
        indices.data(),
        GL_STATIC_DRAW);

    // 配置顶点属性格式，也即读取方式(一次读多少个，从哪里开始读)，对应 VERT_SRC 的 aPos 与 aColor(索引对应)
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    gl->glEnableVertexAttribArray(1);
    gl->glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    gl->glEnableVertexAttribArray(2);

    gl->glBindVertexArray(0);

    m.indexCount = int(indices.size());
    meshes.push_back(m);
}

void RenderEngine::addMeshFromData(const MeshData &meshData) {
    GpuMesh m;

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
    //位置 (Location = 0)
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    gl->glEnableVertexAttribArray(0);

    // 法线(Location = 1)
    gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    gl->glEnableVertexAttribArray(1);

    // UV 坐标(Location = 2)
    gl->glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    gl->glEnableVertexAttribArray(2);

    gl->glBindVertexArray(0);

    m.indexCount = int(meshData.indices.size());
    m.materialIndex = meshData.materialIndex;

    meshes.push_back(m);

    qDebug() << "Mesh added to GPU with" << m.indexCount << " indices";
}

void RenderEngine::uploadMaterialTextures(MaterialData &material) {
    auto uploadFromQImage = [&](const QImage &img, unsigned int &texID) {
        if (img.isNull()) return;

        QImage tmp = img.convertToFormat(QImage::Format_RGBA8888).mirrored();

        gl->glGenTextures(1, &texID);
        gl->glBindTexture(GL_TEXTURE_2D, texID);

        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tmp.width(), tmp.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp.bits());
        gl->glGenerateMipmap(GL_TEXTURE_2D);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
    };

    auto loadAndUpload = [&](const std::string &path,
                             const std::vector<unsigned char> &imageData,
                             const std::string &mime,
                             unsigned int &texID) {
        // 优先使用内嵌字节流（imageData）
        if (!imageData.empty()) {
            QImage img = QImage::fromData(reinterpret_cast<const uchar*>(imageData.data()), static_cast<int>(imageData.size()));
            if (!img.isNull()) {
                uploadFromQImage(img, texID);
                return;
            } else {
                qWarning() << "QImage::fromData failed for embedded image (mime)" << QString::fromStdString(mime);
            }
        }

        // 如果没有内嵌数据，再尝试通过路径加载（外部 .png/.jpg）
        if (!path.empty()) {
            // 注意：path 可能是相对路径，需与 GLTF 文件所在目录拼接 —— 这里假设 path 可直接访问
            QImage img(QString::fromStdString(path));
            if (!img.isNull()) {
                uploadFromQImage(img, texID);
                return;
            } else {
                qWarning() << "Failed to load texture from path:" << QString::fromStdString(path);
            }
        }

        // 均失败则 texID 保持 0（表示无纹理）
    };

    // 调用：albedo / normal / mr / ao / emissive
    loadAndUpload(material.albedoTexPath, material.albedoImageData, material.albedoMimeType, material.albedoTexID);
    loadAndUpload(material.normalTexPath, material.normalImageData, material.normalMimeType, material.normalTexID);
    loadAndUpload(material.metallicRoughnessTexPath, material.metallicRoughnessImageData, material.metallicRoughnessMimeType, material.metallicRoughnessTexID);
    loadAndUpload(material.aoTexPath, material.aoImageData, material.aoMimeType, material.aoTexID);
    // emissive 可以类似处理，如果需要
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
