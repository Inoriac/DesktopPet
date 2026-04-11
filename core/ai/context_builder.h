//
// ContextBuilder
// 负责构造 AIBrain 调用 LLM 所需的系统提示与运行时上下文
//

#ifndef DESKTOP_PET_CONTEXT_BUILDER_H
#define DESKTOP_PET_CONTEXT_BUILDER_H

#include <QString>
#include <QStringList>

class ContextBuilder {
public:
    QString buildSystemPrompt(const QString& petName) const;
    QString buildRuntimeContext(const QString& petName,
                                const QString& reason,
                                const QString& currentState = QString(),
                                const QString& triggerTag = QString(),
                                const QStringList& allowedActions = QStringList()) const;

private:
    QString buildStatisticsSummary(const QString& petName) const;
};

#endif // DESKTOP_PET_CONTEXT_BUILDER_H
