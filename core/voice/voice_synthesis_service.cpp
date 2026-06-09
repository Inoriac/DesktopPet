//
// Optional Python / GENIE voice synthesis sidecar
//

#include "voice_synthesis_service.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QUuid>
#include <QDebug>

namespace {
QString normalizedSource(QString source) {
    source = source.trimmed();
    return source.isEmpty() ? QStringLiteral("assistant") : source;
}
}

VoiceSynthesisService::VoiceSynthesisService(QObject* parent)
    : QObject(parent) {}

VoiceSynthesisService::~VoiceSynthesisService() {
    stop();
}

void VoiceSynthesisService::setConfig(const VoiceConfig& config) {
    const QString oldKey = m_lastConfigKey;
    m_config = config;
    m_lastConfigKey = configKey(m_config);

    if (!m_config.enabled) {
        stop();
        return;
    }

    if (m_process && oldKey != m_lastConfigKey) {
        stop();
    }

    if (m_config.preloadOnStart) {
        ensureStarted();
    }
}

void VoiceSynthesisService::speak(const QString& text, const QString& source) {
    const QString cleanText = text.trimmed();
    const QString cleanSource = normalizedSource(source);
    if (!m_config.enabled || cleanText.isEmpty() || !isSourceEnabled(cleanSource)) {
        return;
    }

    QString clippedText = cleanText;
    if (m_config.maxTextChars > 0 && clippedText.size() > m_config.maxTextChars) {
        clippedText = clippedText.left(m_config.maxTextChars);
    }

    if (m_requestInFlight) {
        m_pendingText = clippedText;
        m_pendingSource = cleanSource;
        qDebug() << "[Voice] queued speech while another request is active, source:" << cleanSource
                 << "chars:" << clippedText.size();
        return;
    }

    if (!ensureStarted()) {
        return;
    }

    if (!m_configured) {
        m_pendingText = clippedText;
        m_pendingSource = cleanSource;
        qDebug() << "[Voice] queued speech until worker is configured, source:" << cleanSource
                 << "chars:" << clippedText.size();
        return;
    }

    sendSpeak(clippedText, cleanSource);
}

