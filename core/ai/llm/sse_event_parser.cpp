#include "sse_event_parser.h"

#include <utility>

SseEventParser::SseEventParser(EventHandler handler)
    : m_handler(std::move(handler)) {}

bool SseEventParser::feed(const QByteArray& chunk, QString* errorMessage) {
    if (m_failed) {
        return fail(errorMessage, QStringLiteral("SSE framing parser is already failed"));
    }

    m_buffer.append(chunk);
    qsizetype consumed = 0;
    qsizetype newline = -1;
    while ((newline = m_buffer.indexOf('\n', consumed)) >= 0) {
        QByteArray line = m_buffer.mid(consumed, newline - consumed);
        consumed = newline + 1;
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (!consumeLine(std::move(line), errorMessage)) {
            m_failed = true;
            m_buffer.clear();
            m_eventName.clear();
            m_dataLines.clear();
            m_hasFields = false;
            return false;
        }
    }
    if (consumed > 0) {
        m_buffer.remove(0, consumed);
    }
    return true;
}

bool SseEventParser::finish(QString* errorMessage) {
    if (m_failed) {
        return fail(errorMessage, QStringLiteral("SSE framing parser is already failed"));
    }
    if (!m_buffer.isEmpty()) {
        m_failed = true;
        m_buffer.clear();
        return fail(errorMessage, QStringLiteral("SSE framing error: incomplete final line"));
    }
    publishPendingEvent();
    return true;
}

bool SseEventParser::consumeLine(QByteArray line, QString* errorMessage) {
    if (line.isEmpty()) {
        publishPendingEvent();
        return true;
    }
    if (line.startsWith(':')) {
        return true;
    }
    if (line.contains('\0')) {
        return fail(errorMessage, QStringLiteral("SSE framing error: invalid field bytes"));
    }

    const qsizetype colon = line.indexOf(':');
    const QByteArray field = colon < 0 ? line : line.left(colon);
    QByteArray value = colon < 0 ? QByteArray{} : line.mid(colon + 1);
    if (value.startsWith(' ')) {
        value.remove(0, 1);
    }

    if (field == "event") {
        m_eventName = QString::fromUtf8(value);
        m_hasFields = true;
    } else if (field == "data") {
        m_dataLines.append(std::move(value));
        m_hasFields = true;
    }
    return true;
}

void SseEventParser::publishPendingEvent() {
    if (!m_hasFields) {
        return;
    }

    if (m_handler && !m_dataLines.isEmpty()) {
        m_handler(SseEvent{m_eventName, m_dataLines.join('\n')});
    }
    m_eventName.clear();
    m_dataLines.clear();
    m_hasFields = false;
}

bool SseEventParser::fail(QString* errorMessage, const QString& message) {
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}
