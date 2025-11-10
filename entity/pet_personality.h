//
// Created by Inoriac on 2025/11/5.
//

#ifndef PET_PERSONALITY_H
#define PET_PERSONALITY_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>

class PetPersonality {
public:
    QString name;       // 性格名称
    double forgetProbability{}; // 遗忘概率
    int randomVariance{};   // 提醒时间随机偏差
    QStringList reminderPhrases;    // 正常提醒语句
    QStringList forgetPhrases;      // 忘记后的语句

    PetPersonality() = default;
    explicit PetPersonality(const QString& name) : name(name){}

    // 从 Json 中加载性格定义
    static PetPersonality fromJson(const QJsonObject& obj);

    /// 保存当前性格
    QJsonObject toJson() const;
};



#endif // PET_PERSONALITY_H