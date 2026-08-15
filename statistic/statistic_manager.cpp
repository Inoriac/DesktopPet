//
// Created by Inoriac on 2025/10/27.
// 统计管理器 - 基于事件驱动的解耦统计系统
//

#include "statistic_manager.h"
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCoreApplication>

StatisticManager & StatisticManager::getInstance() {
    static StatisticManager instance;
    return instance;
}

StatisticManager::~StatisticManager() {
    saveStatistics();

    if (autoSaveTimer) {
        autoSaveTimer->stop();
        delete autoSaveTimer;
    }

    if (runtimeUpdateTimer) {
        runtimeUpdateTimer->stop();
        delete runtimeUpdateTimer;
    }

    petStatisticsMap.clear();
}

void StatisticManager::initialize(const QString &savePath, int autoSaveIntervalSec) {
    {
        {
            QMutexLocker locker(&dataMutex);

            eventSlots.clear();

            filePath = savePath;
            autoSaveInterval = autoSaveIntervalSec;

            QDir saveDir = QFileInfo(savePath).dir();
            if (!saveDir.exists()) {
                saveDir.mkpath(".");
            }

            // 初始化定时器
            if (!autoSaveTimer) {
                autoSaveTimer = new QTimer(this);
                connect(autoSaveTimer, &QTimer::timeout, this, &StatisticManager::onAutoSaveTimer);
            }

            if (!runtimeUpdateTimer) {
                runtimeUpdateTimer = new QTimer(this);
                connect(runtimeUpdateTimer, &QTimer::timeout, this, &StatisticManager::onRuntimeUpdateTimer);
            }
            runtimeUpdateTimer->start(1000);

            // 启用自动保存
            if (autoSaveEnabled && autoSaveInterval > 0) {
                autoSaveTimer->start(autoSaveInterval * 1000);
            }
        }

        // 加载已有数据
        loadStatistics();
    }

    // 注册事件监听
    // 启动
    registerEventSlot(StatisticEventType::PET_START, [this](const StatisticEvent& event) {
        PetStatistics snapshot;
        {
            QMutexLocker locker(&dataMutex);
            ensurePetStatistics(event.petName);
            PetStatistics& stats = petStatisticsMap[event.petName];
            stats.startTime = QDateTime::currentDateTime();
            stats.lastActiveTime = stats.startTime;
            stats.sessionRuntimeMs = 0;
            stats.sessionCount++;
            stats.isRunning = true;
            snapshot = stats;
        }
        emit statisticsUpdated(event.petName, snapshot);
    });

    // 关闭
    registerEventSlot(StatisticEventType::PET_STOP, [this](const StatisticEvent& event) {
        PetStatistics snapshot;
        {
            QMutexLocker locker(&dataMutex);
            ensurePetStatistics(event.petName);
            PetStatistics& stats = petStatisticsMap[event.petName];
            if (stats.startTime.isValid()) {
                const qint64 duration = stats.startTime.msecsTo(QDateTime::currentDateTime());
                stats.sessionRuntimeMs = duration;
                stats.totalRuntimeMs += duration;
                stats.startTime = QDateTime();
                stats.isRunning = false;
            }
            stats.lastActiveTime = QDateTime::currentDateTime();
            snapshot = stats;
        }
        emit statisticsUpdated(event.petName, snapshot);
    });

    // 触摸
    registerEventSlot(StatisticEventType::BODY_PART_TOUCH, [this](const StatisticEvent& event) {
        PetStatistics snapshot;
        {
            QMutexLocker locker(&dataMutex);
            ensurePetStatistics(event.petName);
            PetStatistics& stats = petStatisticsMap[event.petName];
            stats.lastActiveTime = QDateTime::currentDateTime();
            stats.touchAreaCount[event.areaName] += 1;
            snapshot = stats;
        }
        emit statisticsUpdated(event.petName, snapshot);
    });

    //情绪
    // registerEventSlot(StatisticEventType::EMOTION_INTERACTION, [this](const StatisticEvent& event) {
    //     // TODO: 情绪互动暂不处理,待情绪系统完善后进行实现
    // });
}

void StatisticManager::emitStatisticEvent(const StatisticEvent &event) {
    QVector<std::function<void(const StatisticEvent&)>> slotsToCall;

    // 避免阻塞
    {
        QMutexLocker locker(&dataMutex);

        if (eventSlots.contains(event.type))
            slotsToCall = eventSlots[event.type]; // 拷贝一份
    }

    // 事件分发
    // TODO：可能需要修改为异步，避免读写导致的阻塞
    for (auto &slot : slotsToCall) {
        slot(event);
    }

    emit statisticEventOccurred(event);
}

