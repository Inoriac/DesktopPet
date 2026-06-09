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




std::unordered_map<std::string, int> & ModelLoader::getNameToBone() {
    return skeleton.nameToIndex;
}
