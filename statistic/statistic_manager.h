//
// Created by Inoriac on 2025/10/27.
// 统计管理器 - 基于事件驱动的解耦统计系统
//

#ifndef DESKTOP_PET_STATISTIC_MANAGER_H
#define DESKTOP_PET_STATISTIC_MANAGER_H

#include <QObject>
#include <QTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMutex>
#include <functional>

#include "../include/statistic_types.h"

class StatisticManager : public QObject {
    Q_OBJECT

public:
    static StatisticManager& getInstance();

    // 禁用拷贝构造与赋值
    StatisticManager(const StatisticManager&) = delete;
    StatisticManager& operator=(const StatisticManager&) = delete;

    // 保存路径与自动保存间隔(秒)
    void initialize(const QString& savePath = "log/statistics.json",
                    int autoSaveInterval= 300);

    // 注册统计事件槽函数
    template<typename Func>
    void registerEventSlot(StatisticEventType eventType, Func&& slot);
    // 回调
    void emitStatisticEvent(const StatisticEvent& event);

    // 事件发送方法
    void recordPetStart(const QString& petName);
    void recordPetStop(const QString& petName);
    void recordTouchInteraction(const QString& petName, const QString& areaName);
    // void recordAnimationPlay(const QString& petName, const QString& animationName, qint64 durationMs = 0);

    // 数据查询接口
    PetStatistics* getPetStatistics(const QString& petName);
    QHash<QString, PetStatistics*> getAllPetStatistics();

    // 数据管理
    void saveStatistics();
    void loadStatistics();
    void clearStatistics(const QString& petName = "");  // 默认清空所有统计数据

    // 配置管理
    void setAutoSaveEnabled(bool enabled);
    void setAutoSaveInterval(int seconds);
    bool isAutoSaveEnabled() const { return autoSaveEnabled; }
    int getAutoSaveInterval() const { return autoSaveInterval; }

signals:
    // 统计事件信号
    void statisticEventOccurred(const StatisticEvent& event);

    // 统计数据更新信号
    void statisticsUpdated(const QString& petName, const PetStatistics& statistics);

private slots:
    void onAutoSaveTimer();
    void onRuntimeUpdateTimer();    // 用于实时显示运行时长

private:
    StatisticManager() = default;
    ~StatisticManager();

    // 内部方法
    void ensurePetStatistics(const QString& petName);   // 检测是否存在
    QString eventTypeToString(StatisticEventType type); // 类型转换

    // 持久化
    void saveToFile();
    void loadFromFile();
    QJsonObject statisticsToJson();
    void jsonToStatistics(const QJsonObject& json);

private:
    QHash<QString, PetStatistics*> petStatisticsMap;  // 桌宠统计数据
    QHash<StatisticEventType, QList<QMetaObject::Connection>> eventConnections; // 事件连接

    QString filePath;                                // 存储文件路径
    QTimer* autoSaveTimer{};                           // 自动保存定时器
    QTimer* runtimeUpdateTimer{};                      // 运行时长更新定时器

    bool autoSaveEnabled = true;                     // 是否启用自动保存
    int autoSaveInterval = 300;                      // 自动保存间隔(秒)

    QMutex dataMutex;                                // 数据访问互斥锁

    // 事件槽函数存储
    QHash<StatisticEventType, QList<std::function<void(const StatisticEvent&)>>> eventSlots;
};

// 模板方法实现
template<typename Func>
void StatisticManager::registerEventSlot(StatisticEventType eventType, Func&& slot) {
    QMutexLocker locker(&dataMutex);        // 获取锁，作用域结束时会自动释放
    eventSlots[eventType].append(std::forward<Func>(slot));
}

#endif //DESKTOP_PET_STATISTIC_MANAGER_H