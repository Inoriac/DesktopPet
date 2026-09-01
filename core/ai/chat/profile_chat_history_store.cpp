#include "profile_chat_history_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QUuid>
#include <QDebug>

namespace {

constexpr qint64 kMaximumHistoryLineBytes = 1024 * 1024;
constexpr int kHistoryLockTimeoutMs = 5000;
const QUuid kLegacyChatNamespace(
    QStringLiteral("{5c5ee62e-e730-44f1-8fe1-74c338d79cbf}"));

void clearError(QString* errorMessage) {
    if (errorMessage) errorMessage->clear();
}

bool fail(QString* errorMessage, const QString& message) {
    if (errorMessage) *errorMessage = message;
    return false;
}

bool isCanonicalProfileId(const QString& profileId) {
    const QString candidate = profileId.trimmed();
    const QUuid parsed(candidate);
    return !parsed.isNull()
        && parsed.toString(QUuid::WithoutBraces).toLower() == candidate;
}

bool isAllowedRole(const QString& role) {
    return role == QStringLiteral("user")
        || role == QStringLiteral("assistant")
        || role == QStringLiteral("system");
}

bool isValidEntry(const ChatHistoryEntry& entry) {
    if (entry.id.trimmed().isEmpty() || !isAllowedRole(entry.role)
        || !entry.timestamp.isValid() || !isTerminalChatMessageStatus(entry.status)) {
        return false;
    }
    if (entry.role != QStringLiteral("assistant") && !entry.replyToId.isEmpty()) {
        return false;
    }
    if ((entry.role == QStringLiteral("user")
         || entry.role == QStringLiteral("system"))
        && entry.content.trimmed().isEmpty()) {
        return false;
    }
    return true;
}

QByteArray encodeEntry(const ChatHistoryEntry& entry) {
    QJsonObject object{
        {QStringLiteral("schemaVersion"), 2},
        {QStringLiteral("id"), entry.id},
        {QStringLiteral("role"), entry.role},
        {QStringLiteral("content"), entry.content},
        {QStringLiteral("timestamp"), entry.timestamp.toString(Qt::ISODateWithMs)},
        {QStringLiteral("status"), chatMessageStatusStorageName(entry.status)},
    };
    if (!entry.replyToId.isEmpty()) {
        object.insert(QStringLiteral("replyToId"), entry.replyToId);
    }
    if (!entry.errorMessage.isEmpty()) {
        object.insert(QStringLiteral("errorMessage"), entry.errorMessage);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<ChatHistoryEntry> decodeVersionTwo(const QJsonObject& object) {
    if (object.value(QStringLiteral("schemaVersion")).toInt(-1) != 2) {
        return std::nullopt;
    }
    ChatHistoryEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.role = object.value(QStringLiteral("role")).toString();
    entry.replyToId = object.value(QStringLiteral("replyToId")).toString();
    entry.content = object.value(QStringLiteral("content")).toString();
    entry.errorMessage = object.value(QStringLiteral("errorMessage")).toString();
    entry.timestamp = QDateTime::fromString(
        object.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    if (!entry.timestamp.isValid()) {
        entry.timestamp = QDateTime::fromString(
            object.value(QStringLiteral("timestamp")).toString(), Qt::ISODate);
    }
    const auto status = chatMessageStatusFromStorageName(
        object.value(QStringLiteral("status")).toString());
    if (!status) return std::nullopt;
    entry.status = *status;
    if (!isValidEntry(entry)) return std::nullopt;
    return entry;
}

std::optional<ChatHistoryEntry> decodeLegacy(const QJsonObject& object,
                                             const QString& profileId,
                                             qint64 lineNumber,
                                             const QByteArray& rawLine) {
    ChatHistoryEntry entry;
    entry.role = object.value(QStringLiteral("role")).toString(
        QStringLiteral("system")).trimmed().toLower();
    entry.content = object.value(QStringLiteral("content")).toString().trimmed();
    entry.timestamp = QDateTime::fromString(
        object.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    if (!entry.timestamp.isValid()) {
        entry.timestamp = QDateTime::fromString(
            object.value(QStringLiteral("timestamp")).toString(), Qt::ISODate);
    }
    entry.status = ChatMessageStatus::Complete;
    const QByteArray seed = profileId.toUtf8() + ':' + QByteArray::number(lineNumber)
        + ':' + rawLine;
    entry.id = QUuid::createUuidV5(kLegacyChatNamespace, seed)
                   .toString(QUuid::WithoutBraces);
    if (!isValidEntry(entry)) return std::nullopt;
    return entry;
}

template <typename Decoder>
QList<ChatHistoryEntry> readLines(QFile& file, Decoder decoder, bool* skippedLines) {
    QList<ChatHistoryEntry> entries;
    QSet<QString> ids;
    qint64 lineNumber = 0;
    while (!file.atEnd()) {
        ++lineNumber;
        QByteArray line = file.readLine(kMaximumHistoryLineBytes + 2);
        const bool completeLine = line.endsWith('\n');
        if (!completeLine) {
            if (!file.atEnd()) {
                while (!file.atEnd() && !file.readLine(kMaximumHistoryLineBytes + 2).endsWith('\n')) {
                }
            }
            *skippedLines = true;
            continue;
        }
        line.chop(1);
        if (line.endsWith('\r')) line.chop(1);
        if (line.size() > kMaximumHistoryLineBytes || line.trimmed().isEmpty()) {
            if (!line.trimmed().isEmpty()) *skippedLines = true;
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            *skippedLines = true;
            continue;
        }
        const auto decoded = decoder(document.object(), lineNumber, line);
        if (!decoded || ids.contains(decoded->id)) {
            *skippedLines = true;
            continue;
        }
        ids.insert(decoded->id);
        entries.append(*decoded);
    }
    return entries;
}

void setOwnerOnlyPermissions(const QString& path) {
    if (!QFile::setPermissions(path,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        qWarning() << "Failed to restrict chat history file permissions";
    }
}

QString migrationMarkerKey(const QString& profileId) {
    return QStringLiteral("chat/%1/legacyImportedV1").arg(profileId);
}

} // namespace

bool ProfileChatHistoryStore::open(const ProfileChatStoreOptions& options,
                                   QString* errorMessage) {
    clearError(errorMessage);
    m_open = false;
    m_historyPath.clear();
    m_profileId.clear();
    if (options.appDataRoot.trimmed().isEmpty()
        || !isCanonicalProfileId(options.profileId)) {
        return fail(errorMessage, QStringLiteral("Chat history options are invalid."));
    }

    const QString profileRoot = QDir(options.appDataRoot).filePath(
        QStringLiteral("profiles/%1").arg(options.profileId));
    if (!QDir().mkpath(profileRoot)) {
        return fail(errorMessage, QStringLiteral("Unable to create the chat history directory."));
    }
    m_profileId = options.profileId;
    m_historyPath = QDir(profileRoot).filePath(QStringLiteral("chat_history.jsonl"));
    m_open = true;

    QLockFile lock(m_historyPath + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(kHistoryLockTimeoutMs)) {
        m_open = false;
        return fail(errorMessage, QStringLiteral("Unable to lock chat history while opening."));
    }
    if (!importLegacyIfEligible(options, errorMessage)
        || !ensureHistoryFile(errorMessage)) {
        m_open = false;
        return false;
    }
    return true;
}

QList<ChatHistoryEntry> ProfileChatHistoryStore::load(QString* errorMessage) const {
    clearError(errorMessage);
    if (!m_open) {
        fail(errorMessage, QStringLiteral("Chat history is not open."));
        return {};
    }
    QFile file(m_historyPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        fail(errorMessage, QStringLiteral("Unable to read chat history."));
        return {};
    }
    bool skippedLines = false;
    QList<ChatHistoryEntry> entries = readLines(
        file,
        [](const QJsonObject& object, qint64, const QByteArray&) {
            return decodeVersionTwo(object);
        },
        &skippedLines);
    if (skippedLines && errorMessage) {
        *errorMessage = QStringLiteral("Some invalid chat history entries were skipped.");
    }
    return entries;
}

bool ProfileChatHistoryStore::appendFinal(const ChatHistoryEntry& entry,
                                          QString* errorMessage) {
    clearError(errorMessage);
    if (!m_open) {
        return fail(errorMessage, QStringLiteral("Chat history is not open."));
    }
    if (!isValidEntry(entry)) {
        return fail(errorMessage, QStringLiteral("Chat history entry is invalid or non-terminal."));
    }

    QLockFile lock(m_historyPath + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(kHistoryLockTimeoutMs)) {
        return fail(errorMessage, QStringLiteral("Unable to lock chat history for writing."));
    }

    QFile file(m_historyPath);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Append)) {
        return fail(errorMessage, QStringLiteral("Unable to write chat history."));
    }
    bool needsLineSeparator = false;
    const qint64 existingSize = file.size();
    if (existingSize > 0) {
        char lastByte = 0;
        if (!file.seek(existingSize - 1) || file.read(&lastByte, 1) != 1) {
            return fail(errorMessage, QStringLiteral("Unable to inspect chat history before writing."));
        }
        needsLineSeparator = lastByte != '\n';
    }
    QByteArray line = encodeEntry(entry);
    if (needsLineSeparator) line.prepend('\n');
    line.append('\n');
    if (file.write(line) != line.size() || !file.flush()) {
        return fail(errorMessage, QStringLiteral("Unable to finish writing chat history."));
    }
    file.close();
    setOwnerOnlyPermissions(m_historyPath);
    return true;
}

bool ProfileChatHistoryStore::importLegacyIfEligible(
    const ProfileChatStoreOptions& options, QString* errorMessage) {
    const QFileInfo targetInfo(m_historyPath);
    if ((targetInfo.exists() && targetInfo.size() > 0)
        || QSettings().value(migrationMarkerKey(m_profileId), false).toBool()
        || options.legacyHistoryPath.trimmed().isEmpty()
        || !QFile::exists(options.legacyHistoryPath)) {
        return true;
    }

    QSet<QString> validProfileIds;
    for (const QString& profileId : options.registeredProfileIds) {
        if (isCanonicalProfileId(profileId)) validProfileIds.insert(profileId);
    }
    if (validProfileIds.size() != 1 || !validProfileIds.contains(m_profileId)) {
        return true;
    }

    QFile legacy(options.legacyHistoryPath);
    if (!legacy.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return fail(errorMessage, QStringLiteral("Unable to read legacy chat history."));
    }
    bool skippedLines = false;
    const QList<ChatHistoryEntry> imported = readLines(
        legacy,
        [this](const QJsonObject& object, qint64 lineNumber, const QByteArray& rawLine) {
            return decodeLegacy(object, m_profileId, lineNumber, rawLine);
        },
        &skippedLines);
    Q_UNUSED(skippedLines)

    QSaveFile target(m_historyPath);
    if (!target.open(QIODevice::WriteOnly)) {
        return fail(errorMessage, QStringLiteral("Unable to prepare migrated chat history."));
    }
    for (const ChatHistoryEntry& entry : imported) {
        QByteArray line = encodeEntry(entry);
        line.append('\n');
        if (target.write(line) != line.size()) {
            target.cancelWriting();
            return fail(errorMessage, QStringLiteral("Unable to migrate legacy chat history."));
        }
    }
    if (!target.commit()) {
        return fail(errorMessage, QStringLiteral("Unable to activate migrated chat history."));
    }
    setOwnerOnlyPermissions(m_historyPath);
    QSettings settings;
    settings.setValue(migrationMarkerKey(m_profileId), true);
    settings.sync();
    return true;
}

bool ProfileChatHistoryStore::ensureHistoryFile(QString* errorMessage) const {
    QFile file(m_historyPath);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return fail(errorMessage, QStringLiteral("Unable to read chat history."));
        }
        file.close();
        setOwnerOnlyPermissions(m_historyPath);
        return true;
    }
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(errorMessage, QStringLiteral("Unable to create chat history."));
    }
    file.close();
    setOwnerOnlyPermissions(m_historyPath);
    return true;
}
