#ifndef DESKTOP_PET_MEMORY_CUE_EXTRACTOR_H
#define DESKTOP_PET_MEMORY_CUE_EXTRACTOR_H

#include <QString>
#include <QStringList>
#include <QSet>

#include "emotion/emotion_types.h"

struct MemoryCue {
    QString normalizedQuery;
    QStringList tokens;           // Latin words + CJK bigrams/trigrams
    QStringList knownTags;        // Extracted known tags
    QStringList entities;         // Extracted entities (future)
    
    // Context from current session
    QString sessionTopic;
    QStringList activeGoals;
    EmotionType currentEmotion = EmotionType::Neutral;
    double emotionIntensity = 0.0;
    
    // Personality parameters (from PersonaProjection)
    double openness = 0.5;
    double sociability = 0.5;
    double initiative = 0.5;
};

class MemoryCueExtractor {
public:
    MemoryCueExtractor();
    
    // Extract cues from user query text
    MemoryCue extractFromQuery(const QString& queryText) const;
    
    // Set context for enrichment
    void setSessionContext(const QString& topic, const QStringList& goals);
    void setEmotionContext(EmotionType emotion, double intensity);
    void setPersonalityParameters(double openness, double sociability, double initiative);
    
    // Register known tags for matching
    void setKnownTags(const QSet<QString>& tags);
    
private:
    QString normalize(const QString& text) const;
    QStringList tokenize(const QString& text) const;
    QStringList extractLatinWords(const QString& text) const;
    QStringList extractCJKNGrams(const QString& text, int n = 2) const;
    QStringList matchKnownTags(const QStringList& tokens) const;
    
    // Context state
    QString m_sessionTopic;
    QStringList m_activeGoals;
    EmotionType m_currentEmotion = EmotionType::Neutral;
    double m_emotionIntensity = 0.0;
    double m_openness = 0.5;
    double m_sociability = 0.5;
    double m_initiative = 0.5;
    
    QSet<QString> m_knownTags;
};

#endif // DESKTOP_PET_MEMORY_CUE_EXTRACTOR_H
