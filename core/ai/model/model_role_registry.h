#ifndef DESKTOP_PET_MODEL_ROLE_REGISTRY_H
#define DESKTOP_PET_MODEL_ROLE_REGISTRY_H

#include <QList>

#include "ai/domain/domain_result.h"
#include "ai_types.h"

class ModelRoleRegistry {
public:
    explicit ModelRoleRegistry(QList<ModelRoleConfig> configs = {});

    Result<ModelRoleConfig, DomainError> configFor(ModelRole role) const;

private:
    QList<ModelRoleConfig> m_configs;
};

#endif // DESKTOP_PET_MODEL_ROLE_REGISTRY_H
