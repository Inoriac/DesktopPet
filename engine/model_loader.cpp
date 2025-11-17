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


    return parseGLTF(path);
}

void ModelLoader::clear() {
    materials.clear();
    meshes.clear();
    skins.clear();
    skeleton.bones.clear();
    skeleton.nameToIndex.clear();
}

bool ModelLoader::parseGLTF(const std::string &path) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    loader.SetImageLoader(
        [](tinygltf::Image *image, const int image_idx, std::string *err,
           std::string *warn, int req_width, int req_height,
           const unsigned char *bytes, int size, void *user_data) -> bool {
            // 什么都不做，只保留 URI
            // tinygltf 会自动保留 image.uri 字段
            return true;
        },
        nullptr
    );

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

    // 解析 Node
    const tinygltf::Scene& scene = model.scenes[0];
    for(int rootNode : scene.nodes){
        extractNodes(model, rootNode, -1);
    }

    // 解析骨骼
    extractSkeleton(model);
    qDebug() << "Extract skeleton success";

    // 修复骨骼层级
    extractSkeletonHierarchy(model);

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
            mat.pbrMetallicRoughness.baseColorFactor[2]);
        material.alphaFactor = static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[3]);
    }

    material.metallicFactor = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
    material.roughnessFactor = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);

    // 加载贴图
    auto loadTexture = [&](int texIndex,
                           std::string &outPath,
                           std::vector<unsigned char> &outImageData,
                           int &outWidth,
                           int &outHeight) {
        if (texIndex < 0 || texIndex >= model.textures.size())
            return;

        const tinygltf::Texture &tex = model.textures[texIndex];
        const tinygltf::Image &img = model.images[tex.source];

        if (!img.uri.empty()) {
            // 外部图片文件路径
            outPath = modelDirectory + "/" + img.uri;
        } else if (!img.image.empty()) {
            // 内嵌图像（glb bufferView 或 base64）
            outImageData = img.image;
            outWidth = img.width;
            outHeight = img.height;
        }
    };

    // === 基础色贴图 ===
    loadTexture(mat.pbrMetallicRoughness.baseColorTexture.index,
                material.albedoTexPath,
                material.albedoImageData,
                material.albedoWidth,
                material.albedoHeight);

    // === 金属度/粗糙度贴图 ===
    loadTexture(mat.pbrMetallicRoughness.metallicRoughnessTexture.index,
                material.metallicRoughnessTexPath,
                material.metallicRoughnessImageData,
                material.metallicRoughnessWidth,
                material.metallicRoughnessHeight);

    // === 法线贴图 ===
    loadTexture(mat.normalTexture.index,
                material.normalTexPath,
                material.normalImageData,
                material.normalWidth,
                material.normalHeight);

    // === 环境光 AO 贴图 ===
    loadTexture(mat.occlusionTexture.index,
                material.aoTexPath,
                material.aoImageData,
                material.aoWidth,
                material.aoHeight);

    return material;
}

void ModelLoader:: extractNodes(const tinygltf::Model& model, int nodeIndex, int parentIndex) {
    // 获取真实的 node 节点结构
    const tinygltf::Node& gltfNode = model.nodes[nodeIndex];

    nodes.resize(model.nodes.size());

    Node& node = nodes[nodeIndex];
    node.index = nodeIndex;
    node.name = gltfNode.name;
    node.parent = parentIndex;

    node.localTransform = getNodeLocalTransform(gltfNode);

    if (parentIndex < 0) {
        // 父节点
        node.globalTransform = node.localTransform;
    } else {
        // 其他节点则向上寻找
        node.globalTransform = nodes[parentIndex].globalTransform * node.localTransform;
    }

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
        const float* matrixData = reinterpret_cast<const float*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);

        skin.inverseBindMatrices.resize(accessor.count);

        for (size_t i = 0; i < accessor.count; i++) {
            QMatrix4x4 m(
                matrixData[i*16 + 0], matrixData[i*16 + 1], matrixData[i*16 + 2], matrixData[i*16 + 3],
                matrixData[i*16 + 4], matrixData[i*16 + 5], matrixData[i*16 + 6], matrixData[i*16 + 7],
                matrixData[i*16 + 8], matrixData[i*16 + 9], matrixData[i*16 + 10], matrixData[i*16 + 11],
                matrixData[i*16 + 12], matrixData[i*16 + 13], matrixData[i*16 + 14], matrixData[i*16 + 15]
            );
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
        bone.localTransform = node.localTransform;
        bone.globalTransform = node.globalTransform;

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

    // 递归计算 Bone.globalTransform
    std::function<void(int)> computeGlobal = [&](int boneIndex) {
        Bone& bone = skeleton.bones[boneIndex];

        if(bone.parent == -1) {
            // 根节点
            bone.globalTransform = bone.localTransform;
        } else {
            // 子节点
            Bone& parent = skeleton.bones[bone.parent];
            bone.globalTransform = parent.globalTransform * bone.localTransform;
        }

        // 递归处理
        for(size_t i = 0; i < skeleton.bones.size(); ++i) {
            if (skeleton.bones[i].parent == boneIndex) {
                computeGlobal(static_cast<int>(i));
            }
        }
    };


    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
        // 找到根节点
        if(skeleton.bones[i].parent == -1) {
            computeGlobal(static_cast<int>(i));
        }
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
    int jointsStride = tinygltf::GetComponentSizeInBytes(jointsAccessor.componentType) * 4;

    // WEIGHTS_0
    const tinygltf::Accessor& weightsAccessor = model.accessors[ primitive.attributes.at("WEIGHTS_0") ];
    const tinygltf::BufferView& weightsBufferView = model.bufferViews[weightsAccessor.bufferView];
    const tinygltf::Buffer& weightsBuffer = model.buffers[weightsBufferView.buffer];

    const unsigned char* weightsDataBase = &weightsBuffer.data[weightsBufferView.byteOffset + weightsAccessor.byteOffset];

    // 步长
    int weightsStride = tinygltf::GetComponentSizeInBytes(weightsAccessor.componentType) * 4;

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