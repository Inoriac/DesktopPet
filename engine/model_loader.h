//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_MODEL_LOADER_H
#define DESKTOP_PET_MODEL_LOADER_H

#include <QVector3D>
#include <string>
#include <vector>
#include "tiny_gltf.h"

// 用于 CPU 端保存 glTF 中的材质数据
struct MaterialData {
    std::string name;

    QVector3D baseColorFactor {1.0f, 1.0f, 1.0f};
    float metallicFactor {0.0f};
    float roughnessFactor {1.0f};

    // 如果模型使用外部文件，这里保存路径
    std::string albedoTexPath;
    std::string normalTexPath;
    std::string metallicRoughnessTexPath;
    std::string aoTexPath;
    std::string emissiveTexPath;

    // 内嵌二进制数据
    std::vector<unsigned char> albedoImageData;
    std::string albedoMimeType;

    std::vector<unsigned char> normalImageData;
    std::string normalMimeType;

    std::vector<unsigned char> metallicRoughnessImageData;
    std::string metallicRoughnessMimeType;

    std::vector<unsigned char> aoImageData;
    std::string aoMimeType;

    // GPU 纹理 ID
    unsigned int albedoTexID {0};
    unsigned int normalTexID {0};
    unsigned int metallicRoughnessTexID {0};
    unsigned int aoTexID {0};
    unsigned int emissiveTexID {0};
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
    std::vector<MaterialData>& getMaterials() { return materials; };
    void clear();

private:
    std::string modelDirectory;
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;    // 存储所有材质

    // 辅助方法
    bool parseGLTF(const std::string& path);    // 加载模型
    void extractMeshData(const tinygltf::Model& model, const tinygltf::Mesh& mesh); // 获取 MeshData

    void extractVertexData(const tinygltf::Model& model, const tinygltf::Primitive& primitive, MeshData& meshData); // 提取顶点数据
    void extractIndexData(const tinygltf::Model& model, const tinygltf::Primitive& primitive, MeshData& meshData);  // 提取索引数据
    MaterialData extractMaterialData(const tinygltf::Material& mat, const tinygltf::Model& model);     // 提取材质信息
};

#endif //DESKTOP_PET_MODEL_LOADER_H