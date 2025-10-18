//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_MODEL_LOADER_H
#define DESKTOP_PET_MODEL_LOADER_H

#include <QVector3D>
#include <string>
#include <vector>
#include "tiny_gltf.h"

// 材质信息
struct MaterialData {
    std::string name;
    QVector3D baseColor;
    float metallic;
    float roughness;
    float ao;
    std::string albedoTexture;
    std::string normalTexture;
    std::string metallicTexture;
    std::string roughnessTexture;
    std::string aoTexture;
};

// CPU 端数据结构，用于存储从GLB文件解析出来的原始数据
struct MeshData {
    std::vector<float> vertices;    // 顶点数据
    std::vector<unsigned int> indices;  // 索引数据
    std::vector<float> normals;     // 法线数据
    std::vector<float> uvs;         //  uv 坐标
    std::string materialName;      // 材质名称
    int materialIndex;                   // 材质索引
};

class ModelLoader {
public:
    ModelLoader();
    ~ModelLoader();

    bool loadModel(const std::string& path);
    const std::vector<MeshData>& getMeshes() const { return meshes; };
    void clear();

private:
    std::vector<MeshData> meshes;

    // 辅助方法
    bool parseGLTF(const std::string& path);    // 加载模型
    void extractMeshData(const tinygltf::Model& model, const tinygltf::Mesh& mesh); // 获取 MeshData
    void extractVertexData(const tinygltf::Model& model, const tinygltf::Primitive& primitive, MeshData& meshData); // 提取顶点数据
    void extractIndexData(const tinygltf::Model& model, const tinygltf::Primitive& primitive, MeshData& meshData);  // 提取索引数据
};

#endif //DESKTOP_PET_MODEL_LOADER_H