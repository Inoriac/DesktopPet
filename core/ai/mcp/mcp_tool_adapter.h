#ifndef DESKTOP_PET_MCP_TOOL_ADAPTER_H
#define DESKTOP_PET_MCP_TOOL_ADAPTER_H

#include "ai/ai_tool.h"
#include "mcp_client.h"

class McpToolAdapter : public AITool {
public:
    McpToolAdapter(McpToolDescriptor descriptor, McpClient* client);

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    McpToolDescriptor m_descriptor;
    McpClient* m_client = nullptr; // non-owning
};

#endif // DESKTOP_PET_MCP_TOOL_ADAPTER_H