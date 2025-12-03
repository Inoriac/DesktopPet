//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_CONFIG_MANAGER_H
#define DESKTOP_PET_CONFIG_MANAGER_H

#include <QJsonObject>
#include <QJsonDocument>
#include <QString>
#include <QFile>
#include <QDebug>

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
    
    QJsonObject configJson;
};

#endif //DESKTOP_PET_CONFIG_MANAGER_H