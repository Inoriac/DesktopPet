#ifndef DESKTOP_PET_RECALL_TEXT_H
#define DESKTOP_PET_RECALL_TEXT_H
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <algorithm>

// Shared lexical analysis; tokens never become concept tags implicitly.
namespace RecallText {
inline QString normalize(const QString& text) {
    return text.normalized(QString::NormalizationForm_KC).toCaseFolded().simplified();
}
inline bool cjk(QChar ch) {
    return (ch.unicode() >= 0x3400 && ch.unicode() <= 0x9fff)
        || (ch.unicode() >= 0xf900 && ch.unicode() <= 0xfaff);
}
inline bool wordChar(QChar ch) {
    return !cjk(ch) && (ch.isLetterOrNumber() || ch == QLatin1Char('_'));
}
inline bool boundaries(const QString& text, int start, int length) {
    const int end = start + length;
    return !(wordChar(text.at(start)) && start > 0 && wordChar(text.at(start - 1)))
        && !(wordChar(text.at(end - 1)) && end < text.size() && wordChar(text.at(end)));
}
struct Span { QString token; int start; int length; };
inline QList<Span> spans(const QString& normalized) {
    QList<Span> result;
    for (int start = 0; start < normalized.size();) {
        int end = start + 1;
        if (cjk(normalized.at(start))) {
            while (end < normalized.size() && cjk(normalized.at(end))) ++end;
            for (int n : {2, 3})
                for (int i = start; i + n <= end; ++i)
                    result.append({normalized.mid(i, n), i, n});
        } else if (wordChar(normalized.at(start))) {
            while (end < normalized.size() && wordChar(normalized.at(end))) ++end;
            if (end - start >= 2) result.append({normalized.mid(start, end - start), start, end - start});
        }
        start = end;
    }
    return result;
}
inline QStringList tokens(const QString& text) {
    QStringList result;
    QSet<QString> seen;
    for (const auto& span : spans(normalize(text))) {
        if (!seen.contains(span.token)) { seen.insert(span.token); result.append(span.token); }
    }
    return result;
}
// Each query character contributes at most once despite overlapping n-grams.
inline double lexicalCoverage(const QString& query, const QStringList& queryTokens,
                              const QSet<QString>& documentTokens,
                              const QHash<QString, double>& weights = {}) {
    const QString normalized = normalize(query.isEmpty() ? queryTokens.join(QLatin1Char(' ')) : query);
    QSet<QString> allowed;
    for (const auto& token : queryTokens) allowed.insert(normalize(token));
    QVector<double> total(normalized.size(), 0.0), matched(normalized.size(), 0.0);
    for (const auto& span : spans(normalized)) {
        if (!allowed.contains(span.token)) continue;
        const double weight = weights.value(span.token, 1.0);
        for (int i = span.start; i < span.start + span.length; ++i) {
            total[i] = std::max(total[i], weight);
            if (documentTokens.contains(span.token)) matched[i] = std::max(matched[i], weight);
        }
    }
    double denominator = 0.0, numerator = 0.0;
    for (int i = 0; i < total.size(); ++i) { denominator += total[i]; numerator += matched[i]; }
    return denominator > 0.0 ? numerator / denominator : 0.0;
}
inline double tagCoverage(const QStringList& matchedTags, const QStringList& documentTags) {
    QSet<QString> queryTags, tags;
    for (const auto& tag : matchedTags) if (!normalize(tag).isEmpty()) queryTags.insert(normalize(tag));
    for (const auto& tag : documentTags) tags.insert(normalize(tag));
    if (queryTags.isEmpty()) return 0.0;
    int hits = 0;
    for (const auto& tag : queryTags) if (tags.contains(tag)) ++hits;
    return double(hits) / queryTags.size();
}
}
#endif
