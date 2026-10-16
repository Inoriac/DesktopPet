#include "mcp_tool_adapter.h"

#include <utility>

McpToolAdapter::McpToolAdapter(McpToolDescriptor descriptor, McpClient* client)
    : AITool(descriptor.name,
             descriptor.description.isEmpty() ? QString("外部 MCP 工具：%1").arg(descriptor.name) : descriptor.description,
             ToolCategory::Action)
    , m_descriptor(std::move(descriptor))
    , m_client(client) {}

QJsonObject McpToolAdapter::parameterSchema() const {
    if (!m_descriptor.inputSchema.isEmpty()) {
        return m_descriptor.inputSchema;
    }

    QJsonObject schema;
    schema["type"] = "object";
    schema["properties"] = QJsonObject{};
    return schema;
}

ToolResult McpToolAdapter::execute(const QJsonObject& params) {
    if (!m_client || !m_client->isConnected()) return ToolResult::fail("mcp_client_not_connected");
    auto result=m_client->callTool(m_descriptor.name,params,30000);
    if (!result) return ToolResult::fail(result.error().code);
    if (result.value().value("isError").toBool()) return ToolResult::fail("mcp_tool_error");
    return ToolResult::ok(result.value());
}