void StatisticManager::recordPetStart(const QString& petName) {
    if(petName.isEmpty()){
        qDebug() << "pet name is empty!";
        return;
    }
    StatisticEvent event = StatisticEvent(StatisticEventType::PET_START, petName, {});
    emitStatisticEvent(event);
}

void StatisticManager::recordPetStop(const QString& petName) {
    if(petName.isEmpty()){
        qDebug() << "pet name is empty!";
        return;
    }
    StatisticEvent event = StatisticEvent(StatisticEventType::PET_STOP, petName, {});
    emitStatisticEvent(event);

}

void StatisticManager::recordTouchInteraction(const QString& petName, const QString& areaName) {
    if(petName.isEmpty()){
        qDebug() << "pet name is empty!";
        return;
    }
    StatisticEvent event = StatisticEvent(StatisticEventType::BODY_PART_TOUCH, petName, areaName);
    emitStatisticEvent(event);
}

void StatisticManager::recordLlmUsage(const QString& petName, const LlmUsage& usage) {
    const QString effectivePetName = petName.isEmpty() ? "AI_GLOBAL" : petName;

    PetStatistics snapshot;
    {
        QMutexLocker locker(&dataMutex);
        ensurePetStatistics(effectivePetName);
        PetStatistics& stats = petStatisticsMap[effectivePetName];

        stats.llmCallCount += 1;
        stats.lastActiveTime = QDateTime::currentDateTime();
        stats.llmPromptTokens += usage.promptTokens;
        stats.llmCompletionTokens += usage.completionTokens;
        stats.llmTotalTokens += usage.totalTokens;
        stats.llmReasoningTokens += usage.reasoningTokens;
        stats.llmCachedTokens += usage.cachedTokens;
        stats.llmPromptCacheHitTokens += usage.promptCacheHitTokens;
        stats.llmPromptCacheMissTokens += usage.promptCacheMissTokens;
        snapshot = stats;
    }

    emit statisticsUpdated(effectivePetName, snapshot);
}

std::optional<PetStatistics> StatisticManager::getPetStatistics(const QString& petName)
{
    QMutexLocker locker(&dataMutex);
    const auto it = petStatisticsMap.constFind(petName);
    if (it == petStatisticsMap.constEnd()) return std::nullopt;
    return it.value();
}

QHash<QString, PetStatistics> StatisticManager::getAllPetStatistics()
{
    QMutexLocker locker(&dataMutex);
    return petStatisticsMap;
}

void StatisticManager::saveStatistics()
{
    QMutexLocker locker(&dataMutex);
    saveToFile();
}

void StatisticManager::loadStatistics()
{
    QMutexLocker locker(&dataMutex);
    loadFromFile();
}

void StatisticManager::clearStatistics(const QString &petName) {
    QMutexLocker locker(&dataMutex);

    if (!petName.isEmpty()) {
        if (petStatisticsMap.contains(petName)) {
            petStatisticsMap.remove(petName);
        }
    } else {
        petStatisticsMap.clear();
    }

    saveToFile();
}

void StatisticManager::setAutoSaveEnabled(bool enabled)
{
    QMutexLocker locker(&dataMutex);
    autoSaveEnabled = enabled;

    if (enabled && autoSaveInterval > 0) {
        autoSaveTimer->start(autoSaveInterval * 1000);
    } else {
        autoSaveTimer->stop();
    }
}

void StatisticManager::setAutoSaveInterval(int seconds)
{
    QMutexLocker locker(&dataMutex);
    autoSaveInterval = seconds;

    if (autoSaveEnabled && autoSaveInterval > 0) {
        autoSaveTimer->start(autoSaveInterval * 1000);
    }
}

void StatisticManager::onAutoSaveTimer() {
    saveStatistics();
    qDebug() << "Statistics auto-saved at" << QDateTime::currentDateTime().toString();
}

void StatisticManager::onRuntimeUpdateTimer() {
    const QDateTime now = QDateTime::currentDateTime();
    QMutexLocker locker(&dataMutex);
    for (PetStatistics& stats : petStatisticsMap) {
        if (stats.isRunning && stats.startTime.isValid()) {
            stats.sessionRuntimeMs = stats.startTime.msecsTo(now);
        }
    }

    // emit statisticsUpdated(); // 通知 UI 或存储模块
}

void StatisticManager::ensurePetStatistics(const QString &petName) {
    if (!petStatisticsMap.contains(petName)) {
        petStatisticsMap.insert(petName, PetStatistics(petName));
    }
}

