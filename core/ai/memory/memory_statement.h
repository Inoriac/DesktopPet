#ifndef DESKTOP_PET_MEMORY_STATEMENT_H
#define DESKTOP_PET_MEMORY_STATEMENT_H

#include <QRegularExpression>
#include <QStringList>
#include <optional>

// These rules recognize explicit statements, not arbitrary reported speech.
// Keep the request prefix consistent with MemoryExtractor's remember syntax.
namespace MemoryStatement {
inline QString body(QString text) {
    static const QRegularExpression prefix(QStringLiteral(
        "^(?:补充(?:一条|一下)[：:，, ]*)?(?:请)?(?:帮我)?(?:长期|一直)?"
        "(?:记住|记一下|记下来)[：:，, ]*"));
    text = text.trimmed();
    text.remove(prefix);
    return text;
}

inline bool reportedOrUncertain(const QString& text) {
    static const QRegularExpression reported(QStringLiteral(
        "[\"“”「」]|听.*说|告诉|提到|表示|认为|据说|转述|假如|如果|也许|可能|"
        "(?:他|她|朋友|同事).*说"));
    return reported.match(text).hasMatch();
}

inline bool ownObject(const QString& text) {
    // Possession of an object is not the same as a third person's statement:
    // e.g. 我的朋友 / 我的盆栽的主人 must remain unresolved.
    static const QRegularExpression object(QStringLiteral(
        "^我的(?:测试|验收|备用|常用|新|旧)?"
        "(?:盆栽|植物|书签|设备|电脑|手机|闹钟|日程|提醒|书房|房间)"
        "[^的，,。！？\\r\\n]{0,24}(?:叫|名为|放在|位于|在|是|改为)"));
    if (reportedOrUncertain(text) || !object.match(text).hasMatch()) return false;
    // A second clause may introduce someone else's fact. Only keep explicit
    // continuations about the object; do not assign an entire mixed report to
    // the speaker just because its first clause describes their possession.
    static const QRegularExpression continuation(QStringLiteral(
        "^(?:放在|位于|位置|每(?:周|天|月|年)|原来的|名称|名字|型号|颜色|编号|品牌|这和|浇水|用于)"));
    const auto clauses = text.split(QRegularExpression(QStringLiteral("[，,。；;]")), Qt::SkipEmptyParts);
    for (const auto& clause : clauses.mid(1))
        if (!continuation.match(clause.trimmed()).hasMatch()) return false;
    return true;
}

struct Correction {
    QString subject;
    QString previous;
    QString current;
    QString statement;
};

inline std::optional<Correction> correction(const QString& text) {
    const QString value = body(text);
    if (!ownObject(value)) return std::nullopt;
    static const QRegularExpression change(QStringLiteral(
        "^(我的[^的，,。！？\\r\\n]{2,40}?)(?:现在)?改为([^，,。！？\\r\\n]{2,80})[，,]"
        "原来的([^，,。！？\\r\\n]{2,80}?)(?:已经|已)?(?:作废|不再有效)"
        "(?:[，,](.+?))?[。！!]*$"));
    const auto match = change.match(value);
    if (!match.hasMatch()) return std::nullopt;
    Correction result{match.captured(1).trimmed(), match.captured(3).trimmed(),
                      match.captured(2).trimmed(), {}};
    if (result.current.contains(result.previous)) return std::nullopt;
    result.statement = result.subject + result.current;
    if (!match.captured(4).isEmpty()) result.statement += QStringLiteral("，") + match.captured(4);
    return result;
}

inline bool replacesCompleteFact(const QString& oldFact, const Correction& update) {
    if (reportedOrUncertain(oldFact)) return false;
    const auto clauses = oldFact.split(QRegularExpression(QStringLiteral("[，,。；;]")), Qt::SkipEmptyParts);
    if (clauses.isEmpty()) return false;
    QString subject = clauses.first().trimmed();
    subject.remove(QRegularExpression(QStringLiteral("叫做|名为|叫")));
    if (!subject.startsWith(update.subject)) return false;
    const QString firstDetail = subject.mid(update.subject.size());
    if (!firstDetail.isEmpty() && !firstDetail.startsWith(update.previous)) return false;
    bool replaces = false;
    const auto coversDetail = [&](const QString& raw) {
        QString clause = raw.trimmed();
        QString unchanged = clause;
        unchanged.remove(QRegularExpression(QStringLiteral("^(?:放在|位于|位置仍是|位置是|位置在|是|在)")));
        if (!unchanged.isEmpty() && update.statement.contains(unchanged)) return true;
        const auto previousAt = clause.indexOf(update.previous);
        if (previousAt < 0) return false;
        // Only the explicit old value may disappear. A matching weekday alone
        // must not discard a watering amount or a different action such as feeding.
        const auto prefix = clause.left(previousAt);
        const auto suffix = clause.mid(previousAt + update.previous.size());
        if (update.current.size() <= prefix.size() + suffix.size()
            || !update.current.startsWith(prefix) || !update.current.endsWith(suffix)) return false;
        replaces = true;
        return true;
    };
    // The subject's first clause can carry details too, including after a prior
    // correction; apply the same preservation check as for subsequent clauses.
    if (!firstDetail.isEmpty() && !coversDetail(firstDetail)) return false;
    for (const auto& raw : clauses.mid(1))
        if (!coversDetail(raw)) return false;
    return replaces;
}
}
#endif