void VoiceSynthesisService::stop() {
    m_pendingText.clear();
    m_pendingSource.clear();
    m_requestInFlight = false;
    m_configureInFlight = false;
    m_configured = false;
    m_activeRequestId.clear();
    m_configureRequestId.clear();
    m_stdoutBuffer.clear();

    if (!m_process) {
        return;
    }

    if (m_process->state() != QProcess::NotRunning) {
        sendLine(QJsonObject{{"type", "shutdown"}});
        m_process->closeWriteChannel();
        if (!m_process->waitForFinished(1000)) {
            m_process->terminate();
            if (!m_process->waitForFinished(1000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
    }

    m_process->deleteLater();
    m_process = nullptr;
}

bool VoiceSynthesisService::isSourceEnabled(const QString& source) const {
    if (source == "assistant") return m_config.sources.assistant;
    if (source == "proactive") return m_config.sources.proactive;
    if (source == "screenChat") return m_config.sources.screenChat;
    if (source == "fallback") return m_config.sources.fallback;
    if (source == "toolBubble") return m_config.sources.toolBubble;
    return true;
}

bool VoiceSynthesisService::ensureStarted() {
    if (!m_config.enabled) {
        return false;
    }
    if (m_process && m_process->state() != QProcess::NotRunning) {
        if (!m_configured && !m_configureInFlight) {
            sendConfigure();
        }
        return true;
    }

    const QString scriptPath = resolvePath(m_config.workerScript);
    if (!QFileInfo::exists(scriptPath)) {
        warnOnce("worker_missing", QString("[Voice] worker script not found: %1").arg(scriptPath));
        return false;
    }

    auto* process = new QProcess(this);
    process->setProgram(resolvePythonExecutable());
    process->setArguments({scriptPath});
    process->setWorkingDirectory(QDir::currentPath());
    process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(process, &QProcess::readyReadStandardOutput, this, &VoiceSynthesisService::handleStdout);
    connect(process, &QProcess::readyReadStandardError, this, &VoiceSynthesisService::handleStderr);
    connect(process, &QProcess::errorOccurred, this, &VoiceSynthesisService::handleProcessError);
    connect(process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &VoiceSynthesisService::handleProcessFinished);

    process->start();
    if (!process->waitForStarted(1000)) {
        warnOnce("start_failed", QString("[Voice] failed to start Python worker: %1").arg(process->errorString()));
        process->deleteLater();
        return false;
    }

    m_process = process;
    m_configured = false;
    m_configureInFlight = false;
    m_configureRequestId.clear();
    sendConfigure();
    return true;
}

void VoiceSynthesisService::startNextPending() {
    if (m_pendingText.isEmpty() || m_requestInFlight || !m_configured) {
        return;
    }
    const QString text = m_pendingText;
    const QString source = m_pendingSource;
    m_pendingText.clear();
    m_pendingSource.clear();
    sendSpeak(text, source);
}

void VoiceSynthesisService::sendConfigure() {
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_configured = false;
    m_configureInFlight = true;
    m_configureRequestId = newRequestId();
    qDebug() << "[Voice] configuring speaker:" << m_config.selectedSpeaker
             << "mode:" << m_config.speakerMode;
    sendLine(QJsonObject{
        {"type", "configure"},
        {"requestId", m_configureRequestId},
        {"config", configToJson()}
    });
}

void VoiceSynthesisService::sendSpeak(const QString& text, const QString& source) {
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_activeRequestId = newRequestId();
    m_requestInFlight = true;
    qDebug() << "[Voice] sent speak request:" << m_activeRequestId
             << "source:" << source
             << "chars:" << text.size();
    sendLine(QJsonObject{
        {"type", "speak"},
        {"requestId", m_activeRequestId},
        {"source", source},
        {"text", text}
    });
}

void VoiceSynthesisService::sendLine(const QJsonObject& object) {
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return;
    }
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    m_process->write(line);
}

void VoiceSynthesisService::handleStdout() {
    if (!m_process) return;
    m_stdoutBuffer += m_process->readAllStandardOutput();

    int newlineIndex = -1;
    while ((newlineIndex = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newlineIndex).trimmed();
        m_stdoutBuffer.remove(0, newlineIndex + 1);
        if (line.isEmpty()) continue;

        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            qWarning() << "[Voice] non-json worker stdout:" << line;
            continue;
        }
        handleWorkerEvent(doc.object());
    }
}

void VoiceSynthesisService::handleStderr() {
    if (!m_process) return;
    const QString text = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
    if (!text.isEmpty()) {
        qDebug().noquote() << "[VoiceWorker]" << text;
    }
}

void VoiceSynthesisService::handleWorkerEvent(const QJsonObject& event) {
    const QString type = event.value("type").toString();
    const QString requestId = event.value("requestId").toString();

    if (type == "ready") {
        return;
    }
    if (type == "configured") {
        m_configured = true;
        m_configureInFlight = false;
        m_configureRequestId.clear();
        qDebug() << "[Voice] configured speaker:" << event.value("speaker").toString();
        startNextPending();
        return;
    }
    if (type == "speech_started") {
        qDebug() << "[Voice] speech started:" << requestId
                 << "source:" << event.value("source").toString();
        return;
    }
    if (type == "speech_finished" || type == "speech_skipped") {
        qDebug() << "[Voice]" << type << ":" << requestId;
        if (requestId.isEmpty() || requestId == m_activeRequestId) {
            m_requestInFlight = false;
            m_activeRequestId.clear();
            startNextPending();
        }
        return;
    }
    if (type == "error") {
        const QString code = event.value("code").toString("worker_error");
        const QString message = event.value("message").toString();
        warnOnce(code, QString("[Voice] %1: %2").arg(code, message));
        if (requestId == m_configureRequestId || (!m_configured && m_configureInFlight)) {
            m_configureInFlight = false;
            m_configured = false;
            m_configureRequestId.clear();
        }
        if (requestId.isEmpty() || requestId == m_activeRequestId) {
            m_requestInFlight = false;
            m_activeRequestId.clear();
            if (m_configured) {
                startNextPending();
            }
        }
        return;
    }
}

void VoiceSynthesisService::handleProcessFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status)
    if (m_config.enabled) {
        warnOnce("worker_finished", QString("[Voice] worker exited: %1").arg(exitCode));
    }
    m_requestInFlight = false;
    m_configureInFlight = false;
    m_configured = false;
    m_activeRequestId.clear();
    m_configureRequestId.clear();
}

