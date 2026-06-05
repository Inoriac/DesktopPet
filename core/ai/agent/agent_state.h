#ifndef DESKTOP_PET_AGENT_STATE_H
#define DESKTOP_PET_AGENT_STATE_H

#include <QString>

enum class AgentState {
    Idle,
    Understanding,
    Planning,
    ExecutingTool,
    Observing,
    NeedUserConfirm,
    Finished,
    Failed
};

inline QString agentStateToString(AgentState state) {
    switch (state) {
    case AgentState::Idle:
        return "Idle";
    case AgentState::Understanding:
        return "Understanding";
    case AgentState::Planning:
        return "Planning";
    case AgentState::ExecutingTool:
        return "ExecutingTool";
    case AgentState::Observing:
        return "Observing";
    case AgentState::NeedUserConfirm:
        return "NeedUserConfirm";
    case AgentState::Finished:
        return "Finished";
    case AgentState::Failed:
        return "Failed";
    }
    return "Unknown";
}

inline AgentState agentStateFromString(const QString& value) {
    if (value == "Understanding") return AgentState::Understanding;
    if (value == "Planning") return AgentState::Planning;
    if (value == "ExecutingTool") return AgentState::ExecutingTool;
    if (value == "Observing") return AgentState::Observing;
    if (value == "NeedUserConfirm") return AgentState::NeedUserConfirm;
    if (value == "Finished") return AgentState::Finished;
    if (value == "Failed") return AgentState::Failed;
    return AgentState::Idle;
}

#endif // DESKTOP_PET_AGENT_STATE_H