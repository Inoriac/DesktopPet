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

    return obj;
}