void VoiceSynthesisService::handleProcessError(QProcess::ProcessError error) {
    Q_UNUSED(error)
    warnOnce("process_error", QString("[Voice] process error: %1").arg(m_process ? m_process->errorString() : QString()));
    m_requestInFlight = false;
    m_configureInFlight = false;
    m_configured = false;
    m_activeRequestId.clear();
    m_configureRequestId.clear();
}

void VoiceSynthesisService::warnOnce(const QString& key, const QString& message) {
    if (m_warnedKeys.contains(key)) {
        return;
    }
    m_warnedKeys.insert(key);
    qWarning().noquote() << message;
}

QString VoiceSynthesisService::resolvePath(const QString& path) const {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    QFileInfo info(trimmed);
    if (info.isAbsolute()) {
        return info.absoluteFilePath();
    }
    return QDir(QDir::currentPath()).absoluteFilePath(trimmed);
}

QString VoiceSynthesisService::resolvePythonExecutable() const {
    const QString configuredPython = m_config.pythonExecutable.trimmed();
    if (!configuredPython.isEmpty()) {
        const QFileInfo configuredInfo(configuredPython);
        if (configuredInfo.isAbsolute() || configuredPython.contains('/') || configuredPython.contains('\\')) {
            return resolvePath(configuredPython);
        }
        return configuredPython;
    }

#ifdef Q_OS_WIN
    const QString venvPython = QDir(resolvePath(m_config.venvPath)).absoluteFilePath("Scripts/python.exe");
#else
    const QString venvPython = QDir(resolvePath(m_config.venvPath)).absoluteFilePath("bin/python");
#endif
    if (QFileInfo::exists(venvPython)) {
        return venvPython;
    }
    return QStringLiteral("python");
}

QJsonObject VoiceSynthesisService::configToJson() const {
    QJsonObject custom{
        {"name", m_config.customSpeaker.name},
        {"language", m_config.customSpeaker.language},
        {"onnxModelDir", resolvePath(m_config.customSpeaker.onnxModelDir)},
        {"referenceAudioPath", resolvePath(m_config.customSpeaker.referenceAudioPath)},
        {"referenceAudioText", m_config.customSpeaker.referenceAudioText}
    };

    return QJsonObject{
        {"backend", m_config.backend},
        {"allowAutoDownload", m_config.allowAutoDownload},
        {"genieDataDir", resolvePath(m_config.genieDataDir)},
        {"characterModelsDir", resolvePath(m_config.characterModelsDir)},
        {"customCharactersDir", resolvePath(m_config.customCharactersDir)},
        {"speakerMode", m_config.speakerMode},
        {"selectedSpeaker", m_config.selectedSpeaker},
        {"customSpeaker", custom},
        {"saveAudio", m_config.saveAudio},
        {"outputDir", resolvePath(m_config.outputDir)},
        {"maxTextChars", m_config.maxTextChars}
    };
}

QString VoiceSynthesisService::configKey(const VoiceConfig& config) const {
    const QJsonObject object{
        {"enabled", config.enabled},
        {"pythonExecutable", config.pythonExecutable},
        {"venvPath", config.venvPath},
        {"workerScript", config.workerScript},
        {"allowAutoDownload", config.allowAutoDownload},
        {"genieDataDir", config.genieDataDir},
        {"characterModelsDir", config.characterModelsDir},
        {"customCharactersDir", config.customCharactersDir},
        {"speakerMode", config.speakerMode},
        {"selectedSpeaker", config.selectedSpeaker},
        {"customName", config.customSpeaker.name},
        {"customLanguage", config.customSpeaker.language},
        {"customModel", config.customSpeaker.onnxModelDir},
        {"customReference", config.customSpeaker.referenceAudioPath}
    };
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString VoiceSynthesisService::newRequestId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
