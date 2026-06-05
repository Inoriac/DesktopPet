#ifndef DESKTOP_PET_CONTEXT_BUDGET_H
#define DESKTOP_PET_CONTEXT_BUDGET_H

#include <QList>
#include <QString>
#include <QtGlobal>

#include "ai_types.h"

struct ContextBudget {
    int maxMessages = 20;
    int maxCharacters = 12000;
    int maxToolSchemas = 32;

    QString trimString(const QString& value) const {
        if (value.size() <= maxCharacters) {
            return value;
        }
        return value.left(maxCharacters) + QString("... [context truncated %1 chars]").arg(value.size() - maxCharacters);
    }

    QList<ChatMessage> trimMessages(const QList<ChatMessage>& messages) const {
        QList<ChatMessage> trimmed = messages;
        while (trimmed.size() > maxMessages) {
            trimmed.removeFirst();
        }

        int remaining = maxCharacters;
        for (ChatMessage& message : trimmed) {
            if (message.content.size() > remaining) {
                message.content = message.content.left(qMax(0, remaining));
            }
            remaining -= message.content.size();
            if (remaining <= 0) {
                remaining = 0;
            }
        }
        return trimmed;
    }
};

#endif // DESKTOP_PET_CONTEXT_BUDGET_H