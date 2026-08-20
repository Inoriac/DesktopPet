#ifndef DESKTOP_PET_PERSONA_PROJECTOR_H
#define DESKTOP_PET_PERSONA_PROJECTOR_H

#include "identity_baseline.h"

#include <QMap>
#include <QString>

struct PersonaProjection {
    QMap<QString, QString> promptSlots;
};

class PersonaProjector {
public:
    PersonaProjection projectBaseline(const IdentityBaseline& baseline,
                                      const QString& petName) const;
};

#endif // DESKTOP_PET_PERSONA_PROJECTOR_H
