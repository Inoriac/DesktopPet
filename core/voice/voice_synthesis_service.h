//
// Optional Python / GENIE voice synthesis sidecar
//

#ifndef DESKTOP_PET_VOICE_SYNTHESIS_SERVICE_H
#define DESKTOP_PET_VOICE_SYNTHESIS_SERVICE_H

#include <QObject>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QString>

#include "ai_types.h"

class VoiceSynthesisService : public QObject {
    Q_OBJECT

public:
    explicit VoiceSynthesisService(QObject* parent = nullptr);
    ~VoiceSynthesisService() override;

    void setConfig(const VoiceConfig& config);
    void speak(const QString& text, const QString& source);
    void stop();

private:
    bool isSourceEnabled(const QString& source) const;
    bool ensureStarted();
    void startNextPending();
    void sendConfigure();
    void sendSpeak(const QString& text, const QString& source);
    void sendLine(const QJsonObject& object);
    void handleStdout();
    void handleStderr();
    void handleWorkerEvent(const QJsonObject& event);
    void handleProcessFinished(int exitCode, QProcess::ExitStatus status);
    void handleProcessError(QProcess::ProcessError error);
    void warnOnce(const QString& key, const QString& message);

    QString resolvePath(const QString& path) const;
    QString resolvePythonExecutable() const;
    QJsonObject configToJson() const;
    QString configKey(const VoiceConfig& config) const;
    QString newRequestId() const;

    VoiceConfig m_config;
    QProcess* m_process = nullptr;
    QByteArray m_stdoutBuffer;
    QString m_activeRequestId;
    QString m_configureRequestId;
    QString m_pendingText;
    QString m_pendingSource;
    bool m_requestInFlight = false;
    bool m_configureInFlight = false;
    bool m_configured = false;
    QString m_lastConfigKey;
    QSet<QString> m_warnedKeys;
};

#endif // DESKTOP_PET_VOICE_SYNTHESIS_SERVICE_H
