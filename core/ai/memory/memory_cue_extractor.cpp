#include "memory_cue_extractor.h"

#include <QRegularExpression>

MemoryCueExtractor::MemoryCueExtractor() = default;

MemoryCue MemoryCueExtractor::extractFromQuery(const QString& queryText) const {
    MemoryCue cue;
    
    cue.normalizedQuery = normalize(queryText);
    cue.tokens = tokenize(cue.normalizedQuery);
    cue.knownTags = matchKnownTags(cue.tokens);
    
    // Enrich with session context
    cue.sessionTopic = m_sessionTopic;
    cue.activeGoals = m_activeGoals;
    cue.currentEmotion = m_currentEmotion;
    cue.emotionIntensity = m_emotionIntensity;
    cue.openness = m_openness;
    cue.sociability = m_sociability;
    cue.initiative = m_initiative;
    
    return cue;
}

void MemoryCueExtractor::setSessionContext(const QString& topic,
                                            const QStringList& goals) {
    m_sessionTopic = topic;
    m_activeGoals = goals;
}

void MemoryCueExtractor::setEmotionContext(EmotionType emotion, double intensity) {
    m_currentEmotion = emotion;
    m_emotionIntensity = qBound(0.0, intensity, 1.0);
}

void MemoryCueExtractor::setPersonalityParameters(double openness,
                                                   double sociability,
                                                   double initiative) {
    m_openness = qBound(0.0, openness, 1.0);
    m_sociability = qBound(0.0, sociability, 1.0);
    m_initiative = qBound(0.0, initiative, 1.0);
}

void MemoryCueExtractor::setKnownTags(const QSet<QString>& tags) {
    m_knownTags = tags;
}

QString MemoryCueExtractor::normalize(const QString& text) const {
    QString normalized = text.trimmed().toLower();
    // Collapse whitespace
    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return normalized;
}

QStringList MemoryCueExtractor::tokenize(const QString& text) const {
    QStringList tokens;
    
    // Latin words
    tokens.append(extractLatinWords(text));
    
    // CJK bigrams (primary for Chinese)
    tokens.append(extractCJKNGrams(text, 2));
    
    // CJK trigrams (for longer phrases)
    tokens.append(extractCJKNGrams(text, 3));
    
    // Deduplicate
    QSet<QString> seen;
    QStringList unique;
    for (const QString& token : tokens) {
        if (!seen.contains(token)) {
            seen.insert(token);
            unique.append(token);
        }
    }
    
    return unique;
}

QStringList MemoryCueExtractor::extractLatinWords(const QString& text) const {
    QStringList words;
    static const QRegularExpression latinWordRe(QStringLiteral("[a-z0-9]+"));
    
    QRegularExpressionMatchIterator it = latinWordRe.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        const QString word = match.captured(0);
        if (word.length() >= 2) {  // Skip single chars
            words.append(word);
        }
    }
    
    return words;
}

QStringList MemoryCueExtractor::extractCJKNGrams(const QString& text, int n) const {
    QStringList ngrams;
    
    // Extract CJK character sequences
    QString cjkBuffer;
    for (const QChar& ch : text) {
        const ushort code = ch.unicode();
        // CJK Unified Ideographs range (basic)
        const bool isCJK = (code >= 0x4E00 && code <= 0x9FFF);
        
        if (isCJK) {
            cjkBuffer.append(ch);
        } else {
            // Process accumulated buffer
            if (cjkBuffer.length() >= n) {
                for (int i = 0; i + n <= cjkBuffer.length(); ++i) {
                    ngrams.append(cjkBuffer.mid(i, n));
                }
            }
            cjkBuffer.clear();
        }
    }
    
    // Process final buffer
    if (cjkBuffer.length() >= n) {
        for (int i = 0; i + n <= cjkBuffer.length(); ++i) {
            ngrams.append(cjkBuffer.mid(i, n));
        }
    }
    
    return ngrams;
}

QStringList MemoryCueExtractor::matchKnownTags(const QStringList& tokens) const {
    QStringList matched;
    
    for (const QString& token : tokens) {
        if (m_knownTags.contains(token)) {
            matched.append(token);
        }
    }
    
    return matched;
}
