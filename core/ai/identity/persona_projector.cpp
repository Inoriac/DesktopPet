#include "persona_projector.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

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

} // namespace

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
