//
// Created by Inoriac on 2025/10/27.
// 统计管理器 - 基于事件驱动的解耦统计系统
//

#include "statistic_manager.h"
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
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

    // 清理统计数据
    qDeleteAll(petStatisticsMap);
    petStatisticsMap.clear();
}

void StatisticManager::initialize(const QString &savePath, int autoSaveIntervalSec) {
    {
        {
            QMutexLocker locker(&dataMutex);

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
        ensurePetStatistics(event.petName);
        auto* stats = petStatisticsMap[event.petName];

        stats->startTime = QDateTime::currentDateTime();
        stats->sessionCount++;
        stats->isRunning = true;
    });

    // 关闭
    registerEventSlot(StatisticEventType::PET_STOP, [this](const StatisticEvent& event) {
        auto* stats = petStatisticsMap[event.petName];

        if (stats->startTime.isValid()) {
            qint64 duration = stats->startTime.msecsTo(QDateTime::currentDateTime());
            stats->sessionRuntimeMs += duration;
            stats->totalRuntimeMs += duration;
            stats->startTime = QDateTime(); // 清空，标记会话结束
            stats->isRunning = false;
        }
    });

    // 触摸
    registerEventSlot(StatisticEventType::BODY_PART_TOUCH, [this](const StatisticEvent& event) {
        auto* stats = petStatisticsMap[event.petName];

        stats->lastActiveTime = QDateTime::currentDateTime();

        if (!stats->touchAreaCount.contains(event.areaName)) {
            stats->touchAreaCount.insert(event.areaName, 0);
        }
        stats->touchAreaCount[event.areaName] += 1;
        qDebug() << "Touch:" << event.areaName << " for " << stats->petName << " area";
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
    StatisticEvent event = StatisticEvent(StatisticEventType::PET_START, petName, {});
    emitStatisticEvent(event);
}

void StatisticManager::recordPetStop(const QString& petName) {
    StatisticEvent event = StatisticEvent(StatisticEventType::PET_STOP, petName, {});
    emitStatisticEvent(event);
}

void StatisticManager::recordTouchInteraction(const QString& petName, const QString& areaName) {
    StatisticEvent event = StatisticEvent(StatisticEventType::BODY_PART_TOUCH, petName, areaName);
    emitStatisticEvent(event);
}

PetStatistics* StatisticManager::getPetStatistics(const QString& petName)
{
    QMutexLocker locker(&dataMutex);
    return petStatisticsMap.value(petName, nullptr);
}

QHash<QString, PetStatistics*> StatisticManager::getAllPetStatistics()
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
            delete petStatisticsMap[petName];
            petStatisticsMap.remove(petName);
        }
    } else {
        qDeleteAll(petStatisticsMap);
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

void StatisticManager::statisticEventOccurred(const StatisticEvent &event) {
}

void StatisticManager::statisticsUpdated(const QString &petName, const PetStatistics &statistics) {
}

void StatisticManager::onAutoSaveTimer() {
    saveStatistics();
    qDebug() << "Statistics auto-saved at" << QDateTime::currentDateTime().toString();
}

void StatisticManager::onRuntimeUpdateTimer() {
    QList<PetStatistics*> activePets;
    {
        QMutexLocker locker(&dataMutex);
        for (auto* stats : petStatisticsMap) {
            if (stats->isRunning && stats->startTime.isValid()) {
                activePets.append(stats);
            }
        }
    }

    QDateTime now = QDateTime::currentDateTime();

    for (auto* stats : activePets) {
        stats->sessionRuntimeMs = stats->startTime.msecsTo(now);
    }

    // emit statisticsUpdated(); // 通知 UI 或存储模块
}

void StatisticManager::ensurePetStatistics(const QString &petName) {
    if (!petStatisticsMap.contains(petName)) {
        petStatisticsMap[petName] = new PetStatistics(petName);
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
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonDocument doc(statisticsToJson());
        file.write(doc.toJson());
        file.close();
    }
}

void StatisticManager::loadFromFile() {
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
        PetStatistics* stats = it.value();

        QJsonObject petObj;
        petObj["petName"] = stats->petName;
        petObj["sessionCount"] = stats->sessionCount;
        petObj["lastActiveTime"] = stats->lastActiveTime.toString(Qt::ISODate);
        petObj["totalRuntimeMs"] = stats->totalRuntimeMs;
        petObj["sessionRuntimeMs"] = stats->sessionRuntimeMs;

        // 触摸区域统计
        QJsonObject touchAreaObj;
        for (auto it2 = stats->touchAreaCount.begin(); it2 != stats->touchAreaCount.end(); ++it2) {
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

        PetStatistics* stats = new PetStatistics(petName);
        stats->sessionCount = petObj["sessionCount"].toInt();
        stats->lastActiveTime = QDateTime::fromString(petObj["lastActiveTime"].toString(), Qt::ISODate);
        stats->totalRuntimeMs = petObj["totalRuntimeMs"].toVariant().toLongLong();
        stats->sessionRuntimeMs = petObj["sessionRuntimeMs"].toVariant().toLongLong();

        // 加载触摸区域统计
        QJsonObject touchAreaObj = petObj["touchAreaCount"].toObject();
        for (auto it = touchAreaObj.begin(); it != touchAreaObj.end(); ++it) {
            stats->touchAreaCount[it.key()] = it.value().toInt();
        }

        petStatisticsMap[petName] = stats;
    }
}
