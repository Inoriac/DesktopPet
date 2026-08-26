#include "model_role_registry.h"

#include <utility>

ModelRoleRegistry::ModelRoleRegistry(QList<ModelRoleConfig> configs)
    : m_configs(std::move(configs)) {}

Result<ModelRoleConfig, DomainError> ModelRoleRegistry::configFor(
    ModelRole role) const {
    for (const ModelRoleConfig& config : m_configs) {
        if (config.role == role) {
            return Result<ModelRoleConfig, DomainError>::success(config);
        }
    }
    return Result<ModelRoleConfig, DomainError>::failure(
        domainError(QStringLiteral("MODEL_ROLE_UNAVAILABLE"),
                    QStringLiteral("No model configuration exists for the requested role")));
}
