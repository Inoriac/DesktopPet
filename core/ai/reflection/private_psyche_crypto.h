#ifndef DESKTOP_PET_PRIVATE_PSYCHE_CRYPTO_H
#define DESKTOP_PET_PRIVATE_PSYCHE_CRYPTO_H

#include "ai/domain/domain_result.h"
#include "reflection_types.h"

class PrivatePsycheCrypto {
public:
    virtual ~PrivatePsycheCrypto() = default;
    virtual Result<EncryptedPrivatePayload, DomainError> encrypt(
        const QByteArray& plaintext,
        const PrivateRecordAad& aad,
        const PrivateKeyMaterial& key) const = 0;
    virtual Result<QByteArray, DomainError> decrypt(
        const EncryptedPrivatePayload& encrypted,
        const PrivateRecordAad& aad,
        const PrivateKeyMaterial& key) const = 0;
};

class SodiumPrivatePsycheCrypto final : public PrivatePsycheCrypto {
public:
    Result<EncryptedPrivatePayload, DomainError> encrypt(
        const QByteArray& plaintext,
        const PrivateRecordAad& aad,
        const PrivateKeyMaterial& key) const override;
    Result<QByteArray, DomainError> decrypt(
        const EncryptedPrivatePayload& encrypted,
        const PrivateRecordAad& aad,
        const PrivateKeyMaterial& key) const override;
};

#endif // DESKTOP_PET_PRIVATE_PSYCHE_CRYPTO_H
