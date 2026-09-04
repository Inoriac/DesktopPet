#ifndef DESKTOP_PET_ASSOCIATIVE_ACTIVATION_ENGINE_H
#define DESKTOP_PET_ASSOCIATIVE_ACTIVATION_ENGINE_H

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

#include "memory_relation.h"

class MemoryRelationGraph;
class TagCooccurrenceGraph;

struct PropagatedMemory {
    QString memoryId;
    double activation = 0.0;
    QStringList propagationPath;  // ["seed_id", "hop1_id", "hop2_id"]
    int hopCount = 0;
    bool isExploratory = false;
};

struct PersonalityParams {
    double openness = 0.5;      // 0-1, affects exploration rate & temperature
    double sociability = 0.5;   // 0-1, boosts Relationship/MentionedWith
    double initiative = 0.5;    // 0-1, boosts task/goal edges
};

// 图谱激活传播引擎（设计 §11/§12）。
// 两跳传播、环路防护、人格化探索（Softmax 采样）、候选预算 64。
// 关系类型系数：DerivedFrom=1.0, CreatedTask=0.85, TopicOf=0.8,
// ConflictsWith=0.7, Related=0.6, MentionedWith=0.3。
class AssociativeActivationEngine {
public:
    AssociativeActivationEngine();
    
    // Configure propagation
    void setMaxHops(int hops) { m_maxHops = hops; }
    void setMaxCandidates(int count) { m_maxCandidates = count; }
    void setMinPropagationDelta(double delta) { m_minDelta = delta; }
    void setHopDecay(double decay) { m_hopDecay = decay; }
    
    // Configure personality-based exploration
    void setPersonality(const PersonalityParams& params);
    void setExplorationBaseline(double baseline) { m_explorationBaseline = baseline; }
    void setExplorationOpennessRange(double range) { m_explorationRange = range; }
    void setTemperatureBase(double base) { m_temperatureBase = base; }
    void setTemperatureOpennessScale(double scale) { m_temperatureScale = scale; }
    
    // Propagate from seed memories with initial activations
    // Returns expanded candidate set with propagation paths
    QList<PropagatedMemory> propagate(
        const QHash<QString, double>& seedActivations,
        const MemoryRelationGraph& relationGraph,
        const TagCooccurrenceGraph* tagGraph = nullptr,
        const QStringList& seedTags = {}) const;
    
    // Set random source for testing (nullptr = default QRandomGenerator)
    void setRandomSource(std::function<double()> randomFunc);
    
private:
    struct FrontierNode {
        QString memoryId;
        double activation;
        QStringList path;
        int hop;
        
        bool operator<(const FrontierNode& other) const {
            return activation < other.activation;  // Max-heap
        }
    };
    
    double computeRelationTypeFactor(MemoryRelationType type,
                                     const PersonalityParams& personality) const;
    double computePropagationDelta(double sourceActivation,
                                   double edgeWeight,
                                   double edgeConfidence,
                                   double relationFactor,
                                   int hop,
                                   int traversableDegree) const;
    double sigmoid(double x) const;
    
    // Personality-based exploration
    double computeExplorationRate(double openness) const;
    double computeTemperature(double openness) const;
    bool shouldExplore(double explorationRate) const;
    QString sampleExploratory(const QList<FrontierNode>& frontier,
                             double temperature) const;
    
    // Default parameters (design §11/§12)
    int m_maxHops = 2;
    int m_maxCandidates = 64;
    double m_minDelta = 0.08;
    double m_hopDecay = 0.55;  // 0.55^hop
    
    // Personality exploration
    PersonalityParams m_personality;
    double m_explorationBaseline = 0.10;
    double m_explorationRange = 0.15;
    double m_temperatureBase = 0.10;
    double m_temperatureScale = 0.40;
    
    // Random source (nullptr = use default)
    mutable std::function<double()> m_randomSource;
};

#endif // DESKTOP_PET_ASSOCIATIVE_ACTIVATION_ENGINE_H
