#include "capability_discovery.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
QString identity(const QJsonObject& entry) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{
        entry["pluginId"], entry["id"], entry["version"]}).toJson(QJsonDocument::Compact));
}
QStringList terms(const QString& query) {
    static const QRegularExpression word("[\\p{L}\\p{N}]+");
    QStringList result;
    auto matches = word.globalMatch(query);
    while (matches.hasNext()) result.append(matches.next().captured());
    // Chinese activity cues are sentences without spaces. Adjacent Han pairs
    // let a purpose such as 音乐 match inside 现在想听点音乐, without a tokenizer.
    static const QRegularExpression han("\\p{Han}{2,}");
    auto runs = han.globalMatch(query);
    while (runs.hasNext()) {
        const auto characters = runs.next().captured().toUcs4();
        for (int i = 0; i + 1 < characters.size(); ++i) {
            const char32_t pair[] = {char32_t(characters[i]), char32_t(characters[i + 1])};
            result.append(QString::fromUcs4(pair, 2));
        }
    }
    result.removeDuplicates();
    return result;
}
int relevance(const QJsonObject& entry, const QString& query, const QStringList& words) {
    if (query.isEmpty()) return 0;
    const auto id = entry["id"].toString().toCaseFolded();
    const auto plugin = entry["pluginId"].toString().toCaseFolded();
    const auto name = entry["name"].toString().toCaseFolded();
    const auto summary = entry["summary"].toString().toCaseFolded();
    if (id == query || name == query || plugin == query) return 1000;
    int score = 0;
    auto fragments = words;
    if (!fragments.contains(query)) fragments.prepend(query);
    for (const auto& text : fragments) {
        if (id.contains(text) || name.contains(text) || plugin.contains(text)) score += 30;
        else if (summary.contains(text)) score += 10;
    }
    return qMin(score, 999);
}
QList<QJsonObject> ranked(const QJsonArray& entries, const QString& query) {
    struct Match { QJsonObject entry; QString key; int score; };
    QList<Match> matches;
    const auto normalized = query.trimmed().toCaseFolded();
    const auto words = terms(normalized);
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const int score = relevance(entry, normalized, words);
        if (normalized.isEmpty() || score > 0) matches.append({entry, identity(entry), score});
    }
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        return a.score != b.score ? a.score > b.score : a.key < b.key;
    });
    QList<QJsonObject> result;
    for (const auto& match : matches) result.append(match.entry);
    return result;
}
QJsonObject display(QJsonObject entry) {
    for (const auto& field : {QString("name"), QString("summary")}) {
        const int limit = field == "name" ? 128 : 384;
        const auto text = entry[field].toString();
        if (text.size() > limit) {
            int end = limit - 1;
            if (text[end - 1].isHighSurrogate()) --end;
            entry[field] = text.left(end) + QChar(0x2026);
        }
    }
    return entry;
}
using QueryResult = Result<QJsonObject, DomainError>;
QueryResult invalidCursor() {
    return QueryResult::failure(domainError("CAPABILITY_CURSOR_INVALID",
        "Discovery cursor is invalid or expired; restart discovery without a cursor"));
}
}

Result<QJsonObject, DomainError> queryCapabilityEntries(
    const QJsonArray& entries, const QJsonObject& query, const QString& snapshotKey) {
    const auto inputError = [] {
        return QueryResult::failure(domainError("INVALID_INPUT", "Expected query (up to 200 characters), integer limit 1..16, and string cursor"));
    };
    for (auto it = query.begin(); it != query.end(); ++it)
        if (it.key() != "query" && it.key() != "limit" && it.key() != "cursor") return inputError();
    if ((query.contains("query") && !query["query"].isString()) ||
        query["query"].toString().toUcs4().size() > 200 ||
        (query.contains("cursor") && !query["cursor"].isString())) return inputError();
    const double limitValue = query.contains("limit") ? query["limit"].toDouble(-1) : 8;
    if (limitValue < 1 || limitValue > 16 || std::floor(limitValue) != limitValue) return inputError();
    const int limit = int(limitValue);
    const auto filtered = ranked(entries, query["query"].toString());
    const auto binding = QCryptographicHash::hash(QJsonDocument(QJsonArray{
        snapshotKey, query["query"].toString()}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    int offset = 0;
    const auto cursor = query["cursor"].toString().toUtf8();
    if (!cursor.isEmpty()) {
        const auto decoded = QByteArray::fromBase64Encoding(cursor,
            QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) != cursor)
            return invalidCursor();
        const auto payload = QJsonDocument::fromJson(decoded.decoded).array();
        const double position = payload.size() == 2 ? payload[1].toDouble(-1) : -1;
        if (payload.size() != 2 || payload[0].toString().toLatin1() != binding ||
            position < 1 || position >= filtered.size() || std::floor(position) != position)
            return invalidCursor();
        offset = int(position);
    }
    QJsonArray page;
    const auto end = qMin(offset + limit, int(filtered.size()));
    for (int i = offset; i < end; ++i) page.append(display(filtered[i]));
    const bool hasMore = end < filtered.size();
    const auto next = hasMore ? QJsonDocument(QJsonArray{QString::fromLatin1(binding), end})
        .toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) : QByteArray{};
    return QueryResult::success({{"entries", page}, {"total", filtered.size()},
        {"hasMore", hasMore}, {"nextCursor", QString::fromLatin1(next)}});
}

QJsonArray selectCapabilityCandidates(const QJsonArray& entries,
    const QString& query, quint64 browseOffset, int limit) {
    limit = qBound(0, limit, 16);
    QJsonArray visible;
    for (const auto& value : entries)
        if (value.toObject()["readiness"] != "disabled") visible.append(value);
    QJsonArray selected;
    QSet<QString> selectedIds;
    if (!query.trimmed().isEmpty()) {
        const auto related = ranked(visible, query);
        for (int i = 0; i < qMin(qMin(8, limit), int(related.size())); ++i) {
            selected.append(display(related[i]));
            selectedIds.insert(identity(related[i]));
        }
    }
    QList<QJsonObject> remaining;
    for (const auto& entry : ranked(visible, {}))
        if (!selectedIds.contains(identity(entry))) remaining.append(entry);
    const int count = qMin(limit - int(selected.size()), int(remaining.size()));
    for (int i = 0; i < count; ++i)
        selected.append(display(remaining[(browseOffset % remaining.size() + i) % remaining.size()]));
    return selected;
}
