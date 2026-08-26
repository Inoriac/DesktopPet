#include "owner_diary_protocol.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#endif

namespace {

DomainError protocolError(const QString& code, const QString& message) {
    return domainError(code, message);
}

bool hasPrivateOwnerPermissions(const QString& path) {
#ifdef Q_OS_UNIX
    struct stat info {};
    const QByteArray native = QFile::encodeName(path);
    return ::lstat(native.constData(), &info) == 0
        && S_ISREG(info.st_mode)
        && info.st_uid == ::geteuid()
        && (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
#elif defined(Q_OS_WIN)
    const std::wstring native = path.toStdWString();
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityResult = GetNamedSecurityInfoW(
        const_cast<wchar_t*>(native.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr,
        &descriptor);
    if (securityResult != ERROR_SUCCESS || !owner || !dacl) {
        if (descriptor) LocalFree(descriptor);
        return false;
    }
    HANDLE processToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &processToken)) {
        LocalFree(descriptor);
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(processToken, TokenUser, nullptr, 0, &size);
    QByteArray buffer(static_cast<int>(size), Qt::Uninitialized);
    const bool tokenRead = GetTokenInformation(
        processToken, TokenUser, buffer.data(), size, &size);
    CloseHandle(processToken);
    const PSID currentUser = tokenRead
        ? reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid : nullptr;
    bool privateDacl = currentUser && dacl->AceCount > 0;
    for (DWORD index = 0; privateDacl && index < dacl->AceCount; ++index) {
        void* rawAce = nullptr;
        if (!GetAce(dacl, index, &rawAce)) {
            privateDacl = false;
            break;
        }
        const auto* header = static_cast<const ACE_HEADER*>(rawAce);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            privateDacl = false;
            break;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
        privateDacl = EqualSid(
            currentUser, const_cast<DWORD*>(&ace->SidStart));
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool protectedDacl = GetSecurityDescriptorControl(
        descriptor, &control, &revision)
        && (control & SE_DACL_PROTECTED) != 0;
    const bool sameOwner = currentUser && EqualSid(owner, currentUser);
    LocalFree(descriptor);
    return sameOwner && privateDacl && protectedDacl;
#else
    const QFile::Permissions permissions = QFile::permissions(path);
    return !(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                            | QFileDevice::ExeGroup | QFileDevice::ReadOther
                            | QFileDevice::WriteOther | QFileDevice::ExeOther));
#endif
}

void securelyRemove(QFile& file) {
    if (!file.isOpen()) {
        if (!file.open(QIODevice::ReadWrite)) {
            QFile::remove(file.fileName());
            return;
        }
    }
    const qint64 size = file.size();
    file.seek(0);
    const QByteArray zeros(4096, '\0');
    qint64 remaining = size;
    while (remaining > 0) {
        const qint64 chunk = std::min<qint64>(remaining, zeros.size());
        if (file.write(zeros.constData(), chunk) != chunk) break;
        remaining -= chunk;
    }
    file.flush();
    file.close();
    QFile::remove(file.fileName());
}

bool hasExactBootstrapKeys(const QJsonObject& object) {
    static const QSet<QString> keys{
        QStringLiteral("protocolVersion"), QStringLiteral("profileId"),
        QStringLiteral("socketName"), QStringLiteral("capabilityToken"),
        QStringLiteral("expiresAt"), QStringLiteral("maxFrameBytes"),
        QStringLiteral("sessionTtlSeconds")
    };
    const QStringList actualKeys = object.keys();
    const QSet<QString> actual(actualKeys.cbegin(), actualKeys.cend());
    return actual == keys;
}

} // namespace

Result<OwnerDiaryBootstrap, DomainError> consumeOwnerDiaryBootstrap(
    const QString& absolutePath,
    const QString& expectedProfileId) {
    const QFileInfo info(absolutePath);
    if (absolutePath.trimmed().isEmpty() || !info.isAbsolute() || info.isSymLink()
        || !info.isFile() || !hasPrivateOwnerPermissions(absolutePath)) {
        return Result<OwnerDiaryBootstrap, DomainError>::failure(
            protocolError(QStringLiteral("OWNER_AUTH_FAILED"),
                          QStringLiteral("owner diary bootstrap is not private")));
    }
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadWrite) || file.size() <= 0 || file.size() > 65536) {
        securelyRemove(file);
        return Result<OwnerDiaryBootstrap, DomainError>::failure(
            protocolError(QStringLiteral("OWNER_AUTH_FAILED"),
                          QStringLiteral("owner diary bootstrap cannot be read")));
    }
    const QByteArray bytes = file.readAll();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !hasExactBootstrapKeys(document.object())) {
        securelyRemove(file);
        return Result<OwnerDiaryBootstrap, DomainError>::failure(
            protocolError(QStringLiteral("OWNER_AUTH_FAILED"),
                          QStringLiteral("owner diary bootstrap is invalid")));
    }
    const QJsonObject object = document.object();
    OwnerDiaryBootstrap bootstrap;
    bootstrap.protocolVersion = object.value(QStringLiteral("protocolVersion")).toInt(-1);
    bootstrap.profileId = object.value(QStringLiteral("profileId")).toString();
    bootstrap.socketName = object.value(QStringLiteral("socketName")).toString();
    bootstrap.capabilityToken = ownerDiaryTokenFromBase64Url(
        object.value(QStringLiteral("capabilityToken")).toString());
    const QString expiresAt = object.value(QStringLiteral("expiresAt")).toString();
    bootstrap.expiresAt = QDateTime::fromString(expiresAt, Qt::ISODateWithMs);
    bootstrap.maxFrameBytes = std::clamp<qsizetype>(
        object.value(QStringLiteral("maxFrameBytes")).toInteger(-1),
        kOwnerDiaryMinFrameBytes, kOwnerDiaryMaxFrameBytes);
    bootstrap.sessionTtlSeconds = std::clamp(
        object.value(QStringLiteral("sessionTtlSeconds")).toInt(-1),
        kOwnerDiaryMinSessionTtlSeconds, kOwnerDiaryMaxSessionTtlSeconds);

    static const QRegularExpression socketPattern(
        QStringLiteral("^[A-Za-z0-9_.-]{16,220}$"));
    const bool valid = bootstrap.protocolVersion == kOwnerDiaryProtocolVersion
        && bootstrap.profileId == expectedProfileId
        && socketPattern.match(bootstrap.socketName).hasMatch()
        && bootstrap.socketName.contains(expectedProfileId)
        && bootstrap.capabilityToken.size() == 32
        && expiresAt.endsWith(QLatin1Char('Z'))
        && bootstrap.expiresAt.isValid()
        && bootstrap.expiresAt > QDateTime::currentDateTimeUtc()
        && object.value(QStringLiteral("maxFrameBytes")).isDouble()
        && object.value(QStringLiteral("sessionTtlSeconds")).isDouble();
    securelyRemove(file);
    if (!valid) {
        bootstrap.capabilityToken.fill('\0');
        bootstrap.capabilityToken.clear();
        return Result<OwnerDiaryBootstrap, DomainError>::failure(
            protocolError(QStringLiteral("OWNER_AUTH_FAILED"),
                          QStringLiteral("owner diary bootstrap fields are invalid")));
    }
    return Result<OwnerDiaryBootstrap, DomainError>::success(std::move(bootstrap));
}

