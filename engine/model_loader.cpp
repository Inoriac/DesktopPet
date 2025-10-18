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

    return parseGLTF(path);
}

void ModelLoader::clear() {
    meshes.clear();
}

bool ModelLoader::parseGLTF(const std::string &path) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    // 加载 GLB 文件
    bool ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);

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

    qDebug() << "GLB file loaded successfully:" << path.c_str();
    qDebug() << "Meshed count:" << model.meshes.size();
    qDebug() << "Material count:" << model.materials.size();

    // 提取所有的网格数据
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
        const auto& accessor = model.accessors[primitive.attributes.at("POSITION")];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];

        const float* positions = reinterpret_cast<const float*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);

        for (size_t i = 0; i< accessor.count; i++) {
            meshData.vertices.push_back(positions[i * 3 + 0]);  // x
            meshData.vertices.push_back(positions[i * 3 + 1]);  // y
            meshData.vertices.push_back(positions[i * 3 + 2]);  // z
        }
    }

    // 提取法线数据
    if (primitive.attributes.find("NORMAL") != primitive.attributes.end()) {
        const auto& accessor = model.accessors[primitive.attributes.at("NORMAL")];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];

        const float* normals = reinterpret_cast<const float*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);

        for (size_t i = 0; i < accessor.count; ++i) {
            meshData.normals.push_back(normals[i * 3 + 0]); // nx
            meshData.normals.push_back(normals[i * 3 + 1]); // ny
            meshData.normals.push_back(normals[i * 3 + 2]); // nz
        }
    }

    // 提取UV数据
    if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end()) {
        const auto& accessor = model.accessors[primitive.attributes.at("TEXCOORD_0")];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];

        const float* uvs = reinterpret_cast<const float*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);

        for (size_t i = 0; i < accessor.count; ++i) {
            meshData.uvs.push_back(uvs[i * 2 + 0]); // u
            meshData.uvs.push_back(uvs[i * 2 + 1]); // v
        }
    }
}

void ModelLoader::extractIndexData(const tinygltf::Model &model, const tinygltf::Primitive &primitive,
    MeshData &meshData) {
    if (primitive.indices >= 0) {
        const auto& accessor = model.accessors[primitive.indices];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];

        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
            const unsigned int* indices = reinterpret_cast<const unsigned int*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);
            meshData.indices.assign(indices, indices + accessor.count);
        } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            const unsigned short* indices = reinterpret_cast<const unsigned short*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);
            meshData.indices.assign(indices, indices + accessor.count);
        }
    }
}
