//
// Created by Inoriac on 2025/10/15.
//

#include "config_manager.h"
#include <QDir>
#include <QJsonArray>

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

        // 读取相机设置
        if (renderSettings.contains("cameraSettings")) {
            QJsonObject camObj = renderSettings["cameraSettings"].toObject();
            QJsonArray eyeArr = camObj["defaultEye"].toArray();
            QJsonArray centerArr = camObj["defaultCenter"].toArray();

            if (eyeArr.size() == 3) {
                defaultCameraEye = QVector3D(eyeArr[0].toDouble(), eyeArr[1].toDouble(), eyeArr[2].toDouble());
            }
            if (centerArr.size() == 3) {
                defaultCameraCenter = QVector3D(centerArr[0].toDouble(), centerArr[1].toDouble(), centerArr[2].toDouble());
            }
        }
    }

    // 读取碰撞配置
    colliderConfigs.clear();
    if (configJson.contains("interactionSettings")) {
        QJsonObject interaction = configJson["interactionSettings"].toObject();

        dragThreshold = interaction["dragThreshold"].toInt(5);
        clickTimeout = interaction["clickTimeout"].toInt(200);

        if (interaction.contains("colliders")) {
            QJsonArray arr = interaction["colliders"].toArray();
            for (const auto& val : arr) {
                QJsonObject obj = val.toObject();

                std::string bone = obj["bone"].toString().toStdString();
                float r = obj["radius"].toDouble(0.2);
                std::string tag = obj["tag"].toString("Body").toStdString();

                QVector3D offset(0,0,0);
                if (obj.contains("offset")) {
                    QJsonArray off = obj["offset"].toArray();
                    if (off.size() >= 3) {
                        offset = QVector3D(off[0].toDouble(), off[1].toDouble(), off[2].toDouble());
                    }
                }

                // 存入全局结构体
                colliderConfigs.emplace_back(bone, r, offset, tag);
            }
        }
    }
    
    qDebug() << "配置文件加载成功:" << configPath;
    return true;
}