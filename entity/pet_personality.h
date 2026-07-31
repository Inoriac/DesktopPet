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
    // —— 既有：提醒行为（PetReminderManager 消费） ——
    double forgetProbability{}; // 遗忘概率
    int randomVariance{};   // 提醒时间随机偏差
    QStringList reminderPhrases;    // 正常提醒语句
    QStringList forgetPhrases;      // 忘记后的语句
    // —— 新增：LLM 人设（ContextBuilder 渲染系统提示词时消费） ——
    QString gender;            // female/male/neutral，供 {{gender}} 与未来统一屏幕聊天
    QString tone;              // 一句话语气标签，如「温和、淡定」
    QStringList traits;        // 性格特质，渲染为 {{persona_traits}}
    QString speakingStyle;     // 说话风格说明，渲染为 {{speaking_style}}
    QStringList catchphrases;  // 口头禅，渲染为 {{catchphrases}}
    QStringList extraDirectives; // 可追加指令，渲染为 {{extra_directives}}

    PetPersonality() = default;
    explicit PetPersonality(const QString& name) : name(name){}

    // 从 Json 中加载性格定义
    static PetPersonality fromJson(const QJsonObject& obj);

    /// 保存当前性格
    QJsonObject toJson() const;
};



#endif // PET_PERSONALITY_H