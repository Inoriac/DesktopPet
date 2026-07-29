//
// 环境感知 Tools
//

#include "environment_tools.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkInterface>
#include <algorithm>
#include <QtGlobal>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace {
QJsonObject makeIntegerProperty(const QString& description, int defaultValue = 0) {
    QJsonObject obj;
    obj["type"] = "integer";
    obj["description"] = description;
    obj["default"] = defaultValue;
    return obj;
}

QString idleLevel(int idleSeconds) {
    if (idleSeconds < 60) {
        return "active";
    }
    if (idleSeconds < 5 * 60) {
        return "short_idle";
    }
    if (idleSeconds < 20 * 60) {
        return "idle";
    }
    return "long_idle";
}

QString powerStatusText(int status) {
    switch (status) {
    case 0: return "offline";
    case 1: return "charging_or_plugged_in";
    case 255: return "unknown";
    default: return "unknown";
    }
}
}  // namespace

int queryUserIdleSeconds() {
#ifdef Q_OS_WIN
    LASTINPUTINFO info;
    info.cbSize = sizeof(LASTINPUTINFO);
    if (!GetLastInputInfo(&info)) return -1;
    const DWORD idleMs = GetTickCount() - info.dwTime;
    return static_cast<int>(idleMs / 1000);
#elif defined(Q_OS_MACOS)
    // CGEventSourceSecondsSinceLastEventType：只读“距上次 HID 输入的秒数”，
    // 不读取输入内容、无需辅助功能权限。clamp 到 >=0。
    const CFTimeInterval idleSec = CGEventSourceSecondsSinceLastEventType(
        kCGEventSourceStateHIDSystemState, kCGAnyInputEventType);
    return (idleSec > 0.0) ? static_cast<int>(idleSec) : 0;
#else
    return -1; // 平台不支持
#endif
}

GetUserIdleStateTool::GetUserIdleStateTool()
    : AITool(
          "get_user_idle_state",
          "获取用户空闲状态。只返回距离上次键鼠输入的大致时间和活跃等级，不读取屏幕、窗口标题或输入内容。",
          ToolCategory::Query) {}

QJsonObject GetUserIdleStateTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    properties["idle_threshold_seconds"] = makeIntegerProperty("判断用户是否空闲的阈值，默认 300 秒", 300);
    schema["properties"] = properties;
    return schema;
}

ToolResult GetUserIdleStateTool::execute(const QJsonObject& params) {
    const int thresholdSeconds = qMax(1, params.value("idle_threshold_seconds").toInt(300));

    QJsonObject result;
    result["idle_threshold_seconds"] = thresholdSeconds;

    // 复用 queryUserIdleSeconds（Daydream 触发判定也直接调它），单一来源。
    const int idleSeconds = queryUserIdleSeconds();
    if (idleSeconds < 0) {
        result["supported"] = false;
        result["idle_seconds"] = -1;
        result["is_idle"] = false;
        result["level"] = "unknown";
        result["note"] = "当前平台暂未实现空闲状态检测";
    } else {
        result["supported"] = true;
        result["idle_seconds"] = idleSeconds;
        result["is_idle"] = idleSeconds >= thresholdSeconds;
        result["level"] = idleLevel(idleSeconds);
    }

    return ToolResult::ok(result);
}

GetBatteryStatusTool::GetBatteryStatusTool()
    : AITool(
          "get_battery_status",
          "获取电源和电池状态，用于低电量提醒。只读取系统电源摘要。",
          ToolCategory::Query) {}

QJsonObject GetBatteryStatusTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    schema["properties"] = QJsonObject{};
    return schema;
}

ToolResult GetBatteryStatusTool::execute(const QJsonObject& /*params*/) {
    QJsonObject result;

#ifdef Q_OS_WIN
    SYSTEM_POWER_STATUS status;
    if (!GetSystemPowerStatus(&status)) {
        return ToolResult::fail("无法获取电源状态");
    }

    const int percent = status.BatteryLifePercent == 255 ? -1 : static_cast<int>(status.BatteryLifePercent);
    result["supported"] = true;
    result["ac_line_status"] = powerStatusText(status.ACLineStatus);
    result["battery_percent"] = percent;
    result["battery_life_seconds"] = status.BatteryLifeTime == static_cast<DWORD>(-1)
        ? -1
        : static_cast<int>(status.BatteryLifeTime);
    result["is_charging_or_plugged_in"] = status.ACLineStatus == 1;
    result["is_low_battery"] = percent >= 0 && percent <= 20 && status.ACLineStatus != 1;
#else
    result["supported"] = false;
    result["note"] = "当前平台暂未实现电源状态检测";
#endif

    return ToolResult::ok(result);
}

GetNetworkStatusTool::GetNetworkStatusTool()
    : AITool(
          "get_network_status",
          "获取基础网络状态，用于判断是否适合联网查询。只返回网络接口摘要，不包含隐私流量内容。",
          ToolCategory::Query) {}

QJsonObject GetNetworkStatusTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    schema["properties"] = QJsonObject{};
    return schema;
}

ToolResult GetNetworkStatusTool::execute(const QJsonObject& /*params*/) {
    QJsonArray interfaces;
    bool hasUsableInterface = false;

    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : all) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        const bool isUp = flags.testFlag(QNetworkInterface::IsUp);
        const bool isRunning = flags.testFlag(QNetworkInterface::IsRunning);
        const bool isLoopback = flags.testFlag(QNetworkInterface::IsLoopBack);
        if (!isUp || isLoopback) {
            continue;
        }

        QJsonObject item;
        item["name"] = iface.humanReadableName();
        item["type"] = static_cast<int>(iface.type());
        item["is_running"] = isRunning;
        item["has_ip_address"] = !iface.addressEntries().isEmpty();
        interfaces.append(item);

        if (isRunning && !iface.addressEntries().isEmpty()) {
            hasUsableInterface = true;
        }
    }

    QJsonObject result;
    result["online_likely"] = hasUsableInterface;
    result["interface_count"] = interfaces.size();
    result["interfaces"] = interfaces;
    result["note"] = "该结果基于本机网络接口状态推断，不保证外网一定可达。";
    return ToolResult::ok(result);
}
