#include "associative_activation_engine.h"

#include <QRandomGenerator>
#include <QSet>
#include <QtMath>
#include <algorithm>
#include <queue>

#include "memory_relation_graph.h"
#include "tag_cooccurrence_graph.h"

AssociativeActivationEngine::AssociativeActivationEngine() = default;

void AssociativeActivationEngine::setPersonality(const PersonalityParams& params) {
    m_personality = params;
}

void AssociativeActivationEngine::setRandomSource(std::function<double()> randomFunc) {
    m_randomSource = randomFunc;
}

double AssociativeActivationEngine::sigmoid(double x) const {
    return 1.0 / (1.0 + qExp(-x));
}

double AssociativeActivationEngine::computeRelationTypeFactor(
    MemoryRelationType type,
    const PersonalityParams& personality) const {
    
    // Base relation type coefficients (design §11)
    double baseFactor = 0.6;
    switch (type) {
    case MemoryRelationType::DerivedFrom:
        baseFactor = 1.0;
        break;
    case MemoryRelationType::CreatedTask:
        baseFactor = 0.85;
        break;
    case MemoryRelationType::TopicOf:
        baseFactor = 0.8;
        break;
    case MemoryRelationType::ConflictsWith:
        baseFactor = 0.7;
        break;
    case MemoryRelationType::Related:
        baseFactor = 0.6;
        break;
    case MemoryRelationType::MentionedWith:
        baseFactor = 0.3;
        break;
    case MemoryRelationType::Supersedes:
        // Supersedes uses directed resolution, not diffusion (design §11)
        baseFactor = 0.0;
        break;
    }
    
    // Personality modulation (design §12)
    // sociability boosts MentionedWith/Related, initiative boosts CreatedTask
    // Max 15% per dimension
    double modulation = 1.0;
    
    if (type == MemoryRelationType::MentionedWith ||
        type == MemoryRelationType::Related) {
        // sociability: 0.5 = no change, >0.5 = boost up to 15%
        const double sociabilityBoost = (personality.sociability - 0.5) * 0.3;
        modulation += qBound(-0.15, sociabilityBoost, 0.15);
    }
    
    if (type == MemoryRelationType::CreatedTask) {
        // initiative: 0.5 = no change, >0.5 = boost up to 15%
        const double initiativeBoost = (personality.initiative - 0.5) * 0.3;
        modulation += qBound(-0.15, initiativeBoost, 0.15);
    }
    
    return baseFactor * modulation;
}

double AssociativeActivationEngine::computePropagationDelta(
    double sourceActivation,
    double edgeWeight,
    double edgeConfidence,
    double relationFactor,
    int hop,
    int traversableDegree) const {
    
    // Design §11:
    // delta = sigmoid(sourceActivation)
    //       * edge.weight
    //       * edge.confidence
    //       * relationTypeFactor
    //       * 0.55^hop
    //       / sqrt(max(1, traversableDegree))
    
    const double hopDecayFactor = qPow(m_hopDecay, hop);
    const double degreePenalty = qSqrt(qMax(1, traversableDegree));
    
    const double delta = sigmoid(sourceActivation)
                       * edgeWeight
                       * edgeConfidence
                       * relationFactor
                       * hopDecayFactor
                       / degreePenalty;
    
    return delta;
}

double AssociativeActivationEngine::computeExplorationRate(double openness) const {
    // Design §12: explorationRate = clamp(0.10 + 0.15 * openness, 0.10, 0.25)
    const double rate = m_explorationBaseline + m_explorationRange * openness;
    return qBound(0.10, rate, 0.25);
}

double AssociativeActivationEngine::computeTemperature(double openness) const {
    // Design §12: temperature = 0.10 + 0.40 * openness
    // Neutral personality (0.5) => 0.30
    return m_temperatureBase + m_temperatureScale * openness;
}

bool AssociativeActivationEngine::shouldExplore(double explorationRate) const {
    const double rand = m_randomSource ? m_randomSource() 
                                       : QRandomGenerator::global()->generateDouble();
    return rand < explorationRate;
}

int AssociativeActivationEngine::sampleExploratory(
    const QList<FrontierNode>& frontier,
    double temperature) const {
    
    if (frontier.isEmpty()) {
        return -1;
    }
    
    // Softmax sampling (design §12)
    // P(i) = exp(activation_i / temperature) / sum(exp(activation_j / temperature))
    
    QList<double> weights;
    double sumExp = 0.0;
    
    for (const FrontierNode& node : frontier) {
        const double expVal = qExp(node.activation / temperature);
        weights.append(expVal);
        sumExp += expVal;
    }
    
    // Sample using cumulative distribution
    const double rand = m_randomSource ? m_randomSource() 
                                       : QRandomGenerator::global()->generateDouble();
    double cumulative = 0.0;
    
    for (int i = 0; i < frontier.size(); ++i) {
        cumulative += weights[i] / sumExp;
        if (rand <= cumulative) {
            return i;
        }
    }
    
    // Fallback to last (should not reach here)
    return frontier.size() - 1;
}

