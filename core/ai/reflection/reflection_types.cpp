#include "reflection_types.h"

#include <QJsonDocument>

QByteArray PrivateRecordAad::toBytes() const {
    const QJsonObject object{
        {QStringLiteral("schemaVersion"), schemaVersion},
        {QStringLiteral("profileId"), profileId},
        {QStringLiteral("recordType"), recordType},
        {QStringLiteral("recordId"), recordId},
        {QStringLiteral("keyVersion"), keyVersion}
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString sleepDecisionToString(SleepDecision decision) {
    switch (decision) {
    case SleepDecision::Pending: return QStringLiteral("Pending");
    case SleepDecision::Commit: return QStringLiteral("Commit");
    case SleepDecision::Abort: return QStringLiteral("Abort");
    }
    return QStringLiteral("Pending");
}

std::optional<SleepDecision> sleepDecisionFromString(const QString& value) {
    if (value == QLatin1String("Pending")) return SleepDecision::Pending;
    if (value == QLatin1String("Commit")) return SleepDecision::Commit;
    if (value == QLatin1String("Abort")) return SleepDecision::Abort;
    return std::nullopt;
}

QString sleepSessionStateToString(SleepSessionState state) {
    switch (state) {
    case SleepSessionState::Snapshotting: return QStringLiteral("Snapshotting");
    case SleepSessionState::Consolidating: return QStringLiteral("Consolidating");
    case SleepSessionState::Journaling: return QStringLiteral("Journaling");
    case SleepSessionState::Committing: return QStringLiteral("Committing");
    case SleepSessionState::Sleeping: return QStringLiteral("Sleeping");
    case SleepSessionState::Cancelling: return QStringLiteral("Cancelling");
    case SleepSessionState::RolledBack: return QStringLiteral("RolledBack");
    case SleepSessionState::Completed: return QStringLiteral("Completed");
    }
    return QStringLiteral("Snapshotting");
}

std::optional<SleepSessionState> sleepSessionStateFromString(
    const QString& value) {
    if (value == QLatin1String("Snapshotting")) return SleepSessionState::Snapshotting;
    if (value == QLatin1String("Consolidating")) return SleepSessionState::Consolidating;
    if (value == QLatin1String("Journaling")) return SleepSessionState::Journaling;
    if (value == QLatin1String("Committing")) return SleepSessionState::Committing;
    if (value == QLatin1String("Sleeping")) return SleepSessionState::Sleeping;
    if (value == QLatin1String("Cancelling")) return SleepSessionState::Cancelling;
    if (value == QLatin1String("RolledBack")) return SleepSessionState::RolledBack;
    if (value == QLatin1String("Completed")) return SleepSessionState::Completed;
    return std::nullopt;
}
