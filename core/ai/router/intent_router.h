#ifndef DESKTOP_PET_INTENT_ROUTER_H
#define DESKTOP_PET_INTENT_ROUTER_H

#include <QString>
#include <QStringList>

#include "intent_types.h"

class IntentRouter {
public:
    IntentRoute route(const QString& input, const QString& triggerTag = "user_request") const;

private:
    bool containsAny(const QString& normalizedInput, const QStringList& keywords) const;
    QString normalize(const QString& input) const;
};

#endif // DESKTOP_PET_INTENT_ROUTER_H