#ifndef DESKTOP_PET_MCP_SERVER_PROCESS_H
#define DESKTOP_PET_MCP_SERVER_PROCESS_H

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class McpServerProcess : public QObject {
    Q_OBJECT

public:
    explicit McpServerProcess(QObject* parent = nullptr);
    ~McpServerProcess() override;

    void configure(const QString& program, const QStringList& arguments = {});
    bool start(QString* errorMessage = nullptr);
    void stop();
    bool isRunning() const;

signals:
    void started();
    void stopped();
    void errorOccurred(const QString& message);

private:
    QString m_program;
    QStringList m_arguments;
    QProcess m_process;
};

#endif // DESKTOP_PET_MCP_SERVER_PROCESS_H