#include "mcp_server_process.h"

McpServerProcess::McpServerProcess(QObject* parent)
    : QObject(parent) {
    connect(&m_process, &QProcess::started, this, &McpServerProcess::started);
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int, QProcess::ExitStatus) {
        emit stopped();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit errorOccurred(m_process.errorString());
    });
}

McpServerProcess::~McpServerProcess() {
    stop();
}

void McpServerProcess::configure(const QString& program, const QStringList& arguments) {
    m_program = program;
    m_arguments = arguments;
}

bool McpServerProcess::start(QString* errorMessage) {
    if (m_program.isEmpty()) {
        if (errorMessage) *errorMessage = "MCP server program is empty";
        return false;
    }

    if (isRunning()) {
        return true;
    }

    m_process.start(m_program, m_arguments);
    if (!m_process.waitForStarted(3000)) {
        if (errorMessage) *errorMessage = m_process.errorString();
        return false;
    }
    return true;
}

void McpServerProcess::stop() {
    if (!isRunning()) {
        return;
    }
    m_process.terminate();
    if (!m_process.waitForFinished(1500)) {
        m_process.kill();
        m_process.waitForFinished(1500);
    }
}

bool McpServerProcess::isRunning() const {
    return m_process.state() != QProcess::NotRunning;
}