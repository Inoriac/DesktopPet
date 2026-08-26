#include "persona_projector.h"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>

#include "ai/integration/emotion_state_provider.h"
#include "ai/runtime/runtime_types.h"
#include "sqlite_identity_repository.h"

namespace {

QString traitDescription(const QString& name, double value) {
    if (name == QStringLiteral("sociability")) {
        return value < 0.34 ? QStringLiteral("偏内敛")
            : value > 0.66 ? QStringLiteral("乐于交流")
                           : QStringLiteral("交流适度");
    }
    if (name == QStringLiteral("initiative")) {
        return value < 0.34 ? QStringLiteral("不主动打扰")
            : value > 0.66 ? QStringLiteral("较主动")
                           : QStringLiteral("适时主动");
    }
    if (name == QStringLiteral("openness")) {
        return value < 0.34 ? QStringLiteral("谨慎稳妥")
            : value > 0.66 ? QStringLiteral("开放好奇")
                           : QStringLiteral("稳健开放");
    }
    return {};
}

QString tendencyDescription(const QString& name, double value, bool relationship) {
    if (std::abs(value) < 1e-9) return {};
    const QString prefix = relationship ? QStringLiteral("面对主人时") : QString();
    if (name == QStringLiteral("sociability")) {
        return prefix + (value > 0.0 ? QStringLiteral("更乐于交流")
                                    : QStringLiteral("更偏内敛"));
    }
    if (name == QStringLiteral("initiative")) {
        return prefix + (value > 0.0 ? QStringLiteral("更愿意主动表达")
                                    : QStringLiteral("更少主动打扰"));
    }
    if (name == QStringLiteral("openness")) {
        return prefix + (value > 0.0 ? QStringLiteral("更开放好奇")
                                    : QStringLiteral("更谨慎稳妥"));
    }
    return {};
}

QString emotionDescription(EmotionType emotion) {
    switch (emotion) {
    case EmotionType::Joy:
        return QStringLiteral("当前语气自然愉快");
    case EmotionType::Sadness:
        return QStringLiteral("当前语气温和克制");
    case EmotionType::Anger:
        return QStringLiteral("当前语气简短平稳");
    case EmotionType::Fear:
        return QStringLiteral("当前语气谨慎清晰");
    case EmotionType::Surprise:
        return QStringLiteral("当前语气略带惊喜");
    case EmotionType::Neutral:
        return {};
    }
    return {};
}

void appendDescription(QStringList& descriptions, const QString& description) {
    if (!description.isEmpty() && !descriptions.contains(description)) {
        descriptions.append(description);
    }
}

} // namespace

PersonaProjector::PersonaProjector(
    IdentityBaseline baseline,
    PersonalityPolicy policy,
    const SqliteIdentityRepository* repository,
    const EmotionStateProvider* emotionProvider)
    : m_baseline(std::move(baseline))
    , m_policy(sanitizePersonalityPolicy(policy))
    , m_repository(repository)
    , m_emotionProvider(emotionProvider) {}

