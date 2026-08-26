#ifndef DESKTOP_PET_PERSONA_PROJECTOR_H
#define DESKTOP_PET_PERSONA_PROJECTOR_H

#include "identity_baseline.h"
#include "identity_types.h"

#include <QMap>
#include <QString>

class EmotionStateProvider;
class SqliteIdentityRepository;
struct RuntimeSnapshot;

struct PersonaProjection {
    QMap<QString, QString> promptSlots;
    QMap<QString, double> behaviorParameters;
};

class PersonaProjector {
public:
    PersonaProjector() = default;
    PersonaProjector(IdentityBaseline baseline,
                     PersonalityPolicy policy,
                     const SqliteIdentityRepository* repository,
                     const EmotionStateProvider* emotionProvider);

    PersonaProjection project(const RuntimeSnapshot& snapshot,
                              const InteractionContext& context) const;
    PersonaProjection projectBaseline(const IdentityBaseline& baseline,
                                      const QString& petName) const;

private:
    IdentityBaseline m_baseline = IdentityBaseline::defaults();
    PersonalityPolicy m_policy;
    const SqliteIdentityRepository* m_repository = nullptr;
    const EmotionStateProvider* m_emotionProvider = nullptr;
};

#endif // DESKTOP_PET_PERSONA_PROJECTOR_H
