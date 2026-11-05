#include "private_key_provider.h"

#include <QEventLoop>
#include <QCryptographicHash>
#include <QTimer>

#if DESKTOP_PET_HAS_LIBSODIUM
#include <sodium.h>
#endif

#if DESKTOP_PET_HAS_QTKEYCHAIN
#include <qt6keychain/keychain.h>
#endif

#if (!DESKTOP_PET_HAS_QTKEYCHAIN || !DESKTOP_PET_HAS_LIBSODIUM) && defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include <bcrypt.h>
#include <string>
#endif

namespace {

constexpr int kPrivateKeyBytes = 32;

Result<PrivateKeyMaterial, DomainError> unavailable(const QString& message) {
    return Result<PrivateKeyMaterial, DomainError>::failure(
        domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"), message));
}

QString accountFor(const QString& profileId) {
    return QString::fromLatin1(QCryptographicHash::hash(
        profileId.toUtf8(), QCryptographicHash::Sha256).toHex());
}

#if DESKTOP_PET_HAS_QTKEYCHAIN && DESKTOP_PET_HAS_LIBSODIUM
template <typename Job>
bool waitForJob(Job* job) {
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    job->start();
    loop.exec();
    return !timeout.isActive() ? false : true;
}
#endif

} // namespace

bool privateReflectionBuildAvailable() {
#if DESKTOP_PET_HAS_PRIVATE_REFLECTION
    return true;
#else
    return false;
#endif
}

Result<PrivateKeyMaterial, DomainError>
QtKeychainPrivateKeyProvider::loadOrCreate(const QString& profileId) {
#if DESKTOP_PET_HAS_QTKEYCHAIN && DESKTOP_PET_HAS_LIBSODIUM
    if (!isCanonicalEventUuid(profileId)) {
        return unavailable(QStringLiteral("private key profile id is invalid"));
    }
    if (sodium_init() < 0) {
        return unavailable(QStringLiteral("libsodium initialization failed"));
    }
    constexpr auto service = "DesktopPet.PrivatePsyche";
    QKeychain::ReadPasswordJob readJob(QString::fromLatin1(service));
    readJob.setKey(accountFor(profileId));
    if (!waitForJob(&readJob)) {
        return unavailable(QStringLiteral("private key read timed out"));
    }
    if (readJob.error() == QKeychain::NoError) {
        const QByteArray key = QByteArray::fromBase64(readJob.textData().toLatin1());
        if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
            return unavailable(QStringLiteral("private key has invalid length"));
        }
        return Result<PrivateKeyMaterial, DomainError>::success(
            PrivateKeyMaterial{profileId, 1, key});
    }
    if (readJob.error() != QKeychain::EntryNotFound) {
        return unavailable(QStringLiteral("private key read failed: %1")
                               .arg(readJob.errorString()));
    }

    QByteArray key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES, Qt::Uninitialized);
    randombytes_buf(key.data(), static_cast<size_t>(key.size()));
    QKeychain::WritePasswordJob writeJob(QString::fromLatin1(service));
    writeJob.setKey(accountFor(profileId));
    writeJob.setTextData(QString::fromLatin1(key.toBase64()));
    if (!waitForJob(&writeJob) || writeJob.error() != QKeychain::NoError) {
        key.fill('\0');
        return unavailable(QStringLiteral("private key write failed"));
    }
    return Result<PrivateKeyMaterial, DomainError>::success(
        PrivateKeyMaterial{profileId, 1, key});
#elif defined(Q_OS_WIN)
    if (!isCanonicalEventUuid(profileId)) {
        return unavailable(QStringLiteral("private key profile id is invalid"));
    }
    const QString account = accountFor(profileId);
    const QString target = QStringLiteral("DesktopPet.PrivatePsyche/%1").arg(account);
    const std::wstring targetW = target.toStdWString();

    PCREDENTIALW existing = nullptr;
    if (CredReadW(targetW.c_str(), CRED_TYPE_GENERIC, 0, &existing)) {
        QByteArray raw(reinterpret_cast<const char*>(existing->CredentialBlob),
                       static_cast<qsizetype>(existing->CredentialBlobSize));
        CredFree(existing);
        QByteArray key = raw.size() == kPrivateKeyBytes
            ? raw
            : QByteArray::fromBase64(raw);
        SecureZeroMemory(raw.data(), static_cast<size_t>(raw.size()));
        if (key.size() != kPrivateKeyBytes) {
            SecureZeroMemory(key.data(), static_cast<size_t>(key.size()));
            return unavailable(QStringLiteral("private key has invalid length"));
        }
        return Result<PrivateKeyMaterial, DomainError>::success(
            PrivateKeyMaterial{profileId, 1, key});
    }
    const DWORD readErr = GetLastError();
    if (readErr != ERROR_NOT_FOUND) {
        return unavailable(QStringLiteral("private key read failed (win32=%1)")
                               .arg(readErr));
    }

    QByteArray key(kPrivateKeyBytes, '\0');
    const NTSTATUS rngStatus = BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(key.data()),
        static_cast<ULONG>(key.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(rngStatus)) {
        SecureZeroMemory(key.data(), static_cast<size_t>(key.size()));
        return unavailable(QStringLiteral("private key random generation failed"));
    }

    QByteArray encoded = key.toBase64();
    const std::wstring userW = account.toStdWString();
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(targetW.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(encoded.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(encoded.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = const_cast<LPWSTR>(userW.c_str());
    const BOOL written = CredWriteW(&cred, 0);
    const DWORD writeErr = written ? ERROR_SUCCESS : GetLastError();
    SecureZeroMemory(encoded.data(), static_cast<size_t>(encoded.size()));
    if (!written) {
        SecureZeroMemory(key.data(), static_cast<size_t>(key.size()));
        return unavailable(QStringLiteral("private key write failed (win32=%1)")
                               .arg(writeErr));
    }
    return Result<PrivateKeyMaterial, DomainError>::success(
        PrivateKeyMaterial{profileId, 1, key});
#else
    Q_UNUSED(profileId)
    return unavailable(QStringLiteral(
        "private reflection requires libsodium and QtKeychain"));
#endif
}