PersonaProjection PersonaProjector::project(
    const RuntimeSnapshot& snapshot,
    const InteractionContext& context) const {
    IdentityBaseline effectiveBaseline = m_baseline;
    QStringList dynamicDescriptions;
    QString selfNarrative;

    if (m_repository && !snapshot.profileId.isEmpty()) {
        if (snapshot.personalityVersion.has_value()) {
            const auto personality = m_repository->personalityAt(
                snapshot.profileId, *snapshot.personalityVersion);
            if (personality.isOk() && personality.value().has_value()) {
                const PersonalitySnapshot& state = *personality.value();
                effectiveBaseline = state.baseline;
                for (auto it = state.tendencies.cbegin();
                     it != state.tendencies.cend(); ++it) {
                    const double baselineValue = effectiveBaseline.traits.value(it.key(), 0.5);
                    effectiveBaseline.traits.insert(
                        it.key(), std::clamp(baselineValue + it.value(), 0.0, 1.0));
                    appendDescription(
                        dynamicDescriptions,
                        tendencyDescription(it.key(), it.value(), false));
                }
            }
        }
        if (snapshot.relationshipVersion.has_value()
            && !snapshot.subjectId.isEmpty()) {
            const auto relationship = m_repository->relationshipAt(
                snapshot.profileId,
                snapshot.subjectId,
                *snapshot.relationshipVersion);
            if (relationship.isOk() && relationship.value().has_value()) {
                for (auto it = relationship.value()->tendencies.cbegin();
                     it != relationship.value()->tendencies.cend(); ++it) {
                    appendDescription(
                        dynamicDescriptions,
                        tendencyDescription(it.key(), it.value(), true));
                }
            }
        }
        if (snapshot.selfModelVersion.has_value()) {
            const auto selfModel = m_repository->selfModelAt(
                snapshot.profileId, *snapshot.selfModelVersion);
            if (selfModel.isOk() && selfModel.value().has_value()) {
                selfNarrative = selfModel.value()->narrative.simplified();
            }
        }
    }

    if (m_emotionProvider && !snapshot.profileId.isEmpty()) {
        const ProvidedEmotionSnapshot emotion = m_emotionProvider->currentSnapshot(
            snapshot.profileId,
            context.at.isValid() ? context.at.toUTC() : QDateTime::currentDateTimeUtc());
        if (emotion.schemaVersion == 1
            && emotion.profileId == snapshot.profileId
            && !emotion.neutralFallback
            && std::isfinite(emotion.value.intensity)
            && emotion.value.intensity > 0.0) {
            appendDescription(
                dynamicDescriptions, emotionDescription(emotion.value.active));
        }
    }

    PersonaProjection projection = projectBaseline(
        effectiveBaseline, context.petName);
    QString traits = projection.promptSlots.value(QStringLiteral("persona_traits"));
    if (!dynamicDescriptions.isEmpty()) {
        traits += QStringLiteral("当前倾向：%1。").arg(
            dynamicDescriptions.join(QStringLiteral("、")));
    }
    if (!selfNarrative.isEmpty()) {
        traits += QStringLiteral("自我认识：%1").arg(selfNarrative);
    }
    projection.promptSlots.insert(QStringLiteral("persona_traits"), traits);
    for (auto it = effectiveBaseline.traits.cbegin();
         it != effectiveBaseline.traits.cend(); ++it) {
        if (std::isfinite(it.value())) {
            projection.behaviorParameters.insert(
                it.key(), std::clamp(it.value(), 0.0, 1.0));
        }
    }
    for (auto it = projection.promptSlots.begin();
         it != projection.promptSlots.end(); ++it) {
        it.value() = it.value().simplified().left(m_policy.maxPromptSlotChars);
    }
    return projection;
}

PersonaProjection PersonaProjector::projectBaseline(const IdentityBaseline& baseline,
                                                     const QString& petName) const {
    PersonaProjection projection;
    const QString normalizedPetName = petName.simplified().left(128);
    projection.promptSlots.insert(
        QStringLiteral("pet_name"),
        normalizedPetName.isEmpty() ? QStringLiteral("桌宠") : normalizedPetName);

    QStringList descriptions;
    for (auto it = baseline.traits.constBegin(); it != baseline.traits.constEnd(); ++it) {
        if (!std::isfinite(it.value())) {
            continue;
        }
        const QString description = traitDescription(it.key(), std::clamp(it.value(), 0.0, 1.0));
        if (!description.isEmpty()) {
            descriptions.append(description);
        }
    }
    projection.promptSlots.insert(
        QStringLiteral("persona_traits"),
        descriptions.isEmpty()
            ? QString()
            : QStringLiteral("性格倾向：%1。").arg(descriptions.join(QStringLiteral("、"))));
    projection.promptSlots.insert(
        QStringLiteral("speaking_style"), baseline.speakingStyle.simplified().left(512));
    return projection;
}
