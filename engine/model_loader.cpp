//
// Created by Inoriac on 2025/10/15.
//

#include "model_loader.h"
#include "tiny_gltf.h"

#include <QDebug>
#include <QFileInfo>

ModelLoader::ModelLoader() = default;

ModelLoader::~ModelLoader() {
    clear();
}

bool ModelLoader::loadModel(const std::string &path) {
    clear();

    // 检查文件是否存在
    QFileInfo fileInfo(QString::fromStdString(path));
    if (!fileInfo.exists()) {
        qWarning() << "Model file not exist:" << path.c_str();
        return false;
    }

    modelDirectory = fileInfo.path().toStdString();
    // 裁剪资源路径至 ./assets/models/{model_name}
    size_t pos = modelDirectory.rfind("/model");
    if (pos != std::string::npos) {
        modelDirectory = modelDirectory.substr(0, pos);
    }


    return parseGLTF(path);
}

void ModelLoader::clear() {
    meshes.clear();
}

bool ModelLoader::parseGLTF(const std::string &path) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    // 兼容 .glb 与 .gltf
    bool ret = false;
    if (path.find(".glb") != std::string::npos) {
        ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);
    } else {
        ret = loader.LoadASCIIFromFile(&model, &err, &warn, path);
    }

    if (!warn.empty()) {
        qWarning() << "GLTF warning!: " << warn.c_str();
    }

    if (!err.empty()) {
        qWarning() << "GLTF error!: " << err.c_str();
    }

    if (!ret) {
        qWarning() << "Failed to load model from file:" << path.c_str();
        return false;
    }

    // 解析材质
    materials.clear();
    for (const auto &mat : model.materials) {
        materials.push_back(extractMaterialData(mat, model));
    }
    qDebug() << "Extraced" << materials.size() << "materials";

    // 提取网格数据
    for (const auto&mesh : model.meshes) {
        extractMeshData(model, mesh);
    }

    qDebug() << "Extracted" << meshes.size() << "meshes";
    return true;
}

void ModelLoader::extractMeshData(const tinygltf::Model &model, const tinygltf::Mesh &mesh) {
    for (const auto& primitive : mesh.primitives) {
        MeshData meshData;
        meshData.materialName = mesh.name;
        meshData.materialIndex = -1;

        // 提取顶点数据
        extractVertexData(model, primitive, meshData);

        // 提取索引数据
        extractIndexData(model, primitive, meshData);

        // 提取材质索引
        if (primitive.material >= 0) {
            meshData.materialIndex = primitive.material;
        }

        // 存入当前 meshData
        meshes.push_back(meshData);
    }
}

void ModelLoader::extractVertexData(const tinygltf::Model &model, const tinygltf::Primitive &primitive,
    MeshData &meshData) {
    // 提取位置信息
    if (primitive.attributes.find("POSITION") != primitive.attributes.end()) {
        const auto& accessor = model.accessors[primitive.attributes.at("POSITION")];    // 数据的结构
        const auto& bufferView = model.bufferViews[accessor.bufferView];                     // 中转站，获取目标数据的起始偏移量
        const auto& buffer = model.buffers[bufferView.buffer];                               // 原始字节数据区

        // 获取每个顶点的字节跨度
        size_t stride = accessor.ByteStride(bufferView);
        if (stride == 0) stride = sizeof(float) * 3;

        // 依据偏移量，获取 buffer 的起始指针
        const unsigned char *dataPtr = &buffer.data[bufferView.byteOffset + accessor.byteOffset];

        for (size_t i = 0; i < accessor.count; i++) {
            const float *pos = reinterpret_cast<const float *>(dataPtr + i * stride);
            meshData.vertices.insert(meshData.vertices.end(), {pos[0], pos[1], pos[2]});
        }

    }

    // 提取法线数据
    if (primitive.attributes.find("NORMAL") != primitive.attributes.end()) {
        const auto& accessor = model.accessors[primitive.attributes.at("NORMAL")];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];

        size_t stride = accessor.ByteStride(bufferView);
        if (stride == 0) stride = sizeof(float) * 3;

        const unsigned char *dataPtr = &buffer.data[bufferView.byteOffset + accessor.byteOffset];

        for (size_t i = 0; i < accessor.count; i++) {
            const float *n = reinterpret_cast<const float *>(dataPtr + i * stride);
            meshData.normals.insert(meshData.normals.end(), {n[0], n[1], n[2]});
        }
    }

    // 提取UV数据
    if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end()) {
        const auto& accessor = model.accessors[primitive.attributes.at("TEXCOORD_0")];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];

        size_t stride = accessor.ByteStride(bufferView);
        if (stride == 0) stride = sizeof(float) * 2;

        const unsigned char *dataPtr = &buffer.data[bufferView.byteOffset + accessor.byteOffset];

        for (size_t i = 0; i < accessor.count; i++) {
            const float *uv = reinterpret_cast<const float *>(dataPtr + i * stride);
            meshData.uvs.insert(meshData.uvs.end(), {uv[0], uv[1]});
        }
    }
}

