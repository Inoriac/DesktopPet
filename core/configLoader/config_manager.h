//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_CONFIG_MANAGER_H
#define DESKTOP_PET_CONFIG_MANAGER_H

#include <QJsonObject>
#include <QString>
#include <QJsonDocument>
#include <QPoint>
#include <QSize>

#include "global_types.h"
#include "ai_types.h"

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

    int getDragThreshold() const { return dragThreshold; }
    int getClickTimeout() const { return clickTimeout; }

    // 窗口吸附参数
    int getWindowSnapThreshold() const { return windowSnapThreshold; }
    int getWindowSnapVerticalOffset() const { return windowSnapVerticalOffset; }
    QPoint getWindowSnapZoneOffset() const { return windowSnapZoneOffset; }
    QSize getWindowSnapZoneSize() const { return windowSnapZoneSize; }
    int getWindowSnapFollowIntervalMs() const { return windowSnapFollowIntervalMs; }
    bool getWindowSnapForceExitOnBigScreenAlarm() const { return windowSnapForceExitOnBigScreenAlarm; }
    int getTotalWindowSitAnimations() const { return totalWindowSitAnimations; }

    // 获取碰撞配置列表
    const std::vector<BoneCollider>& getColliderConfigs() const { return colliderConfigs; }

    // LLM 配置
    const LlmConfig& getLlmConfig() const { return llmConfig; }
    void setLlmEnabled(bool enabled) { llmConfig.enabled = enabled; }
    const ScreenChatConfig& getScreenChatConfig() const { return screenChatConfig; }
    const VoiceConfig& getVoiceConfig() const { return voiceConfig; }
    void setVoiceConfig(const VoiceConfig& config) { voiceConfig = config; }

    // AI 行为策略
    const AiBehaviorPolicy& getAiBehaviorPolicy() const { return aiBehaviorPolicy; }
    const AiToolAccessPolicy& getAiToolAccessPolicy() const { return aiToolAccessPolicy; }
    
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

    // 触摸相关参数
    int dragThreshold = 5;
    int clickTimeout = 200;

    // 窗口吸附参数
    int windowSnapThreshold = 30;
    int windowSnapVerticalOffset = 0;
    QPoint windowSnapZoneOffset {0, -5};
    QSize windowSnapZoneSize {100, 10};
    int windowSnapFollowIntervalMs = 16;
    bool windowSnapForceExitOnBigScreenAlarm = true;
    int totalWindowSitAnimations = 0;

    std::vector<BoneCollider> colliderConfigs;

    LlmConfig llmConfig;
    ScreenChatConfig screenChatConfig;
    VoiceConfig voiceConfig;
    AiBehaviorPolicy aiBehaviorPolicy;
    AiToolAccessPolicy aiToolAccessPolicy;
};

#endif //DESKTOP_PET_CONFIG_MANAGER_H