QList<PropagatedMemory> AssociativeActivationEngine::propagate(
    const QHash<QString, double>& seedActivations,
    const MemoryRelationGraph& relationGraph,
    const TagCooccurrenceGraph* tagGraph,
    const QStringList& seedTags) const {
    
    // Initialize result set with seeds
    QHash<QString, PropagatedMemory> results;
    for (auto it = seedActivations.constBegin(); it != seedActivations.constEnd(); ++it) {
        PropagatedMemory mem;
        mem.memoryId = it.key();
        mem.activation = it.value();
        mem.propagationPath = {it.key()};
        mem.hopCount = 0;
        mem.isExploratory = false;
        results[it.key()] = mem;
    }
    
    // Priority queue for frontier (max-heap by activation)
    auto cmp = [](const FrontierNode& a, const FrontierNode& b) {
        if (a.activation != b.activation) return a.activation < b.activation;
        return a.path > b.path;
    };
    std::priority_queue<FrontierNode, std::vector<FrontierNode>, decltype(cmp)> frontier(cmp);
    
    // Initialize frontier with seeds
    for (auto it = seedActivations.constBegin(); it != seedActivations.constEnd(); ++it) {
        FrontierNode node;
        node.memoryId = it.key();
        node.activation = it.value();
        node.path = {it.key()};
        node.hop = 0;
        frontier.push(node);
    }
    
    // Personality-based exploration
    const double explorationRate = computeExplorationRate(m_personality.openness);
    const double temperature = computeTemperature(m_personality.openness);
    
    // Two-hop propagation with budget
    while (!frontier.empty()) {
        // Decide: exploit (highest activation) or explore (Softmax sample)
        FrontierNode current;
        const bool isExploratoryStep = shouldExplore(explorationRate);
        
        if (isExploratoryStep && frontier.size() > 1) {
            // Build temporary list for sampling
            QList<FrontierNode> frontierList;
            std::priority_queue<FrontierNode, std::vector<FrontierNode>, decltype(cmp)> tempQueue = frontier;
            while (!tempQueue.empty()) {
                frontierList.append(tempQueue.top());
                tempQueue.pop();
            }
            
            const int sampledIndex = sampleExploratory(frontierList, temperature);
            
            // Find and remove sampled node
            std::priority_queue<FrontierNode, std::vector<FrontierNode>, decltype(cmp)> newFrontier(cmp);
            bool found = false;
            int index = 0;
            while (!frontier.empty()) {
                FrontierNode node = frontier.top();
                frontier.pop();
                // Distinguish different paths to the same memory.
                if (index++ == sampledIndex) {
                    current = node;
                    found = true;
                } else {
                    newFrontier.push(node);
                }
            }
            frontier = newFrontier;
            
            if (!found) {
                // Fallback if sampling failed
                if (frontier.empty()) break;
                current = frontier.top();
                frontier.pop();
            }
        } else {
            // Exploit: take highest activation
            current = frontier.top();
            frontier.pop();
        }
        
        if (isExploratoryStep && current.hop > 0) {
            results[current.memoryId].isExploratory = true;
            current.isExploratory = true;
        }

        // Stop if hop limit reached
        if (current.hop >= m_maxHops) {
            continue;
        }
        
        // Get neighbors from relation graph
        const QList<MemoryRelation> neighbors = relationGraph.neighborsOf(current.memoryId);
        
        for (const MemoryRelation& edge : neighbors) {

            // Determine target node (could be from or to)
            const QString targetId = (edge.fromMemoryId == current.memoryId)
                ? edge.toMemoryId
                : edge.fromMemoryId;
            
            // Reject cycles on this path, not independent converging paths.
            // Seeds keep their supplied activation rather than receiving feedback.
            if (current.path.contains(targetId) || seedActivations.contains(targetId)) {
                continue;
            }
            
            if (!results.contains(targetId) && results.size() >= m_maxCandidates) continue;

            // Compute relation type factor
            const double relationFactor = computeRelationTypeFactor(edge.type, m_personality);
            
            // Skip Supersedes edges (special handling, design §11)
            if (edge.type == MemoryRelationType::Supersedes) {
                continue;
            }
            
            // Compute propagation delta
            const int traversableDegree = neighbors.size();
            const double delta = computePropagationDelta(
                current.activation,
                edge.weight,
                edge.confidence,
                relationFactor,
                current.hop + 1,
                traversableDegree
            );
            
            // Skip if delta too small
            if (delta < m_minDelta) {
                continue;
            }
            
            const bool existing = results.contains(targetId);
            PropagatedMemory& mem = results[targetId];
            mem.memoryId = targetId;
            mem.activation = qMin(1.0, mem.activation + delta);
            // Keep the shortest explanation, independent of arrival order.
            QStringList path = current.path;
            path.append(targetId);
            if (!existing || path.size() < mem.propagationPath.size()
                || (path.size() == mem.propagationPath.size() && path < mem.propagationPath)) {
                mem.propagationPath = path;
                mem.hopCount = current.hop + 1;
            }
            mem.isExploratory = mem.isExploratory || current.isExploratory || isExploratoryStep;

            // Propagate this path's delta, never the accumulated total: otherwise
            // earlier paths would be counted again each time another arrives.
            FrontierNode nextNode;
            nextNode.memoryId = targetId;
            nextNode.activation = delta;
            nextNode.path = path;
            nextNode.hop = current.hop + 1;
            nextNode.isExploratory = current.isExploratory || isExploratoryStep;
            // Terminal paths already contributed; do not queue them for an
            // expansion that cannot run (especially on dense converging graphs).
            if (nextNode.hop < m_maxHops) frontier.push(nextNode);
        }
    }
    
    // Convert to list and sort by activation
    QList<PropagatedMemory> resultList = results.values();
    std::sort(resultList.begin(), resultList.end(),
        [](const PropagatedMemory& a, const PropagatedMemory& b) {
            if (a.activation != b.activation) return a.activation > b.activation;
            return a.memoryId < b.memoryId;
        });
    
    // Limit to max candidates
    if (resultList.size() > m_maxCandidates) {
        resultList = resultList.mid(0, m_maxCandidates);
    }
    
    return resultList;
}
