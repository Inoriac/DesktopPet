#include "private_psyche_crypto.h"

#if DESKTOP_PET_HAS_LIBSODIUM
#include <sodium.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace {

template <typename T>
Result<T, DomainError> cryptoUnavailable(const QString& message) {
    return Result<T, DomainError>::failure(
        domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"), message));
}

#if !DESKTOP_PET_HAS_LIBSODIUM && defined(Q_OS_WIN)
constexpr int kAesGcmKeyBytes = 32;
constexpr int kAesGcmNonceBytes = 12;
constexpr int kAesGcmTagBytes = 16;

struct BcryptHandles {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    ~BcryptHandles() {
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    bool initAesGcm(const QByteArray& rawKey) {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &alg, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
            return false;
        }
        if (!BCRYPT_SUCCESS(BCryptSetProperty(
                alg, BCRYPT_CHAINING_MODE,
                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
            return false;
        }
        return BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
            alg, &key, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(rawKey.constData())),
            static_cast<ULONG>(rawKey.size()), 0));
    }
};
#endif

} // namespace

Result<EncryptedPrivatePayload, DomainError>
SodiumPrivatePsycheCrypto::encrypt(
    const QByteArray& plaintext,
    const PrivateRecordAad& aad,
    const PrivateKeyMaterial& key) const {
#if DESKTOP_PET_HAS_LIBSODIUM
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
#elif defined(Q_OS_WIN)
    if (key.key.size() != kAesGcmKeyBytes || plaintext.isEmpty()) {
        return cryptoUnavailable<EncryptedPrivatePayload>(
            QStringLiteral("private encryption prerequisites are invalid"));
    }
    EncryptedPrivatePayload encrypted;
    encrypted.schemaVersion = aad.schemaVersion;
    encrypted.keyVersion = key.keyVersion;
    encrypted.nonce.resize(kAesGcmNonceBytes);
    if (!BCRYPT_SUCCESS(BCryptGenRandom(
            nullptr, reinterpret_cast<PUCHAR>(encrypted.nonce.data()),
            kAesGcmNonceBytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return cryptoUnavailable<EncryptedPrivatePayload>(
            QStringLiteral("private encryption nonce generation failed"));
    }
    BcryptHandles handles;
    if (!handles.initAesGcm(key.key)) {
        return cryptoUnavailable<EncryptedPrivatePayload>(
            QStringLiteral("private encryption cipher initialization failed"));
    }
    QByteArray tag(kAesGcmTagBytes, '\0');
    const QByteArray aadBytes = aad.toBytes();
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = reinterpret_cast<PUCHAR>(encrypted.nonce.data());
    authInfo.cbNonce = kAesGcmNonceBytes;
    authInfo.pbAuthData = aadBytes.isEmpty()
        ? nullptr
        : reinterpret_cast<PUCHAR>(const_cast<char*>(aadBytes.constData()));
    authInfo.cbAuthData = static_cast<ULONG>(aadBytes.size());
    authInfo.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    authInfo.cbTag = kAesGcmTagBytes;

    QByteArray cipherBody(plaintext.size(), '\0');
    ULONG outputSize = 0;
    const NTSTATUS status = BCryptEncrypt(
        handles.key,
        reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.constData())),
        static_cast<ULONG>(plaintext.size()), &authInfo, nullptr, 0,
        reinterpret_cast<PUCHAR>(cipherBody.data()),
        static_cast<ULONG>(cipherBody.size()), &outputSize, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return cryptoUnavailable<EncryptedPrivatePayload>(
            QStringLiteral("private encryption failed"));
    }
    cipherBody.resize(static_cast<qsizetype>(outputSize));
    encrypted.ciphertext = cipherBody + tag;
    return Result<EncryptedPrivatePayload, DomainError>::success(encrypted);
#else
    Q_UNUSED(plaintext)
    Q_UNUSED(aad)
    Q_UNUSED(key)
    return cryptoUnavailable<EncryptedPrivatePayload>(
        QStringLiteral("libsodium is unavailable"));
#endif
}

Result<QByteArray, DomainError> SodiumPrivatePsycheCrypto::decrypt(
    const EncryptedPrivatePayload& encrypted,
    const PrivateRecordAad& aad,
    const PrivateKeyMaterial& key) const {
#if DESKTOP_PET_HAS_LIBSODIUM
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
#elif defined(Q_OS_WIN)
    if (key.key.size() != kAesGcmKeyBytes
        || encrypted.nonce.size() != kAesGcmNonceBytes
        || encrypted.ciphertext.size() <= kAesGcmTagBytes
        || encrypted.schemaVersion != aad.schemaVersion
        || encrypted.keyVersion != aad.keyVersion) {
        return Result<QByteArray, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                        QStringLiteral("private record metadata is invalid")));
    }
    BcryptHandles handles;
    if (!handles.initAesGcm(key.key)) {
        return cryptoUnavailable<QByteArray>(
            QStringLiteral("private decryption cipher initialization failed"));
    }
    const qsizetype bodyLen = encrypted.ciphertext.size() - kAesGcmTagBytes;
    const QByteArray cipherBody = encrypted.ciphertext.left(bodyLen);
    QByteArray tag = encrypted.ciphertext.right(kAesGcmTagBytes);
    const QByteArray aadBytes = aad.toBytes();

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = reinterpret_cast<PUCHAR>(
        const_cast<char*>(encrypted.nonce.constData()));
    authInfo.cbNonce = kAesGcmNonceBytes;
    authInfo.pbAuthData = aadBytes.isEmpty()
        ? nullptr
        : reinterpret_cast<PUCHAR>(const_cast<char*>(aadBytes.constData()));
    authInfo.cbAuthData = static_cast<ULONG>(aadBytes.size());
    authInfo.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    authInfo.cbTag = kAesGcmTagBytes;

    QByteArray plaintext(bodyLen, '\0');
    ULONG outputSize = 0;
    const NTSTATUS status = BCryptDecrypt(
        handles.key,
        reinterpret_cast<PUCHAR>(const_cast<char*>(cipherBody.constData())),
        static_cast<ULONG>(cipherBody.size()), &authInfo, nullptr, 0,
        reinterpret_cast<PUCHAR>(plaintext.data()),
        static_cast<ULONG>(plaintext.size()), &outputSize, 0);
    if (!BCRYPT_SUCCESS(status)) {
        SecureZeroMemory(plaintext.data(), static_cast<size_t>(plaintext.size()));
        return Result<QByteArray, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                        QStringLiteral("private record authentication failed")));
    }
    plaintext.resize(static_cast<qsizetype>(outputSize));
    return Result<QByteArray, DomainError>::success(plaintext);
#else
    Q_UNUSED(encrypted)
    Q_UNUSED(aad)
    Q_UNUSED(key)
    return cryptoUnavailable<QByteArray>(QStringLiteral("libsodium is unavailable"));
#endif
}
