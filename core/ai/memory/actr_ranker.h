#ifndef DESKTOP_PET_ACTR_RANKER_H
#define DESKTOP_PET_ACTR_RANKER_H

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

#include "memory_types.h"
#include "memory_cue_extractor.h"

struct CandidateMemory {
    MemoryEntry entry;
    double baseActivation = 0.0;      // B_i: strength + importance + access history
    double cueMatch = 0.0;            // C_i: semantic + lexical/tag cues
    double runtimeActivation = 0.0;   // R_i: from ActiveMemoryPool
    double emotionBoost = 0.0;        // E_i: emotion match
    double graphActivation = 0.0;     // G_i: graph propagation (Phase 3)
    double finalScore = 0.0;          // Weighted sum
    
    QStringList sourceChannels;       // e.g., ["active_pool", "hnsw", "keyword"]
    bool isExploratory = false;
};

// ACT-R 启发式精排器（设计文档 §7）。
// 不完整复刻 ACT-R 架构，只取激活传播模型用于记忆召回排序。
class ACTRRanker {
public:
    ACTRRanker();
    
    // Configure weights (defaults from design §7)
    void setWeights(double baseLevel = 1.0,
                    double cueMatch = 1.0,
                    double runtime = 1.5,
                    double emotion = 0.3,
                    double graph = 0.6);
    
    void setSemanticCueCoeff(double coeff) { m_semanticCueCoeff = coeff; }
    void setLexicalCueCoeff(double coeff) { m_lexicalCueCoeff = coeff; }
    
    // Rank candidates using ACT-R activation model
    QList<CandidateMemory> rank(const QList<CandidateMemory>& candidates,
                                const MemoryCue& cue) const;
    
    // Compute individual components (for testing/observability)
    double computeBaseActivation(const MemoryEntry& entry,
                                 const QDateTime& now) const;
    double computeCueMatch(const MemoryEntry& entry,
                           const MemoryCue& cue) const;
    double computeEmotionBoost(const MemoryEntry& entry,
                               const MemoryCue& cue) const;
    
private:
    // ACT-R weights
    double m_baseLevelWeight = 1.0;
    double m_cueMatchWeight = 1.0;
    double m_runtimeWeight = 1.5;
    double m_emotionWeight = 0.3;
    double m_graphWeight = 0.6;
    
    // Cue coefficients
    double m_semanticCueCoeff = 2.0;
    double m_lexicalCueCoeff = 1.2;
    
    // Access history coefficients
    double m_importanceCoeff = 1.0;
    double m_accessDecayExp = -0.5;
};

#endif // DESKTOP_PET_ACTR_RANKER_H
