#include "private_psyche_crypto.h"

#if DESKTOP_PET_HAS_LIBSODIUM
#include <sodium.h>
#endif

namespace {

template <typename T>
Result<T, DomainError> cryptoUnavailable(const QString& message) {
    return Result<T, DomainError>::failure(
        domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"), message));
}

} // namespace

Result<EncryptedPrivatePayload, DomainError>
SodiumPrivatePsycheCrypto::encrypt(
    const QByteArray& plaintext,
    const PrivateRecordAad& aad,
    const PrivateKeyMaterial& key) const {
#if !DESKTOP_PET_HAS_LIBSODIUM
    Q_UNUSED(plaintext)
    Q_UNUSED(aad)
    Q_UNUSED(key)
    return cryptoUnavailable<EncryptedPrivatePayload>(
        QStringLiteral("libsodium is unavailable"));
#else
    if (sodium_init() < 0
        || key.key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES
        || plaintext.isEmpty()) {
        return cryptoUnavailable<EncryptedPrivatePayload>(
            QStringLiteral("private encryption prerequisites are invalid"));
    }
    EncryptedPrivatePayload encrypted;
    encrypted.schemaVersion = aad.schemaVersion;
    encrypted.keyVersion = key.keyVersion;
    encrypted.nonce.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    randombytes_buf(encrypted.nonce.data(), static_cast<size_t>(encrypted.nonce.size()));
    encrypted.ciphertext.resize(
        plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long outputSize = 0;
    const QByteArray aadBytes = aad.toBytes();
    const int status = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(encrypted.ciphertext.data()), &outputSize,
        reinterpret_cast<const unsigned char*>(plaintext.constData()),
        static_cast<unsigned long long>(plaintext.size()),
        reinterpret_cast<const unsigned char*>(aadBytes.constData()),
        static_cast<unsigned long long>(aadBytes.size()), nullptr,
        reinterpret_cast<const unsigned char*>(encrypted.nonce.constData()),
        reinterpret_cast<const unsigned char*>(key.key.constData()));
    if (status != 0) {
        return cryptoUnavailable<EncryptedPrivatePayload>(
            QStringLiteral("private encryption failed"));
    }
    encrypted.ciphertext.resize(static_cast<qsizetype>(outputSize));
    return Result<EncryptedPrivatePayload, DomainError>::success(encrypted);
#endif
}

Result<QByteArray, DomainError> SodiumPrivatePsycheCrypto::decrypt(
    const EncryptedPrivatePayload& encrypted,
    const PrivateRecordAad& aad,
    const PrivateKeyMaterial& key) const {
#if !DESKTOP_PET_HAS_LIBSODIUM
    Q_UNUSED(encrypted)
    Q_UNUSED(aad)
    Q_UNUSED(key)
    return cryptoUnavailable<QByteArray>(QStringLiteral("libsodium is unavailable"));
#else
    if (sodium_init() < 0
        || key.key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES
        || encrypted.nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
        || encrypted.ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES
        || encrypted.schemaVersion != aad.schemaVersion
        || encrypted.keyVersion != aad.keyVersion) {
        return Result<QByteArray, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                        QStringLiteral("private record metadata is invalid")));
    }
    QByteArray plaintext(
        encrypted.ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES,
        Qt::Uninitialized);
    unsigned long long outputSize = 0;
    const QByteArray aadBytes = aad.toBytes();
    const int status = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char*>(plaintext.data()), &outputSize, nullptr,
        reinterpret_cast<const unsigned char*>(encrypted.ciphertext.constData()),
        static_cast<unsigned long long>(encrypted.ciphertext.size()),
        reinterpret_cast<const unsigned char*>(aadBytes.constData()),
        static_cast<unsigned long long>(aadBytes.size()),
        reinterpret_cast<const unsigned char*>(encrypted.nonce.constData()),
        reinterpret_cast<const unsigned char*>(key.key.constData()));
    if (status != 0) {
        plaintext.fill('\0');
        return Result<QByteArray, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                        QStringLiteral("private record authentication failed")));
    }
    plaintext.resize(static_cast<qsizetype>(outputSize));
    return Result<QByteArray, DomainError>::success(plaintext);
#endif
}
