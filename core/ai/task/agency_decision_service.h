#pragma once
#include "task_types.h"

struct AgencyDecision {
    QString decision;
    QString reason;
    QString message;
};

// Request/validation policy, without tool access. Orchestrator owns cancellation/time budgets.
class AgencyDecisionService {
public:
    static TaskModelRequest request(const TaskRecord& task, bool checkpoint = false);
    static bool parse(const QJsonObject& output, bool checkpoint, AgencyDecision* decision);
};
