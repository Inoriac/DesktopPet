#include "actr_ranker.h"

#include <algorithm>
#include <cmath>

namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

}

ACTRRanker::ACTRRanker() = default;

void ACTRRanker::setWeights(double baseLevel,
                            double cueMatch,
                            double runtime,
                            double emotion,
                            double graph) {
    m_baseLevelWeight = baseLevel;
    m_cueMatchWeight = cueMatch;
    m_runtimeWeight = runtime;
    m_emotionWeight = emotion;
    m_graphWeight = graph;
}

QList<CandidateMemory> ACTRRanker::rank(const QList<CandidateMemory>& candidates,
                                        const MemoryCue& cue) const {
    QList<CandidateMemory> ranked = candidates;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    
    for (CandidateMemory& candidate : ranked) {
        // B_i: base-level activation（固定边界归一化，禁止按当轮候选集 min-max）
        candidate.baseActivation = computeBaseActivation(candidate.entry, now);
        
        // C_i: cue match（semantic 通道 Phase 2 暂缺，只有词法/标签）
        candidate.cueMatch = computeCueMatch(candidate.entry, cue);
        
        // R_i: runtime activation（已由调用方从 ActiveMemoryPool 填入，
        //      pool 内上限 2.0，这里归一化到 [0,1]）
        candidate.runtimeActivation = clamp01(candidate.runtimeActivation / 2.0);
        
        // E_i: emotion boost
        candidate.emotionBoost = computeEmotionBoost(candidate.entry, cue);
        
        // G_i: graph propagation（Phase 3，当前恒 0，调用方可预填）
        candidate.graphActivation = clamp01(candidate.graphActivation);
        
        // A_i = 1.0*B + 1.0*C + 1.5*R + 0.3*E + 0.6*G（设计 §7）
        candidate.finalScore = m_baseLevelWeight * candidate.baseActivation
                             + m_cueMatchWeight * candidate.cueMatch
                             + m_runtimeWeight * candidate.runtimeActivation
                             + m_emotionWeight * candidate.emotionBoost
                             + m_graphWeight * candidate.graphActivation;
    }
    
    std::sort(ranked.begin(), ranked.end(),
        [](const CandidateMemory& a, const CandidateMemory& b) {
            if (std::abs(a.finalScore - b.finalScore) > 0.0001) {
                return a.finalScore > b.finalScore;
            }
            return a.entry.updatedAt > b.entry.updatedAt;
        });
    
    return ranked;
}

double ACTRRanker::computeBaseActivation(const MemoryEntry& entry,
                                         const QDateTime& now) const {
    // B_i = strength + importance + ln(1 + accessTerm)
    // accessTerm 近似：accessCount * (1 + accessAgeHours)^-0.5
    // （完整版应批量读取 memory_access_log 最近 32 次；Phase 2 先用
    //   accessCount + lastAccessedAt 近似，误差被 ln 压缩，可接受）
    
    const double strength = clamp01(entry.strength);
    const double importance = clamp01(entry.importance) * m_importanceCoeff;
    
    double accessTerm = 0.0;
    if (entry.accessCount > 0 && entry.lastAccessedAt.isValid() && now.isValid()) {
        const double ageHours = std::max(0.0,
            entry.lastAccessedAt.secsTo(now) / 3600.0);
        accessTerm = entry.accessCount * std::pow(1.0 + ageHours, m_accessDecayExp);
    }
    
    // ln(1+x)/3 封顶 1.0：accessTerm ≈ 19 时达到饱和
    const double accessComponent = std::min(1.0, std::log(1.0 + accessTerm) / 3.0);
    
    // 固定边界归一化：三个分量均在 [0,1]，除以 3
    return clamp01((strength + importance + accessComponent) / 3.0);
}

double ACTRRanker::computeCueMatch(const MemoryEntry& entry,
                                   const MemoryCue& cue) const {
    // 词法线索：cue tokens 命中率
    double lexicalCue = 0.0;
    if (!cue.tokens.isEmpty()) {
        const QString searchText = (entry.key + QLatin1Char(' ')
                                   + entry.summary + QLatin1Char(' ')
                                   + entry.content + QLatin1Char(' ')
                                   + entry.tags.join(QLatin1Char(' '))).toLower();
        int hits = 0;
        for (const QString& token : cue.tokens) {
            if (searchText.contains(token)) ++hits;
        }
        lexicalCue = static_cast<double>(hits) / cue.tokens.size();
    }
    
    // 标签直接命中提升词法线索（封顶 1.0）
    if (!cue.knownTags.isEmpty()) {
        for (const QString& tag : cue.knownTags) {
            if (entry.tags.contains(tag, Qt::CaseInsensitive)) {
                lexicalCue = std::min(1.0, lexicalCue + 0.3);
            }
        }
    }
    
    // 语义线索：Phase 2 无 HNSW，恒 0；Phase 3+ 由调用方通过
    // CandidateMemory 预填后在此合并（TODO）
    const double semanticCue = 0.0;
    
    // C_i = 2.0*semantic + 1.2*lexical，固定上界 3.2 归一化
    const double raw = m_semanticCueCoeff * semanticCue
                     + m_lexicalCueCoeff * lexicalCue;
    return clamp01(raw / (m_semanticCueCoeff + m_lexicalCueCoeff));
}

double ACTRRanker::computeEmotionBoost(const MemoryEntry& entry,
                                       const MemoryCue& cue) const {
    // E_i = emotionMatch * currentIntensity * memoryEmotionConfidence
    if (cue.currentEmotion == EmotionType::Neutral) return 0.0;
    if (entry.emotion == EmotionType::Neutral) return 0.0;
    if (entry.emotion != cue.currentEmotion) return 0.0;
    
    const double currentIntensity = clamp01(cue.emotionIntensity);
    const double memoryConfidence = clamp01(entry.emotionConfidence);
    
    return clamp01(currentIntensity * memoryConfidence);
}
