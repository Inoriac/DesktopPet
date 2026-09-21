#include "memory_cue_extractor.h"
#include "recall_text.h"

MemoryCueExtractor::MemoryCueExtractor() = default;

MemoryCue MemoryCueExtractor::extractFromQuery(const QString& queryText) const {
    MemoryCue cue;
    cue.normalizedQuery = RecallText::normalize(queryText);
    cue.tokens = RecallText::tokens(cue.normalizedQuery);
    cue.knownTags = matchKnownTags(cue.normalizedQuery);
    cue.sessionTopic = m_sessionTopic;
    cue.activeGoals = m_activeGoals;
    cue.currentEmotion = m_currentEmotion;
    cue.emotionIntensity = m_emotionIntensity;
    cue.openness = m_openness;
    cue.sociability = m_sociability;
    cue.initiative = m_initiative;
    return cue;
}
void MemoryCueExtractor::setSessionContext(const QString& topic, const QStringList& goals) {
    m_sessionTopic = topic;
    m_activeGoals = goals;
}
void MemoryCueExtractor::setEmotionContext(EmotionType emotion, double intensity) {
    m_currentEmotion = emotion;
    m_emotionIntensity = qBound(0.0, intensity, 1.0);
}
void MemoryCueExtractor::setPersonalityParameters(double openness, double sociability, double initiative) {
    m_openness = qBound(0.0, openness, 1.0);
    m_sociability = qBound(0.0, sociability, 1.0);
    m_initiative = qBound(0.0, initiative, 1.0);
}
void MemoryCueExtractor::setKnownTags(const QSet<QString>& tags) {
    QSet<QString> normalized;
    for (const auto& tag : tags) {
        const auto key = RecallText::normalize(tag);
        if (!key.isEmpty()) normalized.insert(key);
    }
    if (normalized == m_knownTags) return;
    m_knownTags = normalized;
    m_tagTrie = {TagNode{}};
    for (const auto& tag : normalized) {
        int node = 0;
        for (QChar ch : tag) {
            int child = m_tagTrie[node].children.value(ch, -1);
            if (child < 0) {
                child = m_tagTrie.size();
                m_tagTrie[node].children.insert(ch, child);
                m_tagTrie.append(TagNode{});
            }
            node = child;
        }
        m_tagTrie[node].tag = tag;
    }
}
QStringList MemoryCueExtractor::matchKnownTags(const QString& query) const {
    QList<RecallText::Span> matches;
    for (int start = 0; start < query.size(); ++start) {
        int node = 0;
        for (int end = start; end < query.size(); ++end) {
            node = m_tagTrie[node].children.value(query.at(end), -1);
            if (node < 0) break;
            const auto& tag = m_tagTrie[node].tag;
            if (!tag.isEmpty() && RecallText::boundaries(query, start, end - start + 1))
                matches.append({tag, start, end - start + 1});
        }
    }
    // Longest non-overlapping concepts win; repeated mentions count once.
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        return a.length != b.length ? a.length > b.length : a.start < b.start;
    });
    QVector<bool> covered(query.size(), false);
    QStringList result;
    for (const auto& match : matches) {
        bool overlap = false;
        for (int i = match.start; i < match.start + match.length; ++i) overlap |= covered[i];
        if (overlap) continue;
        for (int i = match.start; i < match.start + match.length; ++i) covered[i] = true;
        if (!result.contains(match.token)) result.append(match.token);
        if (result.size() >= 32) break;
    }
    return result;
}
