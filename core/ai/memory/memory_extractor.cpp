#include "memory_extractor.h"

#include <QDateTime>
#include <QJsonObject>
#include <QRegularExpression>

namespace {
QString normalizedText(const QString& input) {
    QString text = input.trimmed();
    text.replace(QRegularExpression("[\\r\\n]+"), " ");
    text.replace(QRegularExpression("\\s+"), " ");
    return text.trimmed();
}

QString stripTrailingPunctuation(QString text) {
    text = text.trimmed();
    text.replace(QRegularExpression("[。.!！?？；;，,]+$"), "");
    return text.trimmed();
}

QString makeKey(const QString& scope, const QString& content) {
    QString normalized = content.toLower().trimmed();
    normalized.replace(QRegularExpression("\\s+"), "_");
    normalized.replace(QRegularExpression("[^a-z0-9_\\u4e00-\\u9fa5]+"), "_");
    normalized = normalized.left(48).trimmed();
    if (normalized.isEmpty()) {
        normalized = QStringLiteral("item");
    }
    return QString("%1:%2").arg(scope, normalized);
}

bool containsSensitiveHint(const QString& text) {
    static const QStringList hints = {
        QStringLiteral("密码"), QStringLiteral("口令"), QStringLiteral("密钥"),
        QStringLiteral("api key"), QStringLiteral("apikey"), QStringLiteral("access key"),
        QStringLiteral("secret"), QStringLiteral("token"),
        QStringLiteral("身份证"), QStringLiteral("银行卡"), QStringLiteral("信用卡"),
        QStringLiteral("手机号"), QStringLiteral("手机号码"), QStringLiteral("住址"),
        QStringLiteral("家庭住址")
    };

    const QString lower = text.toLower();
    for (const QString& hint : hints) {
        if (lower.contains(hint.toLower())) {
            return true;
        }
    }
    return false;
}

MemoryCandidate makeWriteCandidate(MemoryType type,
                                   const QString& scope,
                                   const QString& content,
                                   const QString& sourceText,
                                   const QString& triggerTag,
                                   bool explicitRequest) {
    MemoryCandidate candidate;
    candidate.operation = MemoryCandidateOperation::Write;
    candidate.rawText = sourceText;
    candidate.triggerTag = triggerTag;
    candidate.explicitRequest = explicitRequest;

    MemoryEntry entry;
    entry.type = type;
    entry.status = MemoryStatus::Active;
    entry.privacyLevel = containsSensitiveHint(content) ? PrivacyLevel::Sensitive : PrivacyLevel::Personal;
    entry.scope = scope;
    entry.key = makeKey(scope, content);
    entry.summary = content;
    entry.content = content;
    entry.value = content;
    entry.source = explicitRequest ? QStringLiteral("user_explicit") : QStringLiteral("rule_inferred");
    entry.confidence = explicitRequest ? 0.98 : 0.78;
    entry.importance = explicitRequest ? 0.72 : 0.55;
    entry.strength = entry.importance;
    entry.evidence = {sourceText};
    entry.tags = {scope, triggerTag};

    if (type == MemoryType::Preference) {
        entry.tags.append(QStringLiteral("preference"));
    } else if (type == MemoryType::Core) {
        entry.tags.append(QStringLiteral("core"));
        entry.importance = 0.9;
        entry.strength = 0.9;
    } else if (type == MemoryType::Semantic) {
        entry.tags.append(QStringLiteral("semantic"));
    }

    QJsonObject payload;
    payload["extracted_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    payload["extractor"] = "rule_v1";
    payload["explicit_request"] = explicitRequest;
    entry.payload = payload;

    candidate.entry = entry;
    return candidate;
}

bool matchFirst(const QString& text, const QList<QRegularExpression>& expressions, QString* captured) {
    for (const QRegularExpression& expression : expressions) {
        const QRegularExpressionMatch match = expression.match(text);
        if (match.hasMatch()) {
            *captured = stripTrailingPunctuation(match.captured(1));
            return !captured->isEmpty();
        }
    }
    return false;
}
}

QList<MemoryCandidate> MemoryExtractor::extractFromUserInput(const QString& input,
                                                             const QString& triggerTag) const {
    QList<MemoryCandidate> candidates;
    const QString text = normalizedText(input);
    if (text.isEmpty()) {
        return candidates;
    }

    QString captured;
    if (matchFirst(text, {
            QRegularExpression(QStringLiteral("^(?:忘记|忘掉|删除记忆|不要记住|别记住)[：:，, ]*(.+)$")),
            QRegularExpression(QStringLiteral("^(?:删除|清除|移除)(?:关于)?(.+?)(?:的)?(?:记忆|相关记忆)$")),
            QRegularExpression(QStringLiteral("^(?:把)?(.+?)(?:这条记忆|相关记忆)?(?:忘记|忘掉|删掉|删除)$"))
        }, &captured)) {
        MemoryCandidate candidate;
        candidate.operation = MemoryCandidateOperation::Forget;
        candidate.query = captured;
        candidate.rawText = text;
        candidate.triggerTag = triggerTag;
        candidate.explicitRequest = true;
        candidates.append(candidate);
        return candidates;
    }

    if (matchFirst(text, {
            QRegularExpression(QStringLiteral("^(?:请)?(?:帮我)?(?:记住|记一下|记下来|帮我记住|帮我记一下)[：:，, ]*(.+)$")),
            QRegularExpression(QStringLiteral("^(.+?)(?:这件事|这个事情)?(?:要|需要)?(?:一直|长期)?记住$"))
        }, &captured)) {
        MemoryType type = MemoryType::Semantic;
        QString scope = QStringLiteral("user");
        if (captured.contains(QStringLiteral("喜欢")) || captured.contains(QStringLiteral("不喜欢"))
            || captured.contains(QStringLiteral("希望")) || captured.contains(QStringLiteral("不要"))) {
            type = MemoryType::Preference;
            scope = QStringLiteral("preference");
        }
        if (captured.contains(QStringLiteral("一直")) || text.contains(QStringLiteral("一直记住"))) {
            type = MemoryType::Core;
            scope = QStringLiteral("core");
        }
        candidates.append(makeWriteCandidate(type, scope, captured, text, triggerTag, true));
    }

    if (matchFirst(text, {
            QRegularExpression(QStringLiteral("^(?:我)?(?:喜欢|偏好|更喜欢)[：:，, ]*(.+)$")),
            QRegularExpression(QStringLiteral("^(?:以后|今后).*(?:喜欢|偏好|更喜欢)[：:，, ]*(.+)$"))
        }, &captured)) {
        const QString summary = QStringLiteral("用户喜欢%1").arg(captured);
        candidates.append(makeWriteCandidate(MemoryType::Preference, QStringLiteral("preference"), summary, text, triggerTag, true));
    }

    if (matchFirst(text, {
            QRegularExpression(QStringLiteral("^(?:我)?(?:不喜欢|讨厌|不希望)[：:，, ]*(.+)$")),
            QRegularExpression(QStringLiteral("^(?:以后|今后)?(?:不要|别)[：:，, ]*(.+)$"))
        }, &captured)) {
        const QString summary = QStringLiteral("用户不希望%1").arg(captured);
        candidates.append(makeWriteCandidate(MemoryType::Preference, QStringLiteral("preference"), summary, text, triggerTag, true));
        return candidates;
    }

    if (matchFirst(text, {
            QRegularExpression(QStringLiteral("^(?:以后|今后)(?:请)?(?:尽量|默认)?[：:，, ]*(.+)$"))
        }, &captured)) {
        const QString summary = QStringLiteral("用户希望以后%1").arg(captured);
        candidates.append(makeWriteCandidate(MemoryType::Preference, QStringLiteral("preference"), summary, text, triggerTag, true));
    }

    return candidates;
}
