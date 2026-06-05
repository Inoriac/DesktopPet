#include "mcp_client.h"

McpClient::McpClient(QObject* parent)
    : QObject(parent) {}

void McpClient::setServerName(const QString& serverName) {
    m_serverName = serverName;
}

void McpClient::setPrototypeTools(const QList<McpToolDescriptor>& tools) {
    m_tools = tools;
    const bool nowConnected = !m_tools.isEmpty();
    if (m_connected != nowConnected) {
        m_connected = nowConnected;
        emit connectedChanged(m_connected);
    }
    emit toolsChanged();
}