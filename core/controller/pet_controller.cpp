#include "pet_controller.h"

#include <QRegularExpression>
#include <QUuid>

PetController::PetController(EmotionEngine* emotionEngine, QObject* parent)
    : QObject(parent),
      m_emotionEngine(emotionEngine) {}

bool PetController::recordTouch(const QString& part,
                                const QDateTime& nowUtc,
                                const QString& eventId) {
    const QString normalizedPart = part.trimmed().toLower().left(64);
    if (normalizedPart.isEmpty()) {
        return false;
    }

    AffectEvent event;
    event.kind = AffectEventKind::Touch;
    event.source = AffectSource::User;
    event.sourceId = QStringLiteral("touch:%1").arg(normalizedPart);
    event.goalCongruence = 0.60;
    event.novelty = 0.10;
    event.certainty = 1.0;
    event.controllability = 0.75;
    event.relevance = 0.80;
    event.agency = AffectAgency::User;
    event.outcome = AffectOutcome::Success;
    event.confidence = 1.0;
    return submit(event, nowUtc, makeEventId(QStringLiteral("touch"), eventId));
}

bool PetController::recordToolOutcome(const QString& toolName,
                                      bool success,
                                      const QDateTime& nowUtc,
                                      const QString& eventId) {
    const QString normalizedName = toolName.trimmed().left(96);
    if (normalizedName.isEmpty()) {
        return false;
    }

    AffectEvent event;
    event.kind = success ? AffectEventKind::ToolSucceeded : AffectEventKind::ToolFailed;
    event.source = AffectSource::Tool;
    event.sourceId = QStringLiteral("tool:%1").arg(normalizedName.toLower());
    event.goalCongruence = success ? 0.35 : -0.30;
    event.novelty = 0.05;
    event.certainty = 1.0;
    event.controllability = success ? 0.75 : 0.55;
    event.relevance = 0.45;
    event.agency = AffectAgency::System;
    event.outcome = success ? AffectOutcome::Success : AffectOutcome::Failure;
    event.confidence = 1.0;
    return submit(event, nowUtc, makeEventId(QStringLiteral("tool"), eventId));
}

bool PetController::recordTaskOutcome(const QString& taskId,
                                      bool success,
                                      const QDateTime& nowUtc,
                                      const QString& eventId) {
    const QString normalizedTask = taskId.trimmed().left(96);
    if (normalizedTask.isEmpty()) {
        return false;
    }

    AffectEvent event;
    event.kind = success ? AffectEventKind::TaskSucceeded : AffectEventKind::TaskFailed;
    event.source = AffectSource::System;
    event.sourceId = QStringLiteral("task:%1").arg(normalizedTask.toLower());
    event.goalCongruence = success ? 0.75 : -0.40;
    event.novelty = 0.10;
    event.certainty = 1.0;
    event.controllability = success ? 0.85 : 0.45;
    event.relevance = 0.75;
    event.agency = AffectAgency::Self;
    event.outcome = success ? AffectOutcome::Success : AffectOutcome::Failure;
    event.confidence = 1.0;
    return submit(event, nowUtc, makeEventId(QStringLiteral("task"), eventId));
}

bool PetController::recordExplicitFeedbackText(const QString& text,
                                               const QDateTime& nowUtc,
                                               const QString& eventId) {
    const QString normalized = text.simplified().left(512);
    if (normalized.isEmpty()) {
        return false;
    }

    static const QRegularExpression boundaryPattern(
        QStringLiteral("(?:不要|别这样|停止|关闭|删掉|删除|忘记|不允许|拒绝)"));
    static const QRegularExpression positivePattern(
        QStringLiteral("(?:谢谢你|做得好|干得好|我喜欢你|喜欢这个反应|很棒)"));
    static const QRegularExpression negativePattern(
        QStringLiteral("(?:我不喜欢|我讨厌|不喜欢这个反应)"));

    AffectEvent event;
    event.source = AffectSource::User;
    event.sourceId = QStringLiteral("explicit_feedback");
    event.certainty = 1.0;
    event.controllability = 0.5;
    event.agency = AffectAgency::User;
    event.confidence = 1.0;

    if (boundaryPattern.match(normalized).hasMatch()) {
        event.kind = AffectEventKind::UserCorrection;
        event.goalCongruence = 0.0;
        event.relevance = 0.0;
        event.outcome = AffectOutcome::Unknown;
    } else if (negativePattern.match(normalized).hasMatch()) {
        event.kind = AffectEventKind::ExplicitNegativeFeedback;
        event.goalCongruence = -0.25;
        event.relevance = 0.45;
        event.outcome = AffectOutcome::Failure;
    } else if (positivePattern.match(normalized).hasMatch()) {
        event.kind = AffectEventKind::ExplicitPositiveFeedback;
        event.goalCongruence = 0.80;
        event.novelty = 0.10;
        event.relevance = 0.90;
        event.outcome = AffectOutcome::Success;
    } else {
        return false;
    }

    return submit(event, nowUtc, makeEventId(QStringLiteral("feedback"), eventId));
}

bool PetController::submit(AffectEvent event,
                           const QDateTime& nowUtc,
                           const QString& eventId) {
    event.id = eventId;
    event.occurredAt = nowUtc;
    const bool accepted = m_emotionEngine && m_emotionEngine->submitEvent(event, nowUtc);
    if (accepted) {
        emit affectEventAccepted(event.id, event.kind);
    } else {
        emit affectEventIgnored(event.id, event.kind);
    }
    return accepted;
}

QString PetController::makeEventId(const QString& prefix, const QString& suppliedId) {
    const QString normalized = suppliedId.trimmed().left(128);
    if (!normalized.isEmpty()) {
        return normalized;
    }
    return QStringLiteral("%1:%2")
        .arg(prefix, QUuid::createUuid().toString(QUuid::WithoutBraces));
}
