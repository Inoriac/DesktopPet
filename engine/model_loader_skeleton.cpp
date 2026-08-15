//
// Created by Inoriac on 2025/10/15.
//

#include "model_loader.h"

#include <QDebug>

#include <cstdint>

void ModelLoader:: extractNodes(const tinygltf::Model& model, int nodeIndex, int parentIndex) {
    // 获取真实的 node 节点结构
    const tinygltf::Node& gltfNode = model.nodes[nodeIndex];

    nodes.resize(model.nodes.size());
    Node& node = nodes[nodeIndex];

    node.index = nodeIndex;
    node.name = gltfNode.name;
    node.parent = parentIndex;

    node.localTransform = getNodeLocalTransform(gltfNode);

    // 对当前节点的子节点使用递归构建，遇到叶子节点时，停止
    for (int childIndex : gltfNode.children) {
        node.children.push_back(childIndex);
        extractNodes(model, childIndex, nodeIndex);
    }
}

void ModelLoader::extractSkeleton(const tinygltf::Model& model) {
    // 清空数据
    skins.clear();
    skeleton.bones.clear();
    skeleton.nameToIndex.clear();

    // 没有 skin
    if (model.skins.empty()) {
        qDebug() << "[ModelLoader] No skins found in model";
        return;
    }

    // 目前只使用第一个 skin
    const tinygltf::Skin& gltfSkin = model.skins[0];
    skeleton.skinIndex = 0;

    // 构建 Skin 结构
    Skin skin;
    skin.skeletonRoot = gltfSkin.skeleton;
    skin.joints = gltfSkin.joints;

    // IBM
    if (gltfSkin.inverseBindMatrices >= 0) {
        const tinygltf::Accessor& accessor = model.accessors[gltfSkin.inverseBindMatrices];
        const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

        // 获取矩阵数据起始地址
        const unsigned char* matrixData = &buffer.data[bufferView.byteOffset + accessor.byteOffset];
        const size_t matrixStride = static_cast<size_t>(accessor.ByteStride(bufferView));

        skin.inverseBindMatrices.resize(accessor.count);

        for (size_t i = 0; i < accessor.count; i++) {
            const float* matrix = reinterpret_cast<const float*>(matrixData + i * matrixStride);
            QMatrix4x4 m;
            m.setToIdentity();
            for (int col = 0; col < 4; col++) {
                for (int row = 0; row < 4; row++) {
                    m(row, col) = matrix[col * 4 + row];
                }
            }
            skin.inverseBindMatrices[i] = m;
        }
    }

    skins.push_back(skin);

    // 创建 skeleton.bones
    skeleton.bones.resize(skin.joints.size());

    for (size_t i = 0; i < skin.joints.size(); i++) {
        int nodeIndex = skin.joints[i];
        const Node& node = nodes[nodeIndex];

        Bone& bone = skeleton.bones[i];
        bone.name = node.name;
        bone.index = nodeIndex; // 记录骨骼 index
        bone.parent = -1;   // 暂定为-1，交由 extractSkeletonHierarchy 修复层级关系
        bone.localTransform.setToIdentity();    // 骨骼的 bind 姿势从 IBM 中得到
        bone.globalTransform.setToIdentity();

        // inverse bind matrix
        if (i < skin.inverseBindMatrices.size()) {
            bone.inverseBindMatrix = skin.inverseBindMatrices[i];
        }

        skeleton.nameToIndex[node.name] = static_cast<int>(i);
    }

    qDebug() << "[ModelLoader] Skeleton extracted. Bone count = " << skeleton.bones.size();
}

void ModelLoader::extractSkeletonHierarchy(const tinygltf::Model& model) {
    if (skeleton.bones.empty()) return;

    // 建立 Bone 层级
    for (Bone& bone : skeleton.bones) {
        int nodeIndex = bone.index;
        const Node& node = nodes[nodeIndex];

        int parentNode = node.parent;

        if(parentNode >= 0){
            auto it = skeleton.nameToIndex.find(nodes[parentNode].name);
            if (it != skeleton.nameToIndex.end()) {
                bone.parent = it->second;
            }
        }
    }

    // 使用 IBM 计算 bind pose
    for (Bone& bone : skeleton.bones) {
        bone.globalTransform = bone.inverseBindMatrix.inverted();
    }

    qDebug() << "[Skeleton] Hierarchy built, global transfroms updated.";
}

