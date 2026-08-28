#ifndef DESKTOP_PET_CHAT_HISTORY_WINDOW_H
#define DESKTOP_PET_CHAT_HISTORY_WINDOW_H

#include <QHash>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QWidget>

class ChatConversationModel;
class ChatMessageRow;
class QCloseEvent;
class QEvent;
class QHideEvent;
class QLabel;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QTimer;
class QVBoxLayout;

class GrowingPlainTextEdit : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit GrowingPlainTextEdit(QWidget* parent = nullptr);

    void setHeightRange(int minimum, int maximum);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateDocumentHeight();

    int m_minimumDocumentHeight = 44;
    int m_maximumDocumentHeight = 120;
};

class ChatHistoryWindow : public QWidget {
    Q_OBJECT

public:
    explicit ChatHistoryWindow(QWidget* parent = nullptr);

    void bindConversation(ChatConversationModel* model,
                          const QString& profileId,
                          const QString& petDisplayName);
    void revealConversation();
    QString lastFullyVisibleMessageId() const;

signals:
    void messageSubmitted(const QString& text);
    void stopRequested();
    void retryRequested(const QString& assistantMessageId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void clearRows();
    void insertMessageRow(int modelIndex, const QString& messageId);
    void scheduleMessageUpdate(const QString& messageId);
    void flushPendingMessageUpdates();
    void submitCurrentMessage();
    void updateResponseActive();
    void ensureLastReadDivider();
    void positionAfterReveal();
    void clampToAvailableScreen();
    void persistWindowState();
    void scrollToBottom();
    bool viewportIsNearBottom() const;
    void updateJumpButton();
    void positionJumpButton();
    void applyThemeStyle();
    int layoutIndexForModelIndex(int modelIndex) const;

    QPointer<ChatConversationModel> m_model;
    QString m_profileId;
    QString m_petDisplayName;
    QStringList m_messageOrder;
    QHash<QString, ChatMessageRow*> m_rows;
    QSet<QString> m_pendingMessageUpdates;
    bool m_responseActive = false;
    bool m_hasBeenRevealed = false;
    bool m_followPendingUpdate = true;
    bool m_internalScrollChange = false;
    int m_preservedScrollValue = 0;

    QLabel* m_titleLabel = nullptr;
    QLabel* m_subtitleLabel = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_messageContainer = nullptr;
    QVBoxLayout* m_messageLayout = nullptr;
    QWidget* m_lastReadDivider = nullptr;
    QPushButton* m_jumpButton = nullptr;
    QWidget* m_inputPanel = nullptr;
    GrowingPlainTextEdit* m_inputEdit = nullptr;
    QPushButton* m_mainActionButton = nullptr;
    QTimer* m_streamFlushTimer = nullptr;
};

#endif // DESKTOP_PET_CHAT_HISTORY_WINDOW_H
