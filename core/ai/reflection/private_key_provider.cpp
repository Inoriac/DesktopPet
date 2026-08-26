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

namespace {

Result<PrivateKeyMaterial, DomainError> unavailable(const QString& message) {
    return Result<PrivateKeyMaterial, DomainError>::failure(
        domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"), message));
}

#if DESKTOP_PET_HAS_QTKEYCHAIN && DESKTOP_PET_HAS_LIBSODIUM
QString accountFor(const QString& profileId) {
    return QString::fromLatin1(QCryptographicHash::hash(
        profileId.toUtf8(), QCryptographicHash::Sha256).toHex());
}

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
#if !DESKTOP_PET_HAS_QTKEYCHAIN || !DESKTOP_PET_HAS_LIBSODIUM
    Q_UNUSED(profileId)
    return unavailable(QStringLiteral(
        "private reflection requires libsodium and QtKeychain"));
#else
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
#endif
}
