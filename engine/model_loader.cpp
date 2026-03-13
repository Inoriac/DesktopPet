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

Skeleton ModelLoader::releaseSkeleton() {
    return std::move(skeleton);
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

    // qDebug() << "---------------------VALIDATION---------------------";
    // runAllModelTests(model);

    calculateBoundingBox(model);

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

        extractSkinningData(model, primitive, meshData);

        // 存入当前 meshData
        meshes.push_back(meshData);

        for (auto& attr : primitive.attributes) {
            qDebug() << "Attribute:" << QString::fromStdString(attr.first);
        }
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
            QMatrix4x4 m;
            m.setToIdentity();
            for (int col = 0; col < 4; col++) {
                for (int row = 0; row < 4; row++) {
                    m(row, col) = matrixData[i * 16 + col * 4 + row];
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

void ModelLoader::calculateBoundingBox(const tinygltf::Model &model) {
    QVector3D minBox(1e9, 1e9, 1e9);
    QVector3D maxBox(-1e9, -1e9, -1e9);
    bool valid = false;

    // 遍历所有 mesh 的 POSITION 属性，更新包围盒
    for (const auto& mesh : model.meshes) {
        for (const auto& primitive : mesh.primitives) {
            auto it = primitive.attributes.find("POSITION");
            if (it != primitive.attributes.end()) {
                const auto& accessor = model.accessors[it->second];

                if (accessor.minValues.size() == 3 && accessor.maxValues.size() == 3) {
                    minBox.setX(std::min(minBox.x(), (float)accessor.minValues[0]));
                    minBox.setY(std::min(minBox.y(), (float)accessor.minValues[1]));
                    minBox.setZ(std::min(minBox.z(), (float)accessor.minValues[2]));

                    maxBox.setX(std::max(maxBox.x(), (float)accessor.maxValues[0]));
                    maxBox.setY(std::max(maxBox.y(), (float)accessor.maxValues[1]));
                    maxBox.setZ(std::max(maxBox.z(), (float)accessor.maxValues[2]));
                    valid = true;
                }
            }
        }
    }

    if (valid) {
        // 计算尺寸
        QVector3D size = maxBox - minBox;
        float maxDim = std::max({size.x(), size.y(), size.z()});

        // 目标大小，将模型归一化到 3.0 个单位
        constexpr float TARGET_SIZE = 3.0f;

        if (maxDim > 0.001f) {
            normalizationScale = TARGET_SIZE / maxDim;
            centerOffset = -(minBox + size * 0.5f) * normalizationScale;
        }

        qDebug() << "Model Bounding Box: Min" << minBox << "Max" << maxBox;
        qDebug() << "Original Size:" << maxDim;
        qDebug() << "Auto Normalization Scale:" << normalizationScale;
    }
}


std::unordered_map<std::string, int> & ModelLoader::getNameToBone() {
    return skeleton.nameToIndex;
}

// // Test 1: Node consistency with glTF children/parent mapping
// bool ModelLoader::testNodesIntegrity(const tinygltf::Model &model) {
//     bool ok = true;
//     if (nodes.size() != model.nodes.size()) {
//         qWarning() << "[TestNodes] nodes.size() mismatch with model.nodes.size()"
//                    << nodes.size() << model.nodes.size();
//         ok = false;
//     }
//
//     // For every model node i, ensure each child j has parent==i in nodes[]
//     for (size_t i = 0; i < model.nodes.size(); ++i) {
//         const tinygltf::Node &gnode = model.nodes[i];
//         for (int childIdx : gnode.children) {
//             if (childIdx < 0 || childIdx >= (int)nodes.size()) {
//                 qWarning() << "[TestNodes] invalid child index in glTF:" << childIdx << "parent gltf node:" << (int)i;
//                 ok = false;
//                 continue;
//             }
//             if (nodes[childIdx].parent != (int)i) {
//                 qWarning() << "[TestNodes] parent mismatch: nodes[" << childIdx << "].parent="
//                            << nodes[childIdx].parent << " expected=" << (int)i
//                            << " child name=" << QString::fromStdString(nodes[childIdx].name)
//                            << " parent name=" << QString::fromStdString(nodes[i].name);
//                 ok = false;
//             }
//         }
//     }
//
//     if (ok) qDebug() << "[TestNodes] OK: node parent/child mapping consistent.";
//     return ok;
// }
//
// // Test 2: Skeleton vs Skin consistency (joints count, bone.index map)
// bool ModelLoader::testSkeletonIntegrity() {
//     bool ok = true;
//     if (skeleton.bones.empty()) {
//         qWarning() << "[TestSkeleton] skeleton.bones empty";
//         return false;
//     }
//     if (skeleton.skinIndex < 0 || skeleton.skinIndex >= (int)skins.size()) {
//         qWarning() << "[TestSkeleton] invalid skeleton.skinIndex =" << skeleton.skinIndex;
//         ok = false;
//     } else {
//         const Skin &skin = skins[skeleton.skinIndex];
//         if (skin.joints.size() != skeleton.bones.size()) {
//             qWarning() << "[TestSkeleton] joints.size() != skeleton.bones.size()"
//                        << skin.joints.size() << skeleton.bones.size();
//             ok = false;
//         }
//         // ensure each bone.index matches skin.joints
//         for (size_t i = 0; i < skeleton.bones.size() && i < skin.joints.size(); ++i) {
//             if (skeleton.bones[i].index != skin.joints[i]) {
//                 qWarning() << "[TestSkeleton] bone[" << i << "].index mismatch:"
//                            << skeleton.bones[i].index << " vs skin.joints[" << i << "] =" << skin.joints[i]
//                            << " bone name=" << QString::fromStdString(skeleton.bones[i].name);
//                 ok = false;
//             }
//             // verify nameToIndex mapping
//             auto it = skeleton.nameToIndex.find(skeleton.bones[i].name);
//             if (it == skeleton.nameToIndex.end() || it->second != (int)i) {
//                 qWarning() << "[TestSkeleton] nameToIndex inconsistent for bone" << QString::fromStdString(skeleton.bones[i].name)
//                            << "map->" << (it == skeleton.nameToIndex.end() ? -999 : it->second)
//                            << "expected=" << (int)i;
//                 ok = false;
//             }
//         }
//     }
//     if (ok) qDebug() << "[TestSkeleton] OK: skeleton and skin.joints consistent.";
//     return ok;
// }
//
// // Test 3: Hierarchy consistency between Node tree and Bone parents
// bool ModelLoader::testHierarchyConsistency() {
//     bool ok = true;
//     for (size_t bi = 0; bi < skeleton.bones.size(); ++bi) {
//         const Bone &b = skeleton.bones[bi];
//         if (b.parent >= 0) {
//             // The parent's node index should equal nodes[b.parent].index
//             int parentNodeIndex = skeleton.bones[b.parent].index;
//             // And nodes[b.index].parent should equal parentNodeIndex
//             if (nodes[b.index].parent != parentNodeIndex) {
//                 qWarning() << "[TestHierarchy] bone parent node mismatch for bone" << QString::fromStdString(b.name)
//                            << " bone.parent=" << b.parent
//                            << " skeleton.bones[b.parent].index=" << parentNodeIndex
//                            << " nodes[b.index].parent=" << nodes[b.index].parent;
//                 ok = false;
//             }
//         } else {
//             // root bone: nodes[b.index].parent should be either -1 or something not in joints
//             // no strong assertion, but warn if node parent is non -1 and actually a joint
//             if (nodes[b.index].parent >= 0) {
//                 int pnode = nodes[b.index].parent;
//                 // if parent node is itself a joint -> suspicious
//                 auto it = skeleton.nameToIndex.find(nodes[pnode].name);
//                 if (it != skeleton.nameToIndex.end()) {
//                     qWarning() << "[TestHierarchy] root bone" << QString::fromStdString(b.name)
//                                << "has node parent that is also a joint:" << nodes[pnode].name;
//                     ok = false;
//                 }
//             }
//         }
//     }
//
//     if (ok) qDebug() << "[TestHierarchy] OK: bone parent relationships align with node tree.";
//     return ok;
// }
//
// // Test 4: Mesh skin arrays correctness and weights normalization
// bool ModelLoader::testMeshSkinningArrays(const MeshData &m, int meshIndex) {
//     bool ok = true;
//     size_t vcount = 0;
//     if (m.vertices.size() % 3 == 0) vcount = m.vertices.size() / 3;
//     else {
//         qWarning() << "[TestMesh] vertex array size not multiple of 3";
//         ok = false;
//     }
//
//     if (!m.hasSkinning) {
//         qDebug() << "[TestMesh] mesh has no skinning (meshIndex=" << meshIndex << ")";
//         return true; // not an error, just no skinning
//     }
//
//     if (m.boneIndices.size() != vcount * 4) {
//         qWarning() << "[TestMesh] boneIndices size mismatch: expected" << vcount*4 << "got" << m.boneIndices.size();
//         ok = false;
//     }
//     if (m.boneWeights.size() != vcount * 4) {
//         qWarning() << "[TestMesh] boneWeights size mismatch: expected" << vcount*4 << "got" << m.boneWeights.size();
//         ok = false;
//     }
//
//     // Check ranges and normalization on first N vertices and sample others
//     const int SAMPLE = std::min((size_t)16, vcount);
//     for (int i = 0; i < SAMPLE; ++i) {
//         float w0 = m.boneWeights[i*4 + 0];
//         float w1 = m.boneWeights[i*4 + 1];
//         float w2 = m.boneWeights[i*4 + 2];
//         float w3 = m.boneWeights[i*4 + 3];
//         float sum = w0 + w1 + w2 + w3;
//         if (sum <= 0.0f) {
//             qWarning() << "[TestMesh] vertex" << i << "weights sum == 0 (unbound?) meshIndex=" << meshIndex;
//             ok = false;
//         } else {
//             if (std::fabs(sum - 1.0f) > 1e-3f) {
//                 qWarning() << "[TestMesh] vertex" << i << "weights not normalized sum=" << sum << "meshIndex=" << meshIndex;
//                 // not fatal, but warn
//             }
//         }
//         // joint range check
//         for (int k = 0; k < 4; ++k) {
//             int bid = m.boneIndices[i*4 + k];
//             if (bid < -1 || bid >= (int)skeleton.bones.size()) {
//                 qWarning() << "[TestMesh] vertex" << i << "bone index out of range:" << bid << "meshIndex=" << meshIndex;
//                 ok = false;
//             }
//         }
//     }
//
//     if (ok) qDebug() << "[TestMesh] meshIndex" << meshIndex << "skin arrays look OK (sampled).";
//     return ok;
// }
//
// // Test 5: inverseBindMatrices correctness (inverse(bindGlobal) ~ inverseBind)
// bool ModelLoader::testInverseBindMatrices(float eps) {
//     if (skeleton.bones.empty() || skeleton.skinIndex < 0) {
//         qWarning() << "[TestIBM] skeleton empty or no skin";
//         return false;
//     }
//     const Skin &skin = skins[skeleton.skinIndex];
//     bool ok = true;
//     if (skin.inverseBindMatrices.size() != skeleton.bones.size()) {
//         qWarning() << "[TestIBM] IBM count != bone count" << skin.inverseBindMatrices.size() << skeleton.bones.size();
//         ok = false;
//     }
//
//     int sampleCount = std::min((size_t)10, skeleton.bones.size());
//     for (int i = 0; i < sampleCount; ++i) {
//         const Bone &b = skeleton.bones[i];
//         const QMatrix4x4 &ibm = skin.inverseBindMatrices.size() > i ? skin.inverseBindMatrices[i] : QMatrix4x4();
//         QMatrix4x4 m = ibm * b.globalTransform;
//         if (!isIdentity(m, eps)) {
//             qWarning() << "[TestIBM] IBM * bindGlobal != I for bone" << QString::fromStdString(b.name)
//                        << "sampleIndex=" << i;
//             ok = false;
//         }
//     }
//     if (ok) qDebug() << "[TestIBM] IBM * bindGlobal ≈ Identity for sampled bones.";
//     return ok;
// }
//
// // Runner: call all tests
// void ModelLoader::runAllModelTests(const tinygltf::Model &model) {
//     qDebug() << "=== Running Model Tests ===";
//     bool n_ok = testNodesIntegrity(model);
//     bool s_ok = testSkeletonIntegrity();
//     bool h_ok = testHierarchyConsistency();
//
//     bool mesh_ok_all = true;
//     for (int i = 0; i < (int)meshes.size(); ++i) {
//         bool r = testMeshSkinningArrays(meshes[i], i);
//         mesh_ok_all &= r;
//     }
//
//     bool ibm_ok = testInverseBindMatrices();
//
//     qDebug() << "=== Tests Summary ===";
//     qDebug() << "Nodes:" << n_ok << "Skeleton:" << s_ok << "Hierarchy:" << h_ok
//              << "MeshSkin:" << mesh_ok_all << "IBM:" << ibm_ok;
// }