QByteArray encodeOwnerDiaryFrame(const QJsonObject& object) {
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()),
                          reinterpret_cast<uchar*>(frame.data()));
    frame.append(payload);
    return frame;
}

Result<QJsonObject, DomainError> parseOwnerDiaryJson(const QByteArray& payload) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<QJsonObject, DomainError>::failure(
            protocolError(QStringLiteral("PROTOCOL_INVALID"),
                          QStringLiteral("owner diary frame is not a JSON object")));
    }
    return Result<QJsonObject, DomainError>::success(document.object());
}

QByteArray ownerDiaryRandomToken() {
    QByteArray token(32, Qt::Uninitialized);
    for (int offset = 0; offset < token.size(); offset += 4) {
        qToBigEndian<quint32>(QRandomGenerator::system()->generate(),
                              reinterpret_cast<uchar*>(token.data() + offset));
    }
    return token;
}

QByteArray ownerDiaryTokenFromBase64Url(const QString& encoded) {
    if (encoded.contains(QLatin1Char('='))) return {};
    const QByteArray decoded = QByteArray::fromBase64(
        encoded.toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    return ownerDiaryTokenToBase64Url(decoded) == encoded ? decoded : QByteArray{};
}

QString ownerDiaryTokenToBase64Url(const QByteArray& token) {
    return QString::fromLatin1(token.toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool ownerDiaryConstantTimeEquals(const QByteArray& left, const QByteArray& right) {
    const qsizetype size = std::max(left.size(), right.size());
    unsigned char difference = static_cast<unsigned char>(left.size() ^ right.size());
    for (qsizetype index = 0; index < size; ++index) {
        const unsigned char leftByte = index < left.size()
            ? static_cast<unsigned char>(left.at(index)) : 0;
        const unsigned char rightByte = index < right.size()
            ? static_cast<unsigned char>(right.at(index)) : 0;
        difference |= leftByte ^ rightByte;
    }
    return difference == 0;
}

QJsonObject ownerDiarySuccessResponse(
    const QString& requestId,
    const QJsonObject& data) {
    return {
        {QStringLiteral("protocolVersion"), kOwnerDiaryProtocolVersion},
        {QStringLiteral("requestId"), requestId},
        {QStringLiteral("ok"), true},
        {QStringLiteral("data"), data},
        {QStringLiteral("error"), QJsonObject{
             {QStringLiteral("code"), QString()},
             {QStringLiteral("message"), QString()},
             {QStringLiteral("retryable"), false}}}
    };
}

QJsonObject ownerDiaryErrorResponse(
    const QString& requestId,
    const QString& code,
    const QString& message,
    bool retryable) {
    return {
        {QStringLiteral("protocolVersion"), kOwnerDiaryProtocolVersion},
        {QStringLiteral("requestId"), requestId},
        {QStringLiteral("ok"), false},
        {QStringLiteral("data"), QJsonObject{}},
        {QStringLiteral("error"), QJsonObject{
             {QStringLiteral("code"), code},
             {QStringLiteral("message"), message},
             {QStringLiteral("retryable"), retryable}}}
    };
}
