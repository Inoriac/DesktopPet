//
// Created by Inoriac on 2025/10/15.
//

#include "config_manager.h"
#include <QDir>
#include <QJsonArray>
#include <QFile>
#include <QDebug>
#include <algorithm>

static QStringList jsonArrayToStringList(const QJsonArray& arr) {
    QStringList list;
    for (const auto& v : arr) {
        const QString s = v.toString().trimmed();
        if (!s.isEmpty()) {
            list.append(s);
        }
    }
    return list;
}

static AiTriggerConfig parseTriggerConfig(const QJsonObject& triggerObj,
                                          int defaultMinMs,
                                          int defaultMaxMs) {
    AiTriggerConfig cfg;
    cfg.enabled = triggerObj.value("enabled").toBool(true);
    cfg.minIntervalMs = triggerObj.value("minIntervalMs").toInt(defaultMinMs);
    cfg.maxIntervalMs = triggerObj.value("maxIntervalMs").toInt(defaultMaxMs);

    if (cfg.minIntervalMs < 1000) cfg.minIntervalMs = 1000;
    if (cfg.maxIntervalMs < cfg.minIntervalMs) cfg.maxIntervalMs = cfg.minIntervalMs;

    return cfg;
}

static int clampInt(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

static DaydreamConfig parseDaydreamConfig(const QJsonObject& object) {
    DaydreamConfig cfg;
    cfg.enabled = object.value("enabled").toBool(true);
    cfg.idleThresholdSec = clampInt(
        object.value("idleThresholdSec").toInt(cfg.idleThresholdSec), 30, 24 * 60 * 60);
    cfg.dueSoonThresholdMs = clampInt(
        object.value("dueSoonThresholdMs").toInt(cfg.dueSoonThresholdMs), 0, 24 * 60 * 60 * 1000);
    cfg.minIntervalMs = clampInt(
        object.value("minIntervalMs").toInt(cfg.minIntervalMs), 60 * 1000, 24 * 60 * 60 * 1000);
    cfg.interruptionBackoffMs = clampInt(
        object.value("interruptionBackoffMs").toInt(cfg.interruptionBackoffMs), 0, 24 * 60 * 60 * 1000);
    cfg.hourlyLimit = clampInt(object.value("hourlyLimit").toInt(cfg.hourlyLimit), 1, 24);
    cfg.tickIntervalMs = clampInt(
        object.value("tickIntervalMs").toInt(cfg.tickIntervalMs), 5 * 1000, 5 * 60 * 1000);

    cfg.sessionLimit = clampInt(object.value("sessionLimit").toInt(cfg.sessionLimit), 1, 128);
    cfg.batchLimit = clampInt(
        object.value("batchLimit").toInt(cfg.batchLimit), 1, std::min(cfg.sessionLimit, 32));
    cfg.inboxLimit = clampInt(object.value("inboxLimit").toInt(cfg.inboxLimit), 1, 5000);
    cfg.inboxLimit = std::max(cfg.inboxLimit, cfg.sessionLimit);
    cfg.relatedMemoryLimit = clampInt(
        object.value("relatedMemoryLimit").toInt(cfg.relatedMemoryLimit), 0, 32);

    cfg.model = object.value("model").toString().trimmed().left(256);
    cfg.maxTokens = clampInt(object.value("maxTokens").toInt(cfg.maxTokens), 256, 8192);
    cfg.temperature = std::clamp(
        object.value("temperature").toDouble(cfg.temperature), 0.0, 2.0);
    return cfg;
}

static QString cleanConfigPath(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    const QString cleaned = QDir::cleanPath(trimmed);
    if (QDir::isAbsolutePath(cleaned)) {
        return cleaned;
    }
    return QDir::cleanPath(QDir::current().absoluteFilePath(cleaned));
}

static VoiceConfig parseVoiceConfig(const QJsonObject& voiceObj) {
    VoiceConfig cfg;
    cfg.enabled = voiceObj.value("enabled").toBool(false);
    cfg.backend = voiceObj.value("backend").toString("genie-tts").trimmed();
    if (cfg.backend.isEmpty()) cfg.backend = "genie-tts";

    cfg.pythonExecutable = voiceObj.value("pythonExecutable").toString().trimmed();
    cfg.venvPath = voiceObj.value("venvPath").toString(".venv").trimmed();
    if (cfg.venvPath.isEmpty()) cfg.venvPath = ".venv";
    cfg.workerScript = voiceObj.value("workerScript").toString("tools/voice/genie_worker.py").trimmed();
    if (cfg.workerScript.isEmpty()) cfg.workerScript = "tools/voice/genie_worker.py";
    cfg.preloadOnStart = voiceObj.value("preloadOnStart").toBool(true);
    cfg.allowAutoDownload = voiceObj.value("allowAutoDownload").toBool(false);

    cfg.genieDataDir = voiceObj.value("genieDataDir").toString("runtime/voice/GenieData").trimmed();
    cfg.characterModelsDir = voiceObj.value("characterModelsDir").toString("runtime/voice/CharacterModels").trimmed();
    cfg.customCharactersDir = voiceObj.value("customCharactersDir").toString("runtime/voice/custom_characters").trimmed();
    if (cfg.genieDataDir.isEmpty()) cfg.genieDataDir = "runtime/voice/GenieData";
    if (cfg.characterModelsDir.isEmpty()) cfg.characterModelsDir = "runtime/voice/CharacterModels";
    if (cfg.customCharactersDir.isEmpty()) cfg.customCharactersDir = "runtime/voice/custom_characters";

    cfg.speakerMode = voiceObj.value("speakerMode").toString("predefined").trimmed().toLower();
    if (cfg.speakerMode != "predefined" && cfg.speakerMode != "custom") {
        cfg.speakerMode = "predefined";
    }
    cfg.selectedSpeaker = voiceObj.value("selectedSpeaker").toString("feibi").trimmed().toLower();
    if (cfg.selectedSpeaker != "feibi" && cfg.selectedSpeaker != "mika" && cfg.selectedSpeaker != "thirtyseven") {
        cfg.selectedSpeaker = "feibi";
    }

    if (voiceObj.contains("customSpeaker") && voiceObj.value("customSpeaker").isObject()) {
        const QJsonObject customObj = voiceObj.value("customSpeaker").toObject();
        cfg.customSpeaker.name = customObj.value("name").toString().trimmed();
        cfg.customSpeaker.language = customObj.value("language").toString("zh").trimmed().toLower();
        if (cfg.customSpeaker.language.isEmpty()) cfg.customSpeaker.language = "zh";
        cfg.customSpeaker.onnxModelDir = customObj.value("onnxModelDir").toString().trimmed();
        cfg.customSpeaker.referenceAudioPath = customObj.value("referenceAudioPath").toString().trimmed();
        cfg.customSpeaker.referenceAudioText = customObj.value("referenceAudioText").toString().trimmed();
    }

    cfg.saveAudio = voiceObj.value("saveAudio").toBool(false);
    cfg.outputDir = voiceObj.value("outputDir").toString("runtime/voice/outputs").trimmed();
    if (cfg.outputDir.isEmpty()) cfg.outputDir = "runtime/voice/outputs";

    if (voiceObj.contains("sources") && voiceObj.value("sources").isObject()) {
        const QJsonObject sourcesObj = voiceObj.value("sources").toObject();
        cfg.sources.assistant = sourcesObj.value("assistant").toBool(true);
        cfg.sources.proactive = sourcesObj.value("proactive").toBool(true);
        cfg.sources.screenChat = sourcesObj.value("screenChat").toBool(true);
        cfg.sources.fallback = sourcesObj.value("fallback").toBool(true);
        cfg.sources.toolBubble = sourcesObj.value("toolBubble").toBool(false);
    }

    cfg.maxTextChars = clampInt(voiceObj.value("maxTextChars").toInt(350), 20, 2000);
    return cfg;
}

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
                float r = obj["hoverRadius"].toDouble(0.25);
                std::string tag = obj["tag"].toString("Body").toStdString();

                QVector3D offset(0,0,0);
                if (obj.contains("offset")) {
                    QJsonArray off = obj["offset"].toArray();
                    if (off.size() >= 3) {
                        offset = QVector3D(off[0].toDouble(), off[1].toDouble(), off[2].toDouble());
                    }
                }

                QVector3D worldOffset(0,0,0);
                if (obj.contains("worldOffset")) {
                    QJsonArray off = obj["worldOffset"].toArray();
                    if (off.size() >= 3) {
                        worldOffset = QVector3D(off[0].toDouble(), off[1].toDouble(), off[2].toDouble());
                    }
                }

                // 存入全局结构体
                colliderConfigs.emplace_back(bone, r, offset, worldOffset, tag);
            }
        }

        if (interaction.contains("windowSnapping") && interaction["windowSnapping"].isObject()) {
            QJsonObject windowSnapping = interaction["windowSnapping"].toObject();
            windowSnapThreshold = windowSnapping["snapThreshold"].toInt(30);
            windowSnapVerticalOffset = windowSnapping["verticalOffset"].toInt(0);
            windowSnapFollowIntervalMs = windowSnapping["followIntervalMs"].toInt(16);
            if (windowSnapFollowIntervalMs < 5) {
                windowSnapFollowIntervalMs = 5;
            }

            if (windowSnapping.contains("snapZoneOffset") && windowSnapping["snapZoneOffset"].isArray()) {
                QJsonArray zoneOffsetArr = windowSnapping["snapZoneOffset"].toArray();
                if (zoneOffsetArr.size() >= 2) {
                    windowSnapZoneOffset = QPoint(zoneOffsetArr[0].toInt(0), zoneOffsetArr[1].toInt(-5));
                }
            }

            if (windowSnapping.contains("snapZoneSize") && windowSnapping["snapZoneSize"].isArray()) {
                QJsonArray zoneSizeArr = windowSnapping["snapZoneSize"].toArray();
                if (zoneSizeArr.size() >= 2) {
                    windowSnapZoneSize = QSize(zoneSizeArr[0].toInt(100), zoneSizeArr[1].toInt(10));
                }
            }

            windowSnapForceExitOnBigScreenAlarm = windowSnapping["forceExitOnBigScreenAlarm"].toBool(true);
            totalWindowSitAnimations = windowSnapping["totalWindowSitAnimations"].toInt(0);
        }
    }

    // 宠物展示设置（尺寸/置顶/点击穿透）。缺失 petSettings 块时沿用成员默认值。
    if (configJson.contains("petSettings")) {
        const QJsonObject pet = configJson["petSettings"].toObject();
        petScalePercent = clampInt(pet["scalePercent"].toInt(100), 50, 200);
        petAlwaysOnTop = pet["alwaysOnTop"].toBool(true);
        petClickThrough = pet["clickThrough"].toBool(false);
    }

    // 读取 AI 配置
    // 支持两种格式：
    // 1) aiSettings 直接包含字段
    // 2) aiSettings.profiles + activeProfile
    llmConfig = LlmConfig{};
    screenChatConfig = ScreenChatConfig{};
    daydreamConfig = DaydreamConfig{};
    voiceConfig = VoiceConfig{};
    aiBehaviorPolicy = AiBehaviorPolicy{};
    aiToolAccessPolicy = AiToolAccessPolicy{};
    aiToolAccessPolicy.allowedRoots = {
        QDir::cleanPath(QDir::currentPath())
    };
    aiToolAccessPolicy.autoGrantedTools = {
        "read_text_file",
        "list_directory"
    };
    aiBehaviorPolicy.idleActionWhitelist = {
        "Idle", "Sitting", "Sleeping", "Happy", "Talk", "Dance"
    };
    aiBehaviorPolicy.touchActionWhitelist = {
        "TouchHead", "TouchBody", "TouchHandL", "TouchHandR", "Happy"
    };
    aiBehaviorPolicy.emotionActionWhitelist = {
        "Happy", "Cry", "Angry", "Fear", "Talk"
    };
    aiBehaviorPolicy.forbiddenActions = {"Drag", "WindowSit"};

    aiBehaviorPolicy.idleTrigger = AiTriggerConfig{true, 60000, 180000};
    aiBehaviorPolicy.emotionTrigger = AiTriggerConfig{true, 120000, 300000};
    aiBehaviorPolicy.proactiveChatTrigger = AiTriggerConfig{true, 180000, 300000};

    if (configJson.contains("aiSettings")) {
        const QJsonObject aiSettings = configJson.value("aiSettings").toObject();
        QJsonObject aiRaw = aiSettings;

        if (aiSettings.contains("profiles")) {
            const QString activeProfile = aiSettings.value("activeProfile").toString("default");
            const QJsonObject profiles = aiSettings.value("profiles").toObject();
            if (profiles.contains(activeProfile)) {
                aiRaw = profiles.value(activeProfile).toObject();
            } else if (profiles.contains("default")) {
                aiRaw = profiles.value("default").toObject();
            }
        }

        llmConfig.enabled = aiRaw.value("enabled").toBool(false);
        llmConfig.provider = aiRaw.value("provider").toString("openai-compatible");
        llmConfig.baseUrl = aiRaw.value("baseUrl").toString("https://api.openai.com/v1");
        llmConfig.apiKey = aiRaw.value("apiKey").toString("");
        llmConfig.model = aiRaw.value("model").toString("gpt-4o-mini");
        llmConfig.visualModel = aiRaw.value("visual_model").toString(llmConfig.model);

        llmConfig.timeoutMs = aiRaw.value("timeoutMs").toInt(30000);
        llmConfig.maxTokens = aiRaw.value("maxTokens").toInt(512);
        llmConfig.temperature = aiRaw.value("temperature").toDouble(0.7);
        llmConfig.retryCount = aiRaw.value("retryCount").toInt(1);
        llmConfig.thinkIntervalMs = aiRaw.value("thinkIntervalMs").toInt(30000);

        if (aiRaw.contains("extraParams") && aiRaw.value("extraParams").isObject()) {
            llmConfig.extraParams = aiRaw.value("extraParams").toObject();
        }

        if (aiRaw.contains("screenChat") && aiRaw.value("screenChat").isObject()) {
            const QJsonObject screenChatObj = aiRaw.value("screenChat").toObject();
            screenChatConfig.enabled = screenChatObj.value("enabled").toBool(false);
            screenChatConfig.minIntervalMs = screenChatObj.value("minIntervalMs").toInt(8 * 60 * 1000);
            screenChatConfig.maxIntervalMs = screenChatObj.value("maxIntervalMs").toInt(12 * 60 * 1000);

            if (screenChatConfig.minIntervalMs < 1000) {
                screenChatConfig.minIntervalMs = 1000;
            }
            if (screenChatConfig.maxIntervalMs < screenChatConfig.minIntervalMs) {
                screenChatConfig.maxIntervalMs = screenChatConfig.minIntervalMs;
            }

            screenChatConfig.bubbleOpacityPercent = clampInt(
                screenChatObj.value("bubbleOpacityPercent").toInt(80),
                10,
                100);
            screenChatConfig.bubbleFontSize = clampInt(
                screenChatObj.value("bubbleFontSize").toInt(14),
                10,
                36);
            screenChatConfig.bubbleOffsetX = screenChatObj.value("bubbleOffsetX").toInt(0);
            screenChatConfig.bubbleOffsetY = screenChatObj.value("bubbleOffsetY").toInt(-20);
            screenChatConfig.bubbleDurationMs = clampInt(
                screenChatObj.value("bubbleDurationMs").toInt(8000),
                1000,
                30000);

            const QString gender = screenChatObj.value("petGender").toString("female").trimmed().toLower();
            if (gender == "male" || gender == "female" || gender == "neutral") {
                screenChatConfig.petGender = gender;
            } else {
                screenChatConfig.petGender = "female";
            }
        }

        if (aiRaw.contains("daydream") && aiRaw.value("daydream").isObject()) {
            daydreamConfig = parseDaydreamConfig(aiRaw.value("daydream").toObject());
        }

        if (aiRaw.contains("voice") && aiRaw.value("voice").isObject()) {
            voiceConfig = parseVoiceConfig(aiRaw.value("voice").toObject());
        }

        // 读取工具访问策略。默认只允许读当前工作目录；写文件和命令执行必须显式开启。
        if (aiRaw.contains("toolAccessPolicy") && aiRaw.value("toolAccessPolicy").isObject()) {
            const QJsonObject toolPolicyObj = aiRaw.value("toolAccessPolicy").toObject();

            if (toolPolicyObj.contains("allowedRoots")) {
                QStringList roots;
                for (const QString& root : jsonArrayToStringList(toolPolicyObj.value("allowedRoots").toArray())) {
                    const QString cleaned = cleanConfigPath(root);
                    if (!cleaned.isEmpty()) {
                        roots.append(cleaned);
                    }
                }
                if (!roots.isEmpty()) {
                    aiToolAccessPolicy.allowedRoots = roots;
                }
            }

            aiToolAccessPolicy.allowFileWrite = toolPolicyObj.value("allowFileWrite").toBool(false);
            aiToolAccessPolicy.allowCommandExecution = toolPolicyObj.value("allowCommandExecution").toBool(false);
            aiToolAccessPolicy.commandWhitelist = jsonArrayToStringList(toolPolicyObj.value("commandWhitelist").toArray());
            aiToolAccessPolicy.autoGrantedTools = jsonArrayToStringList(toolPolicyObj.value("autoGrantedTools").toArray());
            if (aiToolAccessPolicy.autoGrantedTools.isEmpty()) {
                aiToolAccessPolicy.autoGrantedTools = {"read_text_file", "list_directory"};
            }
            aiToolAccessPolicy.commandTimeoutMs = clampInt(
                toolPolicyObj.value("commandTimeoutMs").toInt(5000),
                500,
                30000);
            aiToolAccessPolicy.maxWriteBytes = clampInt(
                toolPolicyObj.value("maxWriteBytes").toInt(64 * 1024),
                1,
                1024 * 1024);
        }

        // 读取行为策略
        if (aiRaw.contains("behaviorPolicy") && aiRaw.value("behaviorPolicy").isObject()) {
            const QJsonObject policyObj = aiRaw.value("behaviorPolicy").toObject();

            if (policyObj.contains("idleActionWhitelist")) {
                aiBehaviorPolicy.idleActionWhitelist = jsonArrayToStringList(policyObj.value("idleActionWhitelist").toArray());
            }
            if (policyObj.contains("touchActionWhitelist")) {
                aiBehaviorPolicy.touchActionWhitelist = jsonArrayToStringList(policyObj.value("touchActionWhitelist").toArray());
            }
            if (policyObj.contains("emotionActionWhitelist")) {
                aiBehaviorPolicy.emotionActionWhitelist = jsonArrayToStringList(policyObj.value("emotionActionWhitelist").toArray());
            }
            if (policyObj.contains("forbiddenActions")) {
                aiBehaviorPolicy.forbiddenActions = jsonArrayToStringList(policyObj.value("forbiddenActions").toArray());
            }

            if (policyObj.contains("triggers") && policyObj.value("triggers").isObject()) {
                const QJsonObject triggersObj = policyObj.value("triggers").toObject();
                if (triggersObj.contains("idleAction")) {
                    aiBehaviorPolicy.idleTrigger = parseTriggerConfig(triggersObj.value("idleAction").toObject(), 60000, 180000);
                }
                if (triggersObj.contains("emotion")) {
                    aiBehaviorPolicy.emotionTrigger = parseTriggerConfig(triggersObj.value("emotion").toObject(), 120000, 300000);
                }
                if (triggersObj.contains("proactiveChat")) {
                    aiBehaviorPolicy.proactiveChatTrigger = parseTriggerConfig(triggersObj.value("proactiveChat").toObject(), 180000, 300000);
                }
            }
        }

    }
    
    qDebug() << "配置文件加载成功:" << configPath;
    return true;
}
