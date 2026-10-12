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
    // New cognitive roles share the configured dialogue provider unless the
    // user explicitly supplies a role route (including a disabled route).
    if (role == ModelRole::AgencyDecision || role == ModelRole::TaskExecution
        || role == ModelRole::PersonalityEvolution) {
        for (const auto& config : m_configs) {
            if (config.role != ModelRole::Dialogue) continue;
            auto inherited = config;
            inherited.role = role;
            for (auto& route : inherited.routes) {
                route.llm.retryCount = 0;
                route.llm.maxTokens = qMax(route.llm.maxTokens,
                    role == ModelRole::AgencyDecision ? 512 : 2048);
            }
            return Result<ModelRoleConfig, DomainError>::success(std::move(inherited));
        }
    }
    return Result<ModelRoleConfig, DomainError>::failure(
        domainError(QStringLiteral("MODEL_ROLE_UNAVAILABLE"),
                    QStringLiteral("No model configuration exists for the requested role")));
}
