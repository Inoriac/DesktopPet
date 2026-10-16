#ifndef DESKTOP_PET_MCP_CLIENT_H
#define DESKTOP_PET_MCP_CLIENT_H
#include <QObject>
#include <QJsonObject>
#include <QList>
#include <QProcess>
#include <QString>
#include <QElapsedTimer>
#include "ai/domain/domain_result.h"
struct McpToolDescriptor { QString name; QString description; QJsonObject inputSchema; };
// Blocking stdio protocol client. Construct and use on a capability worker.
class McpClient : public QObject {
    Q_OBJECT
public:
    explicit McpClient(QObject* parent=nullptr);
    ~McpClient() override;
    void setServerName(const QString& name) { m_serverName=name; }
    const QString& serverName() const { return m_serverName; }
    void setPrototypeTools(const QList<McpToolDescriptor>& tools);
    QList<McpToolDescriptor> tools() const { return m_tools; }
    bool isConnected() const { return m_connected; }
    Result<void,DomainError> connectStdio(const QString& program,const QStringList& arguments,int timeoutMs);
    Result<QJsonObject,DomainError> callTool(const QString& name,const QJsonObject& arguments,int timeoutMs);
    void close();
signals:
    void connectedChanged(bool connected);
    void toolsChanged();
private:
    Result<QJsonObject,DomainError> request(const QString& method,const QJsonObject& params,int timeoutMs);
    QString m_serverName;
    QList<McpToolDescriptor> m_tools;
    bool m_connected=false;
    QProcess m_process;
    QByteArray m_buffer;
    qint64 m_sequence=0;
};
#endif
