#ifndef DESKTOP_PET_MCP_CLIENT_H
#define DESKTOP_PET_MCP_CLIENT_H

#include <QObject>
#include <QJsonObject>
#include <QList>
#include <QString>

struct McpToolDescriptor {
    QString name;
    QString description;
    QJsonObject inputSchema;
};

class McpClient : public QObject {
    Q_OBJECT

public:
    explicit McpClient(QObject* parent = nullptr);

    void setServerName(const QString& serverName);
    const QString& serverName() const { return m_serverName; }

    void setPrototypeTools(const QList<McpToolDescriptor>& tools);
    QList<McpToolDescriptor> tools() const { return m_tools; }

    bool isConnected() const { return m_connected; }

signals:
    void connectedChanged(bool connected);
    void toolsChanged();

private:
    QString m_serverName;
    QList<McpToolDescriptor> m_tools;
    bool m_connected = false;
};

#endif // DESKTOP_PET_MCP_CLIENT_H