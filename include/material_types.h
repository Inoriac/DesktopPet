#pragma once

#include <string>
#include <vector>
#include <QVector3D>

// 用于 CPU 端保存 glTF 中的材质数据
struct MaterialData {
    std::string name;

    QVector3D baseColorFactor {1.0f, 1.0f, 1.0f};
    float alphaFactor {1.0f};
    float metallicFactor {0.0f};
    float roughnessFactor {1.0f};

    std::string albedoTexPath;
    std::vector<unsigned char> albedoImageData;
    std::string albedoMimeType;     // 内嵌数据格式 如 "image/png" 用于.glb的保留字段
    int albedoWidth = 0;
    int albedoHeight = 0;

    std::string normalTexPath;
    std::vector<unsigned char> normalImageData;
    std::string normalMimeType;
    int normalWidth = 0;
    int normalHeight = 0;

    std::string metallicRoughnessTexPath;
    std::vector<unsigned char> metallicRoughnessImageData;
    std::string metallicRoughnessMimeType;
    int metallicRoughnessWidth = 0;
    int metallicRoughnessHeight = 0;

    std::string aoTexPath;
    std::vector<unsigned char> aoImageData;
    std::string aoMimeType;
    int aoWidth = 0;
    int aoHeight = 0;

    std::string emissiveTexPath;
    std::vector<unsigned char> emissiveImageData;
    std::string emissionMimeType;
    int emissiveWidth = 0;
    int emissiveHeight = 0;

    // GPU 纹理 ID
    unsigned int albedoTexID {0};
    unsigned int normalTexID {0};
    unsigned int metallicRoughnessTexID {0};
    unsigned int aoTexID {0};
    unsigned int emissiveTexID {0};
};