QString StatisticManager::eventTypeToString(StatisticEventType type) {
    switch (type) {
        case StatisticEventType::PET_START: return "pet_start";
        case StatisticEventType::PET_STOP: return "pet_stop";
        case StatisticEventType::BODY_PART_TOUCH: return "body_part_touch";
        case StatisticEventType::EMOTION_INTERACTION: return "emotion_interaction";
        default: return "unknown";
    }
}

void StatisticManager::saveToFile() {
    if (filePath.isEmpty()) {
        return;
    }

    QSaveFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        const QByteArray payload = QJsonDocument(statisticsToJson()).toJson();
        if (file.write(payload) != payload.size() || !file.commit()) {
            qWarning() << "Failed to save statistics:" << file.errorString();
        }
    } else {
        qWarning() << "Failed to open statistics file:" << file.errorString();
    }
}

void StatisticManager::loadFromFile() {
    if (filePath.isEmpty()) {
        return;
    }

    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();

        if (!doc.isNull()) {
            jsonToStatistics(doc.object());
        }
    }
}

QJsonObject StatisticManager::statisticsToJson() {
    QJsonObject root;
    QJsonArray petsArray;

    for (auto it = petStatisticsMap.begin(); it != petStatisticsMap.end(); ++it) {
        const PetStatistics& stats = it.value();

        QJsonObject petObj;
        petObj["petName"] = stats.petName;
        petObj["sessionCount"] = stats.sessionCount;
        petObj["lastActiveTime"] = stats.lastActiveTime.toString(Qt::ISODate);
        petObj["totalRuntimeMs"] = stats.totalRuntimeMs;
        petObj["sessionRuntimeMs"] = stats.sessionRuntimeMs;
        petObj["llmCallCount"] = static_cast<qint64>(stats.llmCallCount);
        petObj["llmPromptTokens"] = static_cast<qint64>(stats.llmPromptTokens);
        petObj["llmCompletionTokens"] = static_cast<qint64>(stats.llmCompletionTokens);
        petObj["llmTotalTokens"] = static_cast<qint64>(stats.llmTotalTokens);
        petObj["llmReasoningTokens"] = static_cast<qint64>(stats.llmReasoningTokens);
        petObj["llmCachedTokens"] = static_cast<qint64>(stats.llmCachedTokens);
        petObj["llmPromptCacheHitTokens"] = static_cast<qint64>(stats.llmPromptCacheHitTokens);
        petObj["llmPromptCacheMissTokens"] = static_cast<qint64>(stats.llmPromptCacheMissTokens);

        // 触摸区域统计
        QJsonObject touchAreaObj;
        for (auto it2 = stats.touchAreaCount.begin(); it2 != stats.touchAreaCount.end(); ++it2) {
            touchAreaObj[it2.key()] = it2.value();
        }
        petObj["touchAreaCount"] = touchAreaObj;

        petsArray.append(petObj);
    }

    root["pets"] = petsArray;
    root["version"] = "1.0";
    root["lastSaved"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    return root;
}

void StatisticManager::jsonToStatistics(const QJsonObject &json) {
    QJsonArray petsArray = json["pets"].toArray();

    for (const QJsonValue& value : petsArray) {
        QJsonObject petObj = value.toObject();
        QString petName = petObj["petName"].toString();

        PetStatistics stats(petName);
        stats.sessionCount = petObj["sessionCount"].toInt();
        stats.lastActiveTime = QDateTime::fromString(petObj["lastActiveTime"].toString(), Qt::ISODate);
        stats.totalRuntimeMs = petObj["totalRuntimeMs"].toVariant().toLongLong();
        stats.sessionRuntimeMs = petObj["sessionRuntimeMs"].toVariant().toLongLong();
        stats.llmCallCount = petObj["llmCallCount"].toVariant().toLongLong();
        stats.llmPromptTokens = petObj["llmPromptTokens"].toVariant().toLongLong();
        stats.llmCompletionTokens = petObj["llmCompletionTokens"].toVariant().toLongLong();
        stats.llmTotalTokens = petObj["llmTotalTokens"].toVariant().toLongLong();
        stats.llmReasoningTokens = petObj["llmReasoningTokens"].toVariant().toLongLong();
        stats.llmCachedTokens = petObj["llmCachedTokens"].toVariant().toLongLong();
        stats.llmPromptCacheHitTokens = petObj["llmPromptCacheHitTokens"].toVariant().toLongLong();
        stats.llmPromptCacheMissTokens = petObj["llmPromptCacheMissTokens"].toVariant().toLongLong();

        // 加载触摸区域统计
        QJsonObject touchAreaObj = petObj["touchAreaCount"].toObject();
        for (auto it = touchAreaObj.begin(); it != touchAreaObj.end(); ++it) {
            stats.touchAreaCount[it.key()] = it.value().toInt();
        }

        petStatisticsMap[petName] = stats;
    }
}
