#ifndef DESKTOP_PET_PRIVATE_KEY_PROVIDER_H
#define DESKTOP_PET_PRIVATE_KEY_PROVIDER_H

#include "ai/domain/domain_result.h"
#include "reflection_types.h"

class PrivateKeyProvider {
public:
    virtual ~PrivateKeyProvider() = default;
    virtual Result<PrivateKeyMaterial, DomainError> loadOrCreate(
        const QString& profileId) = 0;
};

class QtKeychainPrivateKeyProvider final : public PrivateKeyProvider {
public:
    Result<PrivateKeyMaterial, DomainError> loadOrCreate(
        const QString& profileId) override;
};

bool privateReflectionBuildAvailable();

#endif // DESKTOP_PET_PRIVATE_KEY_PROVIDER_H
