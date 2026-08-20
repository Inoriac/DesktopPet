#include "identity_baseline.h"

#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kMaxTraits = 16;
constexpr int kMaxTraitNameLength = 64;
constexpr int kMaxSpeakingStyleLength = 512;

bool isTraitNameValid(const QString& name) {
    if (name.isEmpty() || name.size() > kMaxTraitNameLength) {
        return false;
    }
    for (const QChar character : name) {
        if (!character.isLetterOrNumber()
            && character != QLatin1Char('_')
            && character != QLatin1Char('-')) {
            return false;
        }
    }
    return true;
}

} // namespace

IdentityBaseline IdentityBaseline::defaults() {
    IdentityBaseline baseline;
    baseline.traits = {
        {QStringLiteral("initiative"), 0.35},
        {QStringLiteral("openness"), 0.60},
        {QStringLiteral("sociability"), 0.45}
    };
    baseline.speakingStyle = QStringLiteral("自然、简短、重视真实经历");
    return baseline;
}

IdentityBaseline IdentityBaseline::fromJson(const QJsonObject& object) {
    IdentityBaseline baseline = defaults();
    const int schemaVersion = object.value(QStringLiteral("schemaVersion")).toInt(1);
    if (schemaVersion != 1) {
        return baseline;
    }

    baseline.schemaVersion = schemaVersion;
    const QJsonValue traitsValue = object.value(QStringLiteral("traits"));
    if (traitsValue.isObject()) {
        QMap<QString, double> parsedTraits = baseline.traits;
        const QJsonObject traitsObject = traitsValue.toObject();
        for (auto it = traitsObject.constBegin();
             it != traitsObject.constEnd() && parsedTraits.size() < kMaxTraits;
             ++it) {
            const QString name = it.key().trimmed().toLower();
            const double value = it.value().toDouble(std::numeric_limits<double>::quiet_NaN());
            if (isTraitNameValid(name) && std::isfinite(value)) {
                parsedTraits.insert(name, std::clamp(value, 0.0, 1.0));
            }
        }
        baseline.traits = parsedTraits;
    }

    const QString speakingStyle = object.value(QStringLiteral("speakingStyle"))
                                      .toString()
                                      .simplified()
                                      .left(kMaxSpeakingStyleLength);
    if (!speakingStyle.isEmpty()) {
        baseline.speakingStyle = speakingStyle;
    }

    const double anchorStrength = object.value(QStringLiteral("anchorStrength"))
                                      .toDouble(baseline.anchorStrength);
    if (std::isfinite(anchorStrength)) {
        baseline.anchorStrength = std::clamp(anchorStrength, 0.0, 1.0);
    }
    return baseline;
}

QJsonObject IdentityBaseline::toJson() const {
    QJsonObject traitsObject;
    for (auto it = traits.constBegin(); it != traits.constEnd(); ++it) {
        traitsObject.insert(it.key(), std::clamp(it.value(), 0.0, 1.0));
    }

    QJsonObject object;
    object.insert(QStringLiteral("schemaVersion"), schemaVersion);
    object.insert(QStringLiteral("traits"), traitsObject);
    object.insert(QStringLiteral("speakingStyle"), speakingStyle);
    object.insert(QStringLiteral("anchorStrength"), std::clamp(anchorStrength, 0.0, 1.0));
    return object;
}
