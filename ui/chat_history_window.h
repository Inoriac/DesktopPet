#ifndef DESKTOP_PET_CHAT_HISTORY_WINDOW_H
#define DESKTOP_PET_CHAT_HISTORY_WINDOW_H

#include <QDateTime>
#include <QWidget>

class QScrollArea;
class QVBoxLayout;
class QTextEdit;
class QPushButton;

class ChatHistoryWindow : public QWidget {
    Q_OBJECT

public:
    explicit ChatHistoryWindow(QWidget* parent = nullptr);

    void addMessage(const QString& role, const QString& content, const QDateTime& timestamp);
    void clearMessages();

signals:
    void messageSubmitted(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void sendCurrentMessage();
    void scrollToBottom();

private:
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_messageContainer = nullptr;
    QVBoxLayout* m_messageLayout = nullptr;
    QTextEdit* m_inputEdit = nullptr;
    QPushButton* m_sendButton = nullptr;
};

#endif // DESKTOP_PET_CHAT_HISTORY_WINDOW_H
