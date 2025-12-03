//
// Created by Inoriac on 2025/10/15.
//

#include "config_manager.h"
#include <QDir>

ConfigManager::ConfigManager() {
    // 默认加载配置
    loadConfig();
}

ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::loadConfig(const QString& configPath) {
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "无法打开配置文件:" << configPath;
        return false;
    }
    
    QByteArray jsonData = file.readAll();
    file.close();
    
    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    if (!doc.isObject()) {
        qDebug() << "配置文件格式错误:" << configPath;
        return false;
    }
    
    configJson = doc.object();
    
    // 读取应用设置
    if (configJson.contains("appSettings")) {
        QJsonObject appSettings = configJson["appSettings"].toObject();
        defaultFPS = appSettings["defaultFPS"].toInt(60);
        animationSpeed = appSettings["animationSpeed"].toDouble(1.0);
        volume = appSettings["volume"].toDouble(0.75);
    }
    
    // 读取渲染设置
    if (configJson.contains("renderSettings")) {
        QJsonObject renderSettings = configJson["renderSettings"].toObject();
        antialiasing = renderSettings["antialiasing"].toBool(true);
        shadowQuality = renderSettings["shadowQuality"].toString("medium");
        textureQuality = renderSettings["textureQuality"].toString("high");
    }
    
    qDebug() << "配置文件加载成功:" << configPath;
    return true;
}