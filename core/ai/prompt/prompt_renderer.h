//
// PromptRenderer — 把通用模版 + 性格预设装配成系统提示词
// v1 仅做扁平 {{slot}} 替换（不做条件/循环）；未提供的 slot 收敛为空。
// persona 自由文本（tone/speakingStyle/catchphrases/extraDirectives）过一遍 redactSecrets，
// 避免用户在 personalities.json 里混入密钥后被注入到系统提示词。
//

#ifndef DESKTOP_PET_PROMPT_RENDERER_H
#define DESKTOP_PET_PROMPT_RENDERER_H

#include <QMap>
#include <QString>

class PetPersonality;  // 前向声明，实现文件再包含完整定义

class PromptRenderer {
public:
    // 扁平 {{slot}} 替换：遍历 vars 逐个 replace，再清掉残留的 {{...}}。
    static QString render(const QString& body, const QMap<QString, QString>& vars);

    // 由性格预设 + 宠物名构造模版变量；列表型字段拼成自然句。
    static QMap<QString, QString> buildVariables(const PetPersonality& persona, const QString& petName);

    // 脱敏：把 api_key/password/token/secret 关键词替换为 [redacted]。
    // 与 ContextManager::sanitizeForLlm 共用同一份关键词，集中维护。
    static QString redactSecrets(const QString& text);
};

#endif // DESKTOP_PET_PROMPT_RENDERER_H
