//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_CONFIG_MANAGER_H
#define DESKTOP_PET_CONFIG_MANAGER_H

#include <QJsonObject>
#include <QString>
#include <QJsonDocument>

#include "global_types.h"

class ConfigManager {
public:
    static ConfigManager& instance();
    
    bool loadConfig(const QString& configPath = "config/default_common_config.json");
    
    // 应用设置
    int getDefaultFPS() const { return defaultFPS; }
    float getAnimationSpeed() const { return animationSpeed; }
    float getVolume() const { return volume; }
    
    // 渲染设置
    bool isAntialiasingEnabled() const { return antialiasing; }
    QString getShadowQuality() const { return shadowQuality; }
    QString getTextureQuality() const { return textureQuality; }

    QString getStateMachinePath() const { return stateMachinePath; }
    QString getAnimationsBasePath() const { return animationsBasePath; }

    // 相机设置
    QVector3D getDefaultCameraEye() const { return defaultCameraEye; }
    QVector3D getDefaultCameraCenter() const { return defaultCameraCenter; }

    // 获取碰撞配置列表
    const std::vector<BoneCollider>& getColliderConfigs() const { return colliderConfigs; }
    
private:
    ConfigManager();
    ~ConfigManager() = default;
    
    // 应用设置
    int defaultFPS = 60;
    float animationSpeed = 1.0f;
    float volume = 0.75f;
    
    // 渲染设置
    bool antialiasing = true;
    QString shadowQuality = "medium";
    QString textureQuality = "high";

    QString stateMachinePath = "config/animation_state_machine.json";
    QString animationsBasePath = "assets/animations/";
    
    QJsonObject configJson;

    // 相机默认设置
    QVector3D defaultCameraEye {0.0f, 3.0f, 12.0f};
    QVector3D defaultCameraCenter {0.0f, 4.0f, 0.0f};

    std::vector<BoneCollider> colliderConfigs;
};

#endif //DESKTOP_PET_CONFIG_MANAGER_H