void ModelLoader::extractSkinningData(const tinygltf::Model& model, const tinygltf::Primitive& primitive, MeshData& meshData) {
    // 没有蒙皮数据
    if (primitive.attributes.find("JOINTS_0") == primitive.attributes.end() ||
        primitive.attributes.find("WEIGHTS_0") == primitive.attributes.end()) {
        return;
    }

    meshData.hasSkinning = true;

    // JOINTS_0
    const tinygltf::Accessor& jointsAccessor = model.accessors[ primitive.attributes.at("JOINTS_0")];
    const tinygltf::BufferView& jointsBufferView = model.bufferViews[ jointsAccessor.bufferView ];
    const tinygltf::Buffer& jointsBuffer = model.buffers[ jointsBufferView.buffer ];

    // 获取实际数据地址
    const unsigned char* jointsDataBase = &jointsBuffer.data[ jointsBufferView.byteOffset + jointsAccessor.byteOffset ];

    // 步长
    const int jointsStride = jointsAccessor.ByteStride(jointsBufferView);

    // WEIGHTS_0
    const tinygltf::Accessor& weightsAccessor = model.accessors[ primitive.attributes.at("WEIGHTS_0") ];
    const tinygltf::BufferView& weightsBufferView = model.bufferViews[weightsAccessor.bufferView];
    const tinygltf::Buffer& weightsBuffer = model.buffers[weightsBufferView.buffer];

    const unsigned char* weightsDataBase = &weightsBuffer.data[weightsBufferView.byteOffset + weightsAccessor.byteOffset];

    // 步长
    const int weightsStride = weightsAccessor.ByteStride(weightsBufferView);

    // 顶点数量
    size_t vertexCount = jointsAccessor.count;

    // 预分配空间(4 值/顶点)
    meshData.boneIndices.resize(vertexCount * 4);
    meshData.boneWeights.resize(vertexCount * 4);

    // 当前使用的 skin(一般为 0)
    const Skin& skin = skins[skeleton.skinIndex];

    // 遍历每个顶点
    for (size_t v = 0; v < vertexCount; ++v) {
        // 处理 JOINTS
        const unsigned char* jointPtr = jointsDataBase + v * jointsStride;

        uint16_t j0 = 0, j1 = 0, j2 = 0, j3 = 0;

        if (jointsAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            const uint16_t* j = reinterpret_cast<const uint16_t*>(jointPtr);
            j0 = j[0]; j1 = j[1]; j2 = j[2]; j3 = j[3];
        }
        else if (jointsAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            const uint8_t* j = reinterpret_cast<const uint8_t*>(jointPtr);
            j0 = j[0]; j1 = j[1]; j2 = j[2]; j3 = j[3];
        }
        else {
            qWarning() << "Unsupported JOINTS_0 component type.";
            continue;
        }

        // glTF JOINTS_0 存储的是 skin.joints 数组的索引，而我们的 skeleton.bones 正是按照 skin.joints 顺序构建的
        // 所以 j0, j1, j2, j3 直接对应 skeleton bone index
        // 无需经过 mapNodeToBone (那是用于从节点名查找骨骼的)

        meshData.boneIndices[v * 4 + 0] = j0;
        meshData.boneIndices[v * 4 + 1] = j1;
        meshData.boneIndices[v * 4 + 2] = j2;
        meshData.boneIndices[v * 4 + 3] = j3;

/*
        // 旧的错误逻辑：误以为 j0 是 nodeIndex
        // 将 gltf 的 joints 格式从node index 转为 skeleton bone index
        auto mapNodeToBone = [&](int nodeIndex) -> int {
            const std::string& name = nodes[nodeIndex].name;
            auto it = skeleton.nameToIndex.find(name);
            if (it != skeleton.nameToIndex.end()) {
                return it->second;
            }
            return -1;
        };

        meshData.boneIndices[v * 4 + 0] = mapNodeToBone(j0);
        meshData.boneIndices[v * 4 + 1] = mapNodeToBone(j1);
        meshData.boneIndices[v * 4 + 2] = mapNodeToBone(j2);
        meshData.boneIndices[v * 4 + 3] = mapNodeToBone(j3);
*/

        // WEIGHTS
        const unsigned char* weightPtr = weightsDataBase + v * weightsStride;

        float w0, w1, w2, w3;

        if (weightsAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
            const float* w = reinterpret_cast<const float*>(weightPtr);
            w0 = w[0]; w1 = w[1]; w2 = w[2]; w3 = w[3];
        } else if (weightsAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            const uint16_t* w = reinterpret_cast<const uint16_t*>(weightPtr);
            w0 = w[0] / 65535.0f;
            w1 = w[1] / 65535.0f;
            w2 = w[2] / 65535.0f;
            w3 = w[3] / 65535.0f;
        } else if (weightsAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            const uint8_t* w = reinterpret_cast<const uint8_t*>(weightPtr);
            w0 = w[0] / 255.0f;
            w1 = w[1] / 255.0f;
            w2 = w[2] / 255.0f;
            w3 = w[3] / 255.0f;
        } else {
            qWarning() << "Unsupported WEIGHTs_0 component type.";
            continue;
        }

        // 权重归一化
        float sum = w0 + w1 + w2 + w3;
        if (sum > 0.0f) {
            w0 /= sum;
            w1 /= sum;
            w2 /= sum;
            w3 /= sum;
        }

        meshData.boneWeights[v * 4 + 0] = w0;
        meshData.boneWeights[v * 4 + 1] = w1;
        meshData.boneWeights[v * 4 + 2] = w2;
        meshData.boneWeights[v * 4 + 3] = w3;
    }

    qDebug() << "[ModelLoader] Skinning data extracted. Vertices = " << vertexCount;
}

