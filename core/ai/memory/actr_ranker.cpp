#include "actr_ranker.h"
#include "recall_text.h"

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
        
        // C_i: independent text/concept evidence, with no duplicate credit.
        const auto terms = RecallText::tokens(candidate.entry.key + QLatin1Char(' ')
            + candidate.entry.summary + QLatin1Char(' ') + candidate.entry.content
            + QLatin1Char(' ') + candidate.entry.scope);
        candidate.lexicalCue = RecallText::lexicalCoverage(cue.normalizedQuery, cue.tokens,
            QSet<QString>(terms.cbegin(), terms.cend()), cue.tokenWeights);
        candidate.tagCue = RecallText::tagCoverage(cue.knownTags, candidate.entry.tags);
        candidate.cueMatch = m_lexicalCueCoeff * std::max(candidate.lexicalCue, candidate.tagCue)
            / (m_semanticCueCoeff + m_lexicalCueCoeff);
        // Embedding channels provide the semantic part of C_i.  Keep the
        // public entry-only helper for callers without semantic candidates,
        // while folding the similarity into the same fixed [0,1] bound here.
        if (candidate.semanticCue > 0.0) {
            candidate.cueMatch = clamp01(
                candidate.cueMatch
                + m_semanticCueCoeff * clamp01(candidate.semanticCue)
                    / (m_semanticCueCoeff + m_lexicalCueCoeff));
        }
        
        // R_i: runtime activation（已由调用方从 ActiveMemoryPool 填入，
        //      pool 内上限 2.0，这里归一化到 [0,1]）
        candidate.runtimeActivation = clamp01(candidate.runtimeActivation / 2.0);
        
        // E_i: emotion boost
        candidate.emotionBoost = computeEmotionBoost(candidate.entry, cue);
        
        // G_i: graph propagation（Phase 3：由 retrieveWithGraphPropagation 预填）
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
            if (a.entry.updatedAt != b.entry.updatedAt)
                return a.entry.updatedAt > b.entry.updatedAt;
            return a.entry.id < b.entry.id;
        });
    
    return ranked;
}

QList<CandidateMemory> ACTRRanker::select(const QList<CandidateMemory>& candidates,
                                         const MemoryCue& cue, int limit) const {
    if (limit <= 0) return {};
    const auto ranked = rank(candidates, cue);
    QList<CandidateMemory> selected;
    const auto exploration = std::find_if(ranked.cbegin(), ranked.cend(),
        [](const CandidateMemory& candidate) { return candidate.isExploratory; });
    const int stableSlots = limit - (exploration != ranked.cend() ? 1 : 0);
    for (const auto& candidate : ranked) {
        if (selected.size() >= stableSlots) break;
        if (!candidate.isExploratory) selected.append(candidate);
    }
    if (exploration != ranked.cend()) selected.append(*exploration);
    return selected;
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
    const auto tokens = RecallText::tokens(entry.key + QLatin1Char(' ') + entry.summary
        + QLatin1Char(' ') + entry.content + QLatin1Char(' ') + entry.scope);
    const double lexical = RecallText::lexicalCoverage(cue.normalizedQuery, cue.tokens,
        QSet<QString>(tokens.cbegin(), tokens.cend()), cue.tokenWeights);
    const double tag = RecallText::tagCoverage(cue.knownTags, entry.tags);
    // Correlated text/tag evidence competes for one bounded contribution.
    return m_lexicalCueCoeff * std::max(lexical, tag) / (m_semanticCueCoeff + m_lexicalCueCoeff);
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
