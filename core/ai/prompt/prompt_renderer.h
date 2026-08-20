//
// PromptRenderer — 使用有界人格投影装配系统提示词
// v1 仅做扁平 {{slot}} 替换（不做条件/循环）；未提供的 slot 收敛为空。
// 所有 slot 值在注入前统一脱敏。
//

#ifndef DESKTOP_PET_PROMPT_RENDERER_H
#define DESKTOP_PET_PROMPT_RENDERER_H

#include <QMap>
#include <QString>

class PromptRenderer {
public:
    // 扁平 {{slot}} 替换：遍历 vars 逐个 replace，再清掉残留的 {{...}}。
    static QString render(const QString& body, const QMap<QString, QString>& vars);

    // 脱敏：把 api_key/password/token/secret 关键词替换为 [redacted]。
    // 与 ContextManager::sanitizeForLlm 共用同一份关键词，集中维护。
    static QString redactSecrets(const QString& text);
};

#endif // DESKTOP_PET_PROMPT_RENDERER_H
