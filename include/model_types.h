#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <QMatrix4x4>
#include <QVector3D>

#include "material_types.h"

// 通用 gltf 节点
struct Node {
    std::string name;
    int index;                    // glTF node index
    int parent = -1;
    std::vector<int> children;

    QMatrix4x4 localTransform;    // TRS → matrix
    QMatrix4x4 globalTransform;   // runtime calculated
};

struct Bone {
    std::string name;
    int index;              // 对应 gltf 节点索引
    int parent = -1;        // 父节点索引 -1表示 root
    QMatrix4x4 localTransform;  // 本地变换矩阵
    QMatrix4x4 globalTransform; // 初始全局变换
    QMatrix4x4 inverseBindMatrix;   // gltf 提供的 inverse bind matrix
};

struct Skeleton {
    int skinIndex;
    std::vector<Bone> bones;
    std::unordered_map<std::string, int> nameToIndex;   // 骨骼索引
};

struct Skin {
    int skeletonRoot = -1;

    std::vector<int> joints;
    std::vector<QMatrix4x4> inverseBindMatrices;    // gltf IBM

    // 运行时数据
    std::vector<QMatrix4x4> skinMatrices;   // 最终骨骼矩阵
};

// CPU 端数据结构，用于存储从GLB文件解析出来的原始数据
struct MeshData {
    std::vector<float> vertices;    // 顶点数据
    std::vector<unsigned int> indices;  // 索引数据
    std::vector<float> normals;     // 法线数据
    std::vector<float> uvs;         //  uv 坐标

    // 蒙皮数据
    std::vector<float> boneWeights;
    std::vector<int> boneIndices;

    std::string materialName;      // 材质名称
    int materialIndex;                   // 材质索引

    bool hasSkinning = false;
};
