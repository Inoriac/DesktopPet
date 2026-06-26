//
// Memory maintenance tools
//

#ifndef DESKTOP_PET_MEMORY_TOOLS_H
#define DESKTOP_PET_MEMORY_TOOLS_H

#include "../ai_tool.h"

#include <QDateTime>

class MemoryStore;

class MemoryOrganizeTool : public AITool {
public:
    explicit MemoryOrganizeTool(MemoryStore* memoryStore = nullptr);

    QJsonObject parameterSchema() const override;
    bool validate(const QJsonObject& params) const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    MemoryStore* m_memoryStore = nullptr;
    QDateTime m_lastAppliedAt;
};

#endif // DESKTOP_PET_MEMORY_TOOLS_H
