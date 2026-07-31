//
// Created by Inoriac on 2025/11/5.
//

#include "pet_personality.h"

PetPersonality PetPersonality::fromJson(const QJsonObject& obj) {
    PetPersonality personality;
    personality.name = obj["name"].toString();
    personality.forgetProbability = obj["forgetProbability"].toDouble(0.2);
    personality.randomVariance = obj["randomVariance"].toInt(15);

    QJsonArray reminderArr = obj["reminderPhrases"].toArray();
    for (auto v : reminderArr) personality.reminderPhrases.append(v.toString());

    QJsonArray forgetArr = obj["forgetPhrases"].toArray();
    for (auto v : forgetArr) personality.forgetPhrases.append(v.toString());

    // LLM 人设字段（缺失时为空/默认，保证旧格式 personalities.json 仍可加载）
    personality.gender = obj["gender"].toString();
    personality.tone = obj["tone"].toString();
    personality.speakingStyle = obj["speakingStyle"].toString();

    const auto toStringList = [](const QJsonArray& array) {
        QStringList result;
        for (const QJsonValue& value : array) {
            const QString str = value.toString().trimmed();
            if (!str.isEmpty()) {
                result.append(str);
            }
        }
        return result;
    };

    personality.traits = toStringList(obj["traits"].toArray());
    personality.catchphrases = toStringList(obj["catchphrases"].toArray());
    personality.extraDirectives = toStringList(obj["extraDirectives"].toArray());

    return personality;
}

QJsonObject PetPersonality::toJson() const {
    QJsonObject obj;
    obj["name"] = name;
    obj["forgetProbability"] = forgetProbability;
    obj["randomVariance"] = randomVariance;

    QJsonArray reminderArr;
    for (const auto& s : reminderPhrases) reminderArr.append(s);
    obj["reminderPhrases"] = reminderArr;

    QJsonArray forgetArr;
    for (const auto& s : forgetPhrases) forgetArr.append(s);
    obj["forgetPhrases"] = forgetArr;

    obj["gender"] = gender;
    obj["tone"] = tone;
    obj["speakingStyle"] = speakingStyle;
    obj["traits"] = QJsonArray::fromStringList(traits);
    obj["catchphrases"] = QJsonArray::fromStringList(catchphrases);
    obj["extraDirectives"] = QJsonArray::fromStringList(extraDirectives);

    return obj;
}