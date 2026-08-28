#ifndef DESKTOP_PET_THINKING_STATUS_SELECTOR_H
#define DESKTOP_PET_THINKING_STATUS_SELECTOR_H

#include "ai/chat/chat_types.h"

#include <QHash>
#include <QString>

class ThinkingStatusSelector {
public:
    QString next(ChatActivityStage stage, const QString& requestId);
    void reset(const QString& requestId);

private:
    QString m_requestId;
    QHash<int, int> m_nextIndexes;
};

#endif // DESKTOP_PET_THINKING_STATUS_SELECTOR_H
