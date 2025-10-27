//
// Created by Inoriac on 2025/10/27.
// 统计系统数据类型定义
//

#ifndef DESKTOP_PET_STATISTIC_TYPES_H
#define DESKTOP_PET_STATISTIC_TYPES_H

#include <QString>
#include <QDateTime>
#include <QHash>

// 统计事件类型枚举
enum class StatisticEventType {
    PET_START,          // 桌宠启动
    PET_STOP,           // 桌宠停止
    BODY_PART_TOUCH,    // 身体部位触摸
    EMOTION_INTERACTION, // 情绪互动
};

// 统计事件数据结构
struct StatisticEvent {
    StatisticEventType type;
    QString petName;            // 桌宠名称
    QString areaName;
    QDateTime timestamp;        // 事件时间戳

    StatisticEvent(StatisticEventType t, const QString& detail, const QString& name)
    : type(t), petName(name) ,areaName(detail) ,timestamp(QDateTime::currentDateTime()) {}
};

// 统计数据结构(持久化)
struct PetStatistics {
    QString petName;            // 桌宠名称
    bool isRunning = false;

    // 运行时长统计
    QDateTime startTime;            // 开始运行时间
    QDateTime lastActiveTime;       // 最后运行时间
    qint64 totalRuntimeMs = 0;      // 总运行时长
    qint64 sessionRuntimeMs = 0;    // 当前会话运行时长

    // 交互统计
    QHash<QString, int> touchAreaCount;     // 触摸区域统计(areaName -> count)

    // 会话统计 - 使用记录
    int sessionCount = 0;            // 使用次数

    PetStatistics(const QString& name) : petName(name) {}
};

#endif //DESKTOP_PET_STATISTIC_TYPES_H