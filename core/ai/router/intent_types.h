#ifndef DESKTOP_PET_INTENT_TYPES_H
#define DESKTOP_PET_INTENT_TYPES_H

#include <QJsonObject>
#include <QString>

enum class IntentRouteType {
    DirectReply,
    DirectToolCall,
    NeedLLM,
    NeedClarification,
    Rejected
};

inline QString intentRouteTypeToString(IntentRouteType type) {
    switch (type) {
    case IntentRouteType::DirectReply:
        return "DirectReply";
    case IntentRouteType::DirectToolCall:
        return "DirectToolCall";
    case IntentRouteType::NeedLLM:
        return "NeedLLM";
    case IntentRouteType::NeedClarification:
        return "NeedClarification";
    case IntentRouteType::Rejected:
        return "Rejected";
    }
    return "NeedLLM";
}

struct IntentRoute {
    IntentRouteType type = IntentRouteType::NeedLLM;
    QString reply;
    QString toolName;
    QJsonObject toolArguments;
    QString reason;
    double confidence = 0.0;

    static IntentRoute directReply(const QString& reply, const QString& reason = QString()) {
        IntentRoute route;
        route.type = IntentRouteType::DirectReply;
        route.reply = reply;
        route.reason = reason;
        route.confidence = 1.0;
        return route;
    }

    static IntentRoute directToolCall(const QString& toolName,
                                      const QJsonObject& arguments = {},
                                      const QString& reason = QString(),
                                      double confidence = 1.0) {
        IntentRoute route;
        route.type = IntentRouteType::DirectToolCall;
        route.toolName = toolName;
        route.toolArguments = arguments;
        route.reason = reason;
        route.confidence = confidence;
        return route;
    }

    static IntentRoute needLlm(const QString& reason = QString(), double confidence = 0.5) {
        IntentRoute route;
        route.type = IntentRouteType::NeedLLM;
        route.reason = reason;
        route.confidence = confidence;
        return route;
    }

    static IntentRoute needClarification(const QString& question, const QString& reason = QString()) {
        IntentRoute route;
        route.type = IntentRouteType::NeedClarification;
        route.reply = question;
        route.reason = reason;
        route.confidence = 1.0;
        return route;
    }

    static IntentRoute rejected(const QString& reason) {
        IntentRoute route;
        route.type = IntentRouteType::Rejected;
        route.reason = reason;
        route.reply = reason;
        route.confidence = 1.0;
        return route;
    }
};

#endif // DESKTOP_PET_INTENT_TYPES_H