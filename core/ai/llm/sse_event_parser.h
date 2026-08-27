#ifndef DESKTOP_PET_SSE_EVENT_PARSER_H
#define DESKTOP_PET_SSE_EVENT_PARSER_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <functional>

struct SseEvent {
    QString event;
    QByteArray data;
};

class SseEventParser {
public:
    using EventHandler = std::function<void(const SseEvent&)>;

    explicit SseEventParser(EventHandler handler);

    bool feed(const QByteArray& chunk, QString* errorMessage = nullptr);
    bool finish(QString* errorMessage = nullptr);

private:
    bool consumeLine(QByteArray line, QString* errorMessage);
    void publishPendingEvent();
    static bool fail(QString* errorMessage, const QString& message);

    EventHandler m_handler;
    QByteArray m_buffer;
    QString m_eventName;
    QList<QByteArray> m_dataLines;
    bool m_hasFields = false;
    bool m_failed = false;
};

#endif // DESKTOP_PET_SSE_EVENT_PARSER_H