void ModelLoader::extractIndexData(const tinygltf::Model &model, const tinygltf::Primitive &primitive,
    MeshData &meshData) {

    if (primitive.indices < 0) return;

    const auto& accessor = model.accessors[primitive.indices];
    const auto& bufferView = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers[bufferView.buffer];

    const unsigned char *dataPtr = &buffer.data[bufferView.byteOffset + accessor.byteOffset];

    // 根据索引类型读取
    switch (accessor.componentType) {
        case TINYGLTF_COMPONENT_TYPE_BYTE: {
            const uint8_t *indices = reinterpret_cast<const uint8_t *>(dataPtr);
            meshData.indices.assign(indices, indices + accessor.count);
            break;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            const uint16_t *indices = reinterpret_cast<const uint16_t *>(dataPtr);
            meshData.indices.assign(indices, indices + accessor.count);
            break;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
            const uint32_t *indices = reinterpret_cast<const uint32_t *>(dataPtr);
            meshData.indices.assign(indices, indices + accessor.count);
            break;
        }
        default:
            qWarning() << "Unsupported component type:" << accessor.componentType;
            break;
    }
}

MaterialData ModelLoader::extractMaterialData(const tinygltf::Material& mat, const tinygltf::Model& model) {
    MaterialData material;
    material.name = mat.name;

    // 基础色因子
    if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
        material.baseColorFactor = QVector3D(
            mat.pbrMetallicRoughness.baseColorFactor[0],
            mat.pbrMetallicRoughness.baseColorFactor[1],
            mat.pbrMetallicRoughness.baseColorFactor[2]
        );
    }
    material.metallicFactor  = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
    material.roughnessFactor = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);

    // Lambda: 将纹理索引转换为路径或内嵌数据
    auto fillImage = [&](int texIndex,
                         std::string &outPath,
                         std::vector<unsigned char> &outImageData,
                         std::string &outMime) {

        outPath.clear();
        outImageData.clear();
        outMime.clear();

        if (texIndex < 0 || texIndex >= static_cast<int>(model.textures.size())) return;

        const tinygltf::Texture &tex = model.textures[texIndex];
        int imgIndex = tex.source;
        if (imgIndex < 0 || imgIndex >= static_cast<int>(model.images.size())) return;

        const tinygltf::Image &img = model.images[imgIndex];

        // A. 外部 URI 图片
        if (!img.uri.empty()) {
            // 拼接成绝对路径
            outPath = modelDirectory + "/textures/" + img.uri;
            qDebug() << "Resolved external image path:" << QString::fromStdString(outPath);
            return;
        }

        // B. 内嵌图像（.glb）
        if (!img.image.empty()) {
            outImageData = img.image;
            outMime = img.mimeType;
            qDebug() << "Found embedded image (size):"
                     << outImageData.size()
                     << " mime:" << QString::fromStdString(outMime);
            return;
        }

        // C. bufferView 解包（备用）
        if (img.bufferView >= 0 && img.bufferView < static_cast<int>(model.bufferViews.size())) {
            const auto &bufferView = model.bufferViews[img.bufferView];
            const auto &buffer = model.buffers[bufferView.buffer];

            size_t offset = bufferView.byteOffset;
            size_t length = bufferView.byteLength;
            if (offset + length <= buffer.data.size()) {
                outImageData.insert(outImageData.end(),
                                    buffer.data.begin() + offset,
                                    buffer.data.begin() + offset + length);
                outMime = img.mimeType;
                qDebug() << "Recovered embedded image via bufferView (offset:" << offset
                         << ", len:" << length << ") mime:" << QString::fromStdString(outMime);
            } else {
                qWarning() << "Invalid bufferView range for image" << imgIndex;
            }
            return;
        }

        qWarning() << "Image" << imgIndex << "has no valid data source.";
    };

    // BaseColor
    fillImage(mat.pbrMetallicRoughness.baseColorTexture.index,
              material.albedoTexPath,
              material.albedoImageData,
              material.albedoMimeType);

    // MetallicRoughness
    fillImage(mat.pbrMetallicRoughness.metallicRoughnessTexture.index,
              material.metallicRoughnessTexPath,
              material.metallicRoughnessImageData,
              material.metallicRoughnessMimeType);

    // Normal
    fillImage(mat.normalTexture.index,
              material.normalTexPath,
              material.normalImageData,
              material.normalMimeType);

    // AO
    fillImage(mat.occlusionTexture.index,
              material.aoTexPath,
              material.aoImageData,
              material.aoMimeType);

    return material;
}

