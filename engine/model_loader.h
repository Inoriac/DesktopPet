//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_MODEL_LOADER_H
#define DESKTOP_PET_MODEL_LOADER_H

#include <QMatrix4x4>
#include <QVector3D>
#include <string>
#include <vector>

#include "tiny_gltf.h"
#include "model_types.h"
#include "material_types.h"

class ModelLoader {
public:
    ModelLoader();
    ~ModelLoader();

    bool loadModel(const std::string& path);
    const std::vector<MeshData>& getMeshes() const { return meshes; };
    std::vector<MaterialData>& getMaterials() { return materials; };
    void clear();

    // 获取归一化的缩放比例
    float getNormalizationScale() const { return normalizationScale; };
    // 获取模型原本的中心偏移点
    QVector3D getCenterOffset() const { return centerOffset; };

    // 临时测试方法
    std::unordered_map<std::string, int>& getNameToBone();
    Skeleton& getSkeleton() { return skeleton; };
    Skeleton releaseSkeleton();

private:
    std::string modelDirectory;

    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;    // 存储所有材质
    std::vector<Node> nodes;
    std::vector<Skin> skins;
    Skeleton skeleton;  // 骨骼信息

    float normalizationScale = 1.0f;
    QVector3D centerOffset;

private:
    // 辅助方法
    bool parseGLTF(const std::string& path);    // 加载模型
    void extractMeshData(const tinygltf::Model& model, const tinygltf::Mesh& mesh); // 获取 MeshData

    void extractVertexData(const tinygltf::Model& model, const tinygltf::Primitive& primitive, MeshData& meshData); // 提取顶点数据
    void extractIndexData(const tinygltf::Model& model, const tinygltf::Primitive& primitive, MeshData& meshData);  // 提取索引数据
    MaterialData extractMaterialData(const tinygltf::Material& mat, const tinygltf::Model& model);     // 提取材质信息

    /// @brief 解析 gltf 节点结构
    /// @param model Model的引用
    /// @param nodeIndex 当前节点的索引
    /// @param parentIndex 当前节点的父索引
    void extractNodes(const tinygltf::Model& model, int nodeIndex, int parentIndex);
    void extractSkeleton(const tinygltf::Model& model);     // 读取骨骼列表与 inverseBindMatrices
    void extractSkeletonHierarchy(const tinygltf::Model& model);    // 解析骨骼层级
    void extractSkinningData(const tinygltf::Model& model, const tinygltf::Primitive& primitive, MeshData& meshData);   // 解析 mesh 的皮肤数据

    void calculateBoundingBox(const tinygltf::Model& model);    // 计算包围盒

    // // 临时测试方法
    // bool testNodesIntegrity(const tinygltf::Model &model);
    // bool testSkeletonIntegrity();
    // bool testHierarchyConsistency();
    // bool testMeshSkinningArrays(const MeshData &m, int meshIndex = -1);
    // bool testInverseBindMatrices(float eps = 1e-2f);
    // void runAllModelTests(const tinygltf::Model &model);

    // // 小工具：判断矩阵是否接近单位矩阵
    // static bool isIdentity(const QMatrix4x4 &m, float eps = 1e-3f) {
    //     // Compare to identity
    //     for (int r = 0; r < 4; ++r) {
    //         for (int c = 0; c < 4; ++c) {
    //             float expected = (r == c) ? 1.0f : 0.0f;
    //             if (std::fabs(m(r, c) - expected) > eps) return false;
    //         }
    //     }
    //     return true;
    // }

    // 构建 tinygltf_matrix -> QMatrix4x4
    static QMatrix4x4 getNodeLocalTransform(const tinygltf::Node& node)
    {
        QMatrix4x4 M;
        M.setToIdentity();

        // ========= Case 1: matrix =========
        if (node.matrix.size() == 16) {
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    M(row, col) = static_cast<float>(node.matrix[col * 4 + row]);
                }
            }
            return M;
        }

        // ========= Case 2: TRS =========
        QMatrix4x4 T, R, S;
        T.setToIdentity();
        R.setToIdentity();
        S.setToIdentity();

        // T -----
        if (!node.translation.empty()) {
            T(0,3) = node.translation[0];
            T(1,3) = node.translation[1];
            T(2,3) = node.translation[2];
        }

        // R -----
        if (!node.rotation.empty()) {
            // glTF rotation = [x, y, z, w]
            QQuaternion q(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);

            // 手动构造旋转矩阵（避免 Qt 内部坐标系不同造成偏差）
            float xx = q.x() * q.x();
            float yy = q.y() * q.y();
            float zz = q.z() * q.z();
            float xy = q.x() * q.y();
            float xz = q.x() * q.z();
            float yz = q.y() * q.z();
            float wx = q.scalar() * q.x();
            float wy = q.scalar() * q.y();
            float wz = q.scalar() * q.z();

            R(0,0) = 1 - 2 * (yy + zz);
            R(0,1) = 2 * (xy - wz);
            R(0,2) = 2 * (xz + wy);

            R(1,0) = 2 * (xy + wz);
            R(1,1) = 1 - 2 * (xx + zz);
            R(1,2) = 2 * (yz - wx);

            R(2,0) = 2 * (xz - wy);
            R(2,1) = 2 * (yz + wx);
            R(2,2) = 1 - 2 * (xx + yy);
        }

        // S -----
        if (!node.scale.empty()) {
            S(0,0) = node.scale[0];
            S(1,1) = node.scale[1];
            S(2,2) = node.scale[2];
        }

        M = T * R * S;
        return M;
    }
};

#endif //DESKTOP_PET_MODEL_LOADER_H