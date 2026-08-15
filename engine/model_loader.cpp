//
// Created by Inoriac on 2025/10/15.
//

#include "model_loader.h"
#include "tiny_gltf.h"

#include <QDebug>
#include <QFileInfo>
#include <cstring>
#include <functional>
#include <limits>

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
    nodes.clear();
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

    QString validationError;
    if (!validateModel(model, validationError)) {
        qWarning() << "Invalid glTF model:" << validationError;
        return false;
    }

    // 解析材质
    materials.clear();
    for (const auto &mat : model.materials) {
        materials.push_back(extractMaterialData(mat, model));
    }
    qDebug() << "Extraced" << materials.size() << "materials";

    // 解析 Node
    const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
    const tinygltf::Scene& scene = model.scenes[sceneIndex];
    nodes.resize(model.nodes.size());
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

bool ModelLoader::validateModel(const tinygltf::Model& model, QString& errorMessage) const {
    const auto fail = [&](const QString& message) {
        errorMessage = message;
        return false;
    };
    const auto validIndex = [](int index, size_t size) {
        return index >= 0 && static_cast<size_t>(index) < size;
    };

    if (model.scenes.empty()) return fail("model has no scenes");
    if (model.defaultScene >= 0 && !validIndex(model.defaultScene, model.scenes.size())) {
        return fail("default scene index is out of range");
    }

    for (size_t i = 0; i < model.bufferViews.size(); ++i) {
        const tinygltf::BufferView& view = model.bufferViews[i];
        if (!validIndex(view.buffer, model.buffers.size())) {
            return fail(QString("bufferView %1 references an invalid buffer").arg(i));
        }
        const size_t bufferSize = model.buffers[view.buffer].data.size();
        if (view.byteOffset > bufferSize || view.byteLength > bufferSize - view.byteOffset) {
            return fail(QString("bufferView %1 exceeds its buffer").arg(i));
        }
    }

    const auto validateAccessor = [&](int accessorIndex,
                                      int expectedType = -1,
                                      const QList<int>& allowedComponents = {}) {
        if (!validIndex(accessorIndex, model.accessors.size())) {
            errorMessage = QString("accessor index %1 is out of range").arg(accessorIndex);
            return false;
        }
        const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
        if (!validIndex(accessor.bufferView, model.bufferViews.size()) || accessor.sparse.isSparse) {
            errorMessage = QString("accessor %1 has no supported bufferView").arg(accessorIndex);
            return false;
        }
        if (expectedType >= 0 && accessor.type != expectedType) {
            errorMessage = QString("accessor %1 has an unexpected shape").arg(accessorIndex);
            return false;
        }
        if (!allowedComponents.isEmpty() && !allowedComponents.contains(accessor.componentType)) {
            errorMessage = QString("accessor %1 has an unsupported component type").arg(accessorIndex);
            return false;
        }

        const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[view.buffer];
        const int componentSize = tinygltf::GetComponentSizeInBytes(accessor.componentType);
        const int componentCount = tinygltf::GetNumComponentsInType(accessor.type);
        const int byteStride = accessor.ByteStride(view);
        if (componentSize <= 0 || componentCount <= 0 || byteStride <= 0) {
            errorMessage = QString("accessor %1 has an invalid element layout").arg(accessorIndex);
            return false;
        }
        const size_t elementSize = static_cast<size_t>(componentSize) * componentCount;
        const size_t stride = static_cast<size_t>(byteStride);
        if (stride < elementSize || stride % static_cast<size_t>(componentSize) != 0) {
            errorMessage = QString("accessor %1 has an invalid stride").arg(accessorIndex);
            return false;
        }
        if (view.byteOffset > std::numeric_limits<size_t>::max() - accessor.byteOffset) {
            errorMessage = QString("accessor %1 offset overflows").arg(accessorIndex);
            return false;
        }
        const size_t start = view.byteOffset + accessor.byteOffset;
        if (start > buffer.data.size() || start % static_cast<size_t>(componentSize) != 0) {
            errorMessage = QString("accessor %1 starts outside or unaligned in its buffer").arg(accessorIndex);
            return false;
        }
        if (accessor.count == 0) return true;
        if (accessor.count - 1 > (std::numeric_limits<size_t>::max() - elementSize) / stride) {
            errorMessage = QString("accessor %1 size overflows").arg(accessorIndex);
            return false;
        }
        const size_t required = (accessor.count - 1) * stride + elementSize;
        if (accessor.byteOffset > view.byteLength || required > view.byteLength - accessor.byteOffset) {
            errorMessage = QString("accessor %1 exceeds its bufferView").arg(accessorIndex);
            return false;
        }
        return true;
    };

    std::vector<int> nodeState(model.nodes.size(), 0);
    std::function<bool(int)> validateNode = [&](int nodeIndex) {
        if (!validIndex(nodeIndex, model.nodes.size())) {
            errorMessage = QString("node index %1 is out of range").arg(nodeIndex);
            return false;
        }
        if (nodeState[nodeIndex] == 1) {
            errorMessage = QString("node graph contains a cycle at %1").arg(nodeIndex);
            return false;
        }
        if (nodeState[nodeIndex] == 2) return true;
        nodeState[nodeIndex] = 1;
        for (int child : model.nodes[nodeIndex].children) {
            if (!validateNode(child)) return false;
        }
        nodeState[nodeIndex] = 2;
        return true;
    };
    for (const tinygltf::Scene& scene : model.scenes) {
        for (int node : scene.nodes) {
            if (!validIndex(node, model.nodes.size())) return fail("scene references an invalid node");
        }
    }
    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
        const tinygltf::Node& node = model.nodes[nodeIndex];
        if (node.mesh >= 0 && !validIndex(node.mesh, model.meshes.size())) {
            return fail(QString("node %1 references an invalid mesh").arg(nodeIndex));
        }
        if (node.skin >= 0 && !validIndex(node.skin, model.skins.size())) {
            return fail(QString("node %1 references an invalid skin").arg(nodeIndex));
        }
        if (!validateNode(static_cast<int>(nodeIndex))) return false;
    }

    for (size_t skinIndex = 0; skinIndex < model.skins.size(); ++skinIndex) {
        const tinygltf::Skin& skin = model.skins[skinIndex];
        if (skin.joints.empty()) {
            return fail(QString("skin %1 has no joints").arg(skinIndex));
        }
        if (skin.skeleton >= 0 && !validIndex(skin.skeleton, model.nodes.size())) {
            return fail(QString("skin %1 has an invalid skeleton root").arg(skinIndex));
        }
        for (int joint : skin.joints) {
            if (!validIndex(joint, model.nodes.size())) {
                return fail(QString("skin %1 has an invalid joint").arg(skinIndex));
            }
        }
        if (skin.inverseBindMatrices >= 0
            && !validateAccessor(skin.inverseBindMatrices,
                                 TINYGLTF_TYPE_MAT4,
                                 {TINYGLTF_COMPONENT_TYPE_FLOAT})) {
            return false;
        }
        if (skin.inverseBindMatrices >= 0
            && model.accessors[skin.inverseBindMatrices].count != skin.joints.size()) {
            return fail(QString("skin %1 inverse bind matrix count differs from joint count").arg(skinIndex));
        }
    }

    for (const tinygltf::Texture& texture : model.textures) {
        if (texture.source >= 0 && !validIndex(texture.source, model.images.size())) {
            return fail("texture references an invalid image");
        }
    }

    for (const tinygltf::Mesh& mesh : model.meshes) {
        for (const tinygltf::Primitive& primitive : mesh.primitives) {
            if (primitive.material >= 0 && !validIndex(primitive.material, model.materials.size())) {
                return fail("primitive references an invalid material");
            }
            const auto validateAttribute = [&](const char* name,
                                               int type,
                                               const QList<int>& components) {
                const auto it = primitive.attributes.find(name);
                return it == primitive.attributes.end() || validateAccessor(it->second, type, components);
            };
            if (!validateAttribute("POSITION", TINYGLTF_TYPE_VEC3, {TINYGLTF_COMPONENT_TYPE_FLOAT})
                || !validateAttribute("NORMAL", TINYGLTF_TYPE_VEC3, {TINYGLTF_COMPONENT_TYPE_FLOAT})
                || !validateAttribute("TEXCOORD_0", TINYGLTF_TYPE_VEC2, {TINYGLTF_COMPONENT_TYPE_FLOAT})
                || !validateAttribute("JOINTS_0", TINYGLTF_TYPE_VEC4,
                                      {TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                       TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT})
                || !validateAttribute("WEIGHTS_0", TINYGLTF_TYPE_VEC4,
                                      {TINYGLTF_COMPONENT_TYPE_FLOAT,
                                       TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                       TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT})) {
                return false;
            }
            if (primitive.indices >= 0
                && !validateAccessor(primitive.indices,
                                     TINYGLTF_TYPE_SCALAR,
                                     {TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                      TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                      TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT})) {
                return false;
            }
            const bool hasJoints = primitive.attributes.count("JOINTS_0") > 0;
            const bool hasWeights = primitive.attributes.count("WEIGHTS_0") > 0;
            if (hasJoints != hasWeights || ((hasJoints || hasWeights) && model.skins.empty())) {
                return fail("primitive has incomplete skinning data");
            }
            if (hasJoints) {
                const auto& joints = model.accessors[primitive.attributes.at("JOINTS_0")];
                const auto& weights = model.accessors[primitive.attributes.at("WEIGHTS_0")];
                if (joints.count != weights.count) return fail("skinning accessor counts differ");
            }

            const auto positionIt = primitive.attributes.find("POSITION");
            if (positionIt != primitive.attributes.end()) {
                const size_t vertexCount = model.accessors[positionIt->second].count;
                for (const char* attribute : {"NORMAL", "TEXCOORD_0", "JOINTS_0", "WEIGHTS_0"}) {
                    const auto it = primitive.attributes.find(attribute);
                    if (it != primitive.attributes.end() && model.accessors[it->second].count != vertexCount) {
                        return fail(QString("primitive attribute %1 count differs from POSITION").arg(attribute));
                    }
                }
                if (primitive.indices >= 0) {
                    const tinygltf::Accessor& accessor = model.accessors[primitive.indices];
                    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
                    const tinygltf::Buffer& buffer = model.buffers[view.buffer];
                    const size_t stride = static_cast<size_t>(accessor.ByteStride(view));
                    const unsigned char* data = accessor.count > 0
                        ? buffer.data.data() + view.byteOffset + accessor.byteOffset
                        : nullptr;
                    for (size_t i = 0; i < accessor.count; ++i) {
                        quint32 index = 0;
                        const unsigned char* element = data + i * stride;
                        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                            index = *element;
                        } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                            quint16 value = 0;
                            std::memcpy(&value, element, sizeof(value));
                            index = value;
                        } else {
                            std::memcpy(&index, element, sizeof(index));
                        }
                        if (index >= vertexCount) return fail("primitive index exceeds vertex count");
                    }
                }
            } else if (primitive.indices >= 0 || hasJoints || hasWeights) {
                return fail("primitive data requires a POSITION attribute");
            }
        }
    }

    return true;
}




std::unordered_map<std::string, int> & ModelLoader::getNameToBone() {
    return skeleton.nameToIndex;
}
