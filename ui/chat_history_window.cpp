#include "chat_history_window.h"

#include "chat_conversation_model.h"
#include "theme_manager.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCloseEvent>
#include <QFrame>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kBottomFollowDistance = 24;
constexpr int kStreamFlushIntervalMs = 32;

QString displayNameForRole(const QString& role, const QString& petName) {
    if (role == QLatin1String("user")) return QStringLiteral("你");
    if (role == QLatin1String("assistant")) {
        return petName.trimmed().isEmpty() ? QStringLiteral("桌宠") : petName;
    }
    if (role == QLatin1String("system")) return QStringLiteral("系统");
    return role;
}

bool isResponseActive(ChatMessageStatus status) {
    return status == ChatMessageStatus::Pending
        || status == ChatMessageStatus::Streaming;
}

bool isRetryable(ChatMessageStatus status) {
    return status == ChatMessageStatus::Interrupted
        || status == ChatMessageStatus::Failed;
}

QString statusCaption(ChatMessageStatus status) {
    switch (status) {
    case ChatMessageStatus::Pending:
        return QStringLiteral("等待回复");
    case ChatMessageStatus::Streaming:
        return QStringLiteral("正在回复");
    case ChatMessageStatus::Interrupted:
        return QStringLiteral("回复中断");
    case ChatMessageStatus::Stopped:
        return QStringLiteral("已停止");
    case ChatMessageStatus::Failed:
        return QStringLiteral("回复失败");
    case ChatMessageStatus::Complete:
        return {};
    }
    return {};
}

const ChatHistoryEntry* findEntry(const QList<ChatHistoryEntry>& messages,
                                  const QString& messageId) {
    const auto found = std::find_if(
        messages.cbegin(), messages.cend(),
        [&messageId](const ChatHistoryEntry& entry) {
            return entry.id == messageId;
        });
    return found == messages.cend() ? nullptr : &*found;
}

bool hasSourceUser(const QList<ChatHistoryEntry>& messages,
                   const ChatHistoryEntry& assistant) {
    if (assistant.replyToId.isEmpty()) return false;
    const ChatHistoryEntry* source = findEntry(messages, assistant.replyToId);
    return source && source->role == QLatin1String("user");
}

} // namespace

class ChatMessageRow final : public QWidget {
public:
    explicit ChatMessageRow(QWidget* parent = nullptr)
        : QWidget(parent) {
        setObjectName(QStringLiteral("chatMessageRow"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

        m_outerLayout = new QHBoxLayout(this);
        m_outerLayout->setContentsMargins(0, 0, 0, 0);
        m_outerLayout->setSpacing(0);

        m_panel = new QWidget(this);
        m_panel->setObjectName(QStringLiteral("messagePanel"));
        m_panel->setMaximumWidth(360);
        m_panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        auto* panelLayout = new QVBoxLayout(m_panel);
        panelLayout->setContentsMargins(12, 9, 12, 9);
        panelLayout->setSpacing(4);

        auto* metaLayout = new QHBoxLayout();
        metaLayout->setContentsMargins(0, 0, 0, 0);
        metaLayout->setSpacing(8);
        m_authorLabel = new QLabel(m_panel);
        m_authorLabel->setObjectName(QStringLiteral("messageAuthor"));
        m_timeLabel = new QLabel(m_panel);
        m_timeLabel->setObjectName(QStringLiteral("messageTime"));
        metaLayout->addWidget(m_authorLabel);
        metaLayout->addStretch(1);
        metaLayout->addWidget(m_timeLabel);
        panelLayout->addLayout(metaLayout);

        m_bodyLabel = new QLabel(m_panel);
        m_bodyLabel->setObjectName(QStringLiteral("messageBody"));
        m_bodyLabel->setTextFormat(Qt::PlainText);
        m_bodyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_bodyLabel->setWordWrap(true);
        m_bodyLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        panelLayout->addWidget(m_bodyLabel);

        auto* statusLayout = new QHBoxLayout();
        statusLayout->setContentsMargins(0, 0, 0, 0);
        statusLayout->setSpacing(4);
        m_statusLabel = new QLabel(m_panel);
        m_statusLabel->setObjectName(QStringLiteral("messageStatus"));
        m_retryButton = new QPushButton(m_panel);
        m_retryButton->setObjectName(QStringLiteral("retryButton"));
        m_retryButton->setFixedSize(32, 32);
        m_retryButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        m_retryButton->setToolTip(QStringLiteral("重新生成回复"));
        statusLayout->addWidget(m_statusLabel);
        statusLayout->addStretch(1);
        statusLayout->addWidget(m_retryButton);
        panelLayout->addLayout(statusLayout);

        m_outerLayout->addWidget(m_panel);
        m_outerLayout->addStretch(1);
    }

    void setEntry(const ChatHistoryEntry& entry,
                  const QString& petDisplayName,
                  bool allowRetry) {
        setProperty("messageId", entry.id);
        setProperty("chatRole", entry.role);
        m_authorLabel->setText(displayNameForRole(entry.role, petDisplayName));
        m_timeLabel->setText(entry.timestamp.isValid()
                                 ? entry.timestamp.toString(QStringLiteral("HH:mm"))
                                 : QString());
        m_bodyLabel->setText(entry.content);
        const QString caption = statusCaption(entry.status);
        m_statusLabel->setText(caption);
        m_statusLabel->setVisible(!caption.isEmpty());
        m_retryButton->setProperty("assistantMessageId", entry.id);
        m_retryButton->setVisible(allowRetry);

        while (QLayoutItem* item = m_outerLayout->takeAt(0)) {
            delete item;
        }
        if (entry.role == QLatin1String("user")) {
            m_outerLayout->addStretch(1);
            m_outerLayout->addWidget(m_panel, 0, Qt::AlignRight);
        } else if (entry.role == QLatin1String("system")) {
            m_outerLayout->addStretch(1);
            m_outerLayout->addWidget(m_panel, 0, Qt::AlignHCenter);
            m_outerLayout->addStretch(1);
        } else {
            m_outerLayout->addWidget(m_panel, 0, Qt::AlignLeft);
            m_outerLayout->addStretch(1);
        }
        updateGeometry();
    }

    QPushButton* retryButton() const { return m_retryButton; }

    void applyTheme(bool dark) {
        const QString primary = dark ? QStringLiteral("#f1f3f5")
                                     : QStringLiteral("#202124");
        const QString caption = dark ? QStringLiteral("#aeb6c2")
                                     : QStringLiteral("#667085");
        const QString role = property("chatRole").toString();
        QString panelBackground = QStringLiteral("transparent");
        QString panelBorder = QStringLiteral("transparent");
        if (role == QLatin1String("user")) {
            panelBackground = dark ? QStringLiteral("rgba(64, 126, 201, 0.22)")
                                   : QStringLiteral("rgba(0, 102, 204, 0.10)");
            panelBorder = dark ? QStringLiteral("rgba(116, 170, 235, 0.24)")
                               : QStringLiteral("rgba(0, 102, 204, 0.16)");
        } else if (role == QLatin1String("assistant")) {
            panelBackground = dark ? QStringLiteral("rgba(255, 255, 255, 0.035)")
                                   : QStringLiteral("rgba(255, 255, 255, 0.72)");
            panelBorder = dark ? QStringLiteral("rgba(255, 255, 255, 0.08)")
                               : QStringLiteral("rgba(32, 33, 36, 0.08)");
        }
        m_panel->setStyleSheet(QStringLiteral(
            "QWidget#messagePanel { background: %1; border: 1px solid %2; "
            "border-radius: 8px; }")
                                   .arg(panelBackground, panelBorder));
        m_authorLabel->setStyleSheet(QStringLiteral(
            "font-size: 13px; font-weight: 600; color: %1; background: transparent;")
                                         .arg(primary));
        const QString bodyColor = role == QLatin1String("system")
            ? caption
            : primary;
        m_bodyLabel->setStyleSheet(QStringLiteral(
            "font-size: 14px; color: %1; background: transparent; border: none;")
                                       .arg(bodyColor));
        const QString captionStyle = QStringLiteral(
            "font-size: 12px; color: %1; background: transparent; border: none;")
                                         .arg(caption);
        m_timeLabel->setStyleSheet(captionStyle);
        m_statusLabel->setStyleSheet(captionStyle);
        m_retryButton->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; border-radius: 6px; padding: 4px; }"
            "QPushButton:hover { background: rgba(0, 102, 204, 0.12); }"));
    }

private:
    QHBoxLayout* m_outerLayout = nullptr;
    QWidget* m_panel = nullptr;
    QLabel* m_authorLabel = nullptr;
    QLabel* m_timeLabel = nullptr;
    QLabel* m_bodyLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPushButton* m_retryButton = nullptr;
};

GrowingPlainTextEdit::GrowingPlainTextEdit(QWidget* parent)
    : QPlainTextEdit(parent) {
    setTabChangesFocus(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    connect(document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged,
            this,
            [this]() { updateDocumentHeight(); });
    setHeightRange(m_minimumDocumentHeight, m_maximumDocumentHeight);
}

void GrowingPlainTextEdit::setHeightRange(int minimum, int maximum) {
    m_minimumDocumentHeight = std::max(0, minimum);
    m_maximumDocumentHeight = std::max(m_minimumDocumentHeight, maximum);
    setMinimumHeight(m_minimumDocumentHeight);
    setMaximumHeight(m_maximumDocumentHeight);
    updateDocumentHeight();
}

void GrowingPlainTextEdit::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    if (event->oldSize().width() != event->size().width()) {
        QTimer::singleShot(0, this, [this]() { updateDocumentHeight(); });
    }
}

void GrowingPlainTextEdit::updateDocumentHeight() {
    const QSizeF documentSize = document()->documentLayout()->documentSize();
    const int explicitLineHeight =
        document()->blockCount() * fontMetrics().lineSpacing();
    const int chrome = frameWidth() * 2
        + contentsMargins().top() + contentsMargins().bottom() + 10;
    const int contentHeight = std::max(
        static_cast<int>(std::ceil(documentSize.height())), explicitLineHeight);
    const int desired = contentHeight + chrome;
    setFixedHeight(std::clamp(desired,
                             m_minimumDocumentHeight,
                             m_maximumDocumentHeight));
}

ChatHistoryWindow::ChatHistoryWindow(QWidget* parent)
    : QWidget(parent) {
    setWindowTitle(QStringLiteral("桌宠私聊"));
    setWindowFlag(Qt::Window, true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(460, 680);
    setMinimumSize(360, 520);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("chatHeader"));
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(16, 10, 16, 10);
    headerLayout->setSpacing(2);
    m_titleLabel = new QLabel(QStringLiteral("桌宠私聊"), header);
    m_titleLabel->setObjectName(QStringLiteral("chatTitle"));
    m_subtitleLabel = new QLabel(QStringLiteral("私聊"), header);
    m_subtitleLabel->setObjectName(QStringLiteral("chatSubtitle"));
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addWidget(m_subtitleLabel);
    rootLayout->addWidget(header);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName(QStringLiteral("conversationScrollArea"));
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messageContainer = new QWidget(m_scrollArea);
    m_messageContainer->setObjectName(QStringLiteral("messageContainer"));
    m_messageLayout = new QVBoxLayout(m_messageContainer);
    m_messageLayout->setContentsMargins(16, 16, 16, 16);
    m_messageLayout->setSpacing(12);
    m_messageLayout->setAlignment(Qt::AlignTop);
    m_messageLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    m_scrollArea->setWidget(m_messageContainer);
    m_scrollArea->viewport()->installEventFilter(this);
    rootLayout->addWidget(m_scrollArea, 1);

    m_jumpButton = new QPushButton(m_scrollArea->viewport());
    m_jumpButton->setObjectName(QStringLiteral("jumpToLatestButton"));
    m_jumpButton->setFixedSize(32, 32);
    m_jumpButton->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    m_jumpButton->setToolTip(QStringLiteral("回到最新消息"));
    m_jumpButton->hide();
    connect(m_jumpButton, &QPushButton::clicked, this,
            &ChatHistoryWindow::scrollToBottom);

    m_inputPanel = new QWidget(this);
    m_inputPanel->setObjectName(QStringLiteral("chatInputPanel"));
    auto* inputLayout = new QHBoxLayout(m_inputPanel);
    inputLayout->setContentsMargins(12, 10, 12, 10);
    inputLayout->setSpacing(8);
    m_inputEdit = new GrowingPlainTextEdit(m_inputPanel);
    m_inputEdit->setObjectName(QStringLiteral("messageInput"));
    m_inputEdit->setPlaceholderText(QStringLiteral("和桌宠说点什么..."));
    m_inputEdit->setHeightRange(44, 120);
    m_inputEdit->installEventFilter(this);
    inputLayout->addWidget(m_inputEdit, 1, Qt::AlignBottom);
    m_mainActionButton = new QPushButton(m_inputPanel);
    m_mainActionButton->setObjectName(QStringLiteral("mainActionButton"));
    m_mainActionButton->setFixedSize(32, 32);
    inputLayout->addWidget(m_mainActionButton, 0, Qt::AlignBottom);
    rootLayout->addWidget(m_inputPanel);

    m_streamFlushTimer = new QTimer(this);
    m_streamFlushTimer->setSingleShot(true);
    m_streamFlushTimer->setInterval(kStreamFlushIntervalMs);
    connect(m_streamFlushTimer, &QTimer::timeout, this,
            &ChatHistoryWindow::flushPendingMessageUpdates);
    connect(m_mainActionButton, &QPushButton::clicked, this, [this]() {
        if (m_responseActive) {
            emit stopRequested();
            return;
        }
        submitCurrentMessage();
    });
    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this]() {
                if (!m_internalScrollChange && !viewportIsNearBottom()) {
                    m_followPendingUpdate = false;
                    m_preservedScrollValue =
                        m_scrollArea->verticalScrollBar()->value();
                }
                updateJumpButton();
            });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this](ThemeManager::Theme) { applyThemeStyle(); });

    updateResponseActive();
    applyThemeStyle();
}

void ChatHistoryWindow::bindConversation(ChatConversationModel* model,
                                         const QString& profileId,
                                         const QString& petDisplayName) {
    if (m_model == model && m_profileId == profileId) {
        m_petDisplayName = petDisplayName;
        m_titleLabel->setText(petDisplayName.trimmed().isEmpty()
                                  ? QStringLiteral("桌宠私聊")
                                  : petDisplayName);
        return;
    }

    if (m_model) disconnect(m_model, nullptr, this, nullptr);
    clearRows();
    m_model = model;
    m_profileId = profileId;
    m_petDisplayName = petDisplayName;
    m_titleLabel->setText(petDisplayName.trimmed().isEmpty()
                              ? QStringLiteral("桌宠私聊")
                              : petDisplayName);
    m_hasBeenRevealed = false;

    if (!m_model) {
        updateResponseActive();
        return;
    }

    const QList<ChatHistoryEntry> messages = m_model->messages();
    for (int index = 0; index < messages.size(); ++index) {
        insertMessageRow(index, messages.at(index).id);
    }

    connect(m_model, &ChatConversationModel::messageInserted,
            this, [this](int index, const QString& messageId) {
                const bool follow = viewportIsNearBottom();
                const int previousValue =
                    m_scrollArea->verticalScrollBar()->value();
                insertMessageRow(index, messageId);
                updateResponseActive();
                QTimer::singleShot(0, this, [this, follow, previousValue]() {
                    if (follow) scrollToBottom();
                    else {
                        m_internalScrollChange = true;
                        m_scrollArea->verticalScrollBar()->setValue(previousValue);
                        m_internalScrollChange = false;
                        updateJumpButton();
                    }
                });
            });
    connect(m_model, &ChatConversationModel::messageChanged,
            this, [this](int index, const QString& messageId) {
                scheduleMessageUpdate(messageId);
                const QList<ChatHistoryEntry> messages = m_model->messages();
                if (index >= 0 && index < messages.size()
                    && isTerminalChatMessageStatus(messages.at(index).status)) {
                    flushPendingMessageUpdates();
                }
            });
    connect(m_model, &ChatConversationModel::assistantStageChanged,
            this, [this](const QString& messageId, ChatActivityStage) {
                scheduleMessageUpdate(messageId);
            });
    updateResponseActive();
    applyThemeStyle();
}

void ChatHistoryWindow::revealConversation() {
    if (isVisible()) {
        raise();
        activateWindow();
        return;
    }
    if (!m_profileId.isEmpty()) {
        const QByteArray geometry = QSettings().value(
            QStringLiteral("chat/%1/windowGeometry").arg(m_profileId))
                                        .toByteArray();
        if (!geometry.isEmpty()) restoreGeometry(geometry);
    }
    clampToAvailableScreen();
    ensureLastReadDivider();
    show();
    raise();
    activateWindow();
    m_hasBeenRevealed = true;
    QTimer::singleShot(0, this, &ChatHistoryWindow::positionAfterReveal);
}

QString ChatHistoryWindow::lastFullyVisibleMessageId() const {
    if (!m_scrollArea) return {};
    const QWidget* viewport = m_scrollArea->viewport();
    QString lastFullyVisible;
    for (const QString& messageId : m_messageOrder) {
        ChatMessageRow* row = m_rows.value(messageId, nullptr);
        if (!row || row->isHidden()) continue;
        const QPoint topLeft = row->mapTo(
            const_cast<QWidget*>(viewport), QPoint(0, 0));
        const int bottom = topLeft.y() + row->height();
        if (topLeft.y() >= 0 && bottom <= viewport->height()) {
            lastFullyVisible = messageId;
        }
    }
    return lastFullyVisible;
}

bool ChatHistoryWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_inputEdit && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const bool enterPressed = keyEvent->key() == Qt::Key_Return
            || keyEvent->key() == Qt::Key_Enter;
        const bool wantsNewLine =
            keyEvent->modifiers().testFlag(Qt::ShiftModifier);
        if (enterPressed && !wantsNewLine) {
            if (!m_responseActive) submitCurrentMessage();
            return true;
        }
    }
    if (m_scrollArea && watched == m_scrollArea->viewport()
        && event->type() == QEvent::Resize) {
        positionJumpButton();
    }
    return QWidget::eventFilter(watched, event);
}

void ChatHistoryWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    positionJumpButton();
}

void ChatHistoryWindow::hideEvent(QHideEvent* event) {
    persistWindowState();
    QWidget::hideEvent(event);
}

void ChatHistoryWindow::closeEvent(QCloseEvent* event) {
    persistWindowState();
    QWidget::closeEvent(event);
}

void ChatHistoryWindow::clearRows() {
    m_streamFlushTimer->stop();
    m_pendingMessageUpdates.clear();
    m_rows.clear();
    m_messageOrder.clear();
    m_lastReadDivider = nullptr;
    while (QLayoutItem* item = m_messageLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void ChatHistoryWindow::insertMessageRow(int modelIndex,
                                         const QString& messageId) {
    if (!m_model || m_rows.contains(messageId)) return;
    const QList<ChatHistoryEntry> messages = m_model->messages();
    const ChatHistoryEntry* entry = findEntry(messages, messageId);
    if (!entry) return;

    const int boundedIndex = std::clamp(
        modelIndex, 0, static_cast<int>(m_messageOrder.size()));
    auto* row = new ChatMessageRow(m_messageContainer);
    row->setEntry(*entry, m_petDisplayName,
                  isRetryable(entry->status) && hasSourceUser(messages, *entry));
    row->applyTheme(ThemeManager::instance().isDarkTheme());
    connect(row->retryButton(), &QPushButton::clicked, this,
            [this, messageId]() {
                if (!m_responseActive) emit retryRequested(messageId);
            });
    m_messageLayout->insertWidget(layoutIndexForModelIndex(boundedIndex), row);
    m_messageOrder.insert(boundedIndex, messageId);
    m_rows.insert(messageId, row);

    if (m_hasBeenRevealed) ensureLastReadDivider();
}

void ChatHistoryWindow::scheduleMessageUpdate(const QString& messageId) {
    if (!m_rows.contains(messageId)) return;
    if (!m_streamFlushTimer->isActive()) {
        m_followPendingUpdate = viewportIsNearBottom();
        m_preservedScrollValue = m_scrollArea->verticalScrollBar()->value();
        m_streamFlushTimer->start();
    }
    m_pendingMessageUpdates.insert(messageId);
}

void ChatHistoryWindow::flushPendingMessageUpdates() {
    if (!m_model) return;
    m_streamFlushTimer->stop();
    const QList<ChatHistoryEntry> messages = m_model->messages();
    const QSet<QString> pending = std::exchange(
        m_pendingMessageUpdates, QSet<QString>{});
    for (const QString& messageId : pending) {
        ChatMessageRow* row = m_rows.value(messageId, nullptr);
        const ChatHistoryEntry* entry = findEntry(messages, messageId);
        if (!row || !entry) continue;
        row->setEntry(*entry, m_petDisplayName,
                      isRetryable(entry->status)
                          && hasSourceUser(messages, *entry));
        row->applyTheme(ThemeManager::instance().isDarkTheme());
    }
    updateResponseActive();
    m_messageLayout->activate();
    m_messageContainer->adjustSize();
    if (!m_followPendingUpdate) {
        m_internalScrollChange = true;
        m_scrollArea->verticalScrollBar()->setValue(m_preservedScrollValue);
        m_internalScrollChange = false;
        updateJumpButton();
    }
    QTimer::singleShot(0, this, [this]() {
        if (m_followPendingUpdate) {
            scrollToBottom();
        } else {
            m_internalScrollChange = true;
            m_scrollArea->verticalScrollBar()->setValue(m_preservedScrollValue);
            m_internalScrollChange = false;
            updateJumpButton();
        }
    });
}

void ChatHistoryWindow::submitCurrentMessage() {
    const QString text = m_inputEdit->toPlainText().trimmed();
    if (text.isEmpty() || m_responseActive) return;
    m_inputEdit->clear();
    emit messageSubmitted(text);
}

void ChatHistoryWindow::updateResponseActive() {
    m_responseActive = false;
    QList<ChatHistoryEntry> messages;
    if (m_model) {
        messages = m_model->messages();
        m_responseActive = std::any_of(
            messages.cbegin(), messages.cend(),
            [](const ChatHistoryEntry& entry) {
                return entry.role == QLatin1String("assistant")
                    && isResponseActive(entry.status);
            });
    }
    m_mainActionButton->setIcon(style()->standardIcon(
        m_responseActive ? QStyle::SP_BrowserStop : QStyle::SP_ArrowForward));
    m_mainActionButton->setToolTip(
        m_responseActive ? QStringLiteral("停止回复") : QStringLiteral("发送"));
    for (const ChatHistoryEntry& entry : messages) {
        ChatMessageRow* row = m_rows.value(entry.id, nullptr);
        if (!row) continue;
        const bool eligible = isRetryable(entry.status)
            && hasSourceUser(messages, entry);
        row->retryButton()->setVisible(eligible);
        row->retryButton()->setEnabled(eligible && !m_responseActive);
    }
}

void ChatHistoryWindow::ensureLastReadDivider() {
    if (m_lastReadDivider) {
        m_messageLayout->removeWidget(m_lastReadDivider);
        delete m_lastReadDivider;
        m_lastReadDivider = nullptr;
    }
    if (!m_model) return;
    const int readIndex = m_messageOrder.indexOf(m_model->lastReadMessageId());
    if (readIndex < 0 || readIndex >= m_messageOrder.size() - 1) return;

    m_lastReadDivider = new QWidget(m_messageContainer);
    m_lastReadDivider->setObjectName(QStringLiteral("lastReadDivider"));
    auto* dividerLayout = new QHBoxLayout(m_lastReadDivider);
    dividerLayout->setContentsMargins(0, 4, 0, 4);
    dividerLayout->setSpacing(8);
    auto makeLine = [this]() {
        auto* line = new QFrame(m_lastReadDivider);
        line->setObjectName(QStringLiteral("lastReadLine"));
        line->setFrameShape(QFrame::HLine);
        return line;
    };
    auto* caption = new QLabel(QStringLiteral("上次看到这里"), m_lastReadDivider);
    caption->setObjectName(QStringLiteral("lastReadCaption"));
    dividerLayout->addWidget(makeLine(), 1);
    dividerLayout->addWidget(caption);
    dividerLayout->addWidget(makeLine(), 1);
    m_messageLayout->insertWidget(layoutIndexForModelIndex(readIndex + 1),
                                  m_lastReadDivider);
    applyThemeStyle();
}

void ChatHistoryWindow::positionAfterReveal() {
    if (m_lastReadDivider) {
        const int target = std::max(
            0, m_lastReadDivider->y() - m_scrollArea->viewport()->height() / 3);
        m_internalScrollChange = true;
        m_scrollArea->verticalScrollBar()->setValue(target);
        m_internalScrollChange = false;
        updateJumpButton();
        return;
    }
    scrollToBottom();
}

void ChatHistoryWindow::clampToAvailableScreen() {
    const QRect frame = frameGeometry();
    const QList<QScreen*> screens = QGuiApplication::screens();
    const bool visibleOnScreen = std::any_of(
        screens.cbegin(), screens.cend(), [&frame](QScreen* screen) {
            return screen && screen->availableGeometry().intersects(frame);
        });
    if (visibleOnScreen) return;

    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect available = screen->availableGeometry();
    resize(std::min(width(), available.width()),
           std::min(height(), available.height()));
    move(available.center() - rect().center());
}

void ChatHistoryWindow::persistWindowState() {
    if (m_profileId.isEmpty()) return;
    QSettings settings;
    settings.setValue(QStringLiteral("chat/%1/windowGeometry").arg(m_profileId),
                      saveGeometry());
    if (m_model) {
        const QString messageId = lastFullyVisibleMessageId();
        if (!messageId.isEmpty()) m_model->markReadThrough(messageId);
    }
    settings.sync();
}

void ChatHistoryWindow::scrollToBottom() {
    if (!m_scrollArea) return;
    QTimer::singleShot(0, this, [this]() {
        QScrollBar* bar = m_scrollArea->verticalScrollBar();
        m_internalScrollChange = true;
        bar->setValue(bar->maximum());
        m_internalScrollChange = false;
        updateJumpButton();
    });
}

bool ChatHistoryWindow::viewportIsNearBottom() const {
    if (!m_scrollArea) return true;
    const QScrollBar* bar = m_scrollArea->verticalScrollBar();
    return bar->maximum() - bar->value() <= kBottomFollowDistance;
}

void ChatHistoryWindow::updateJumpButton() {
    if (!m_jumpButton) return;
    m_jumpButton->setVisible(isVisible() && !viewportIsNearBottom());
    if (m_jumpButton->isVisible()) m_jumpButton->raise();
}

void ChatHistoryWindow::positionJumpButton() {
    if (!m_scrollArea || !m_jumpButton) return;
    const int x = m_scrollArea->viewport()->width()
        - m_jumpButton->width() - 12;
    const int y = m_scrollArea->viewport()->height()
        - m_jumpButton->height() - 12;
    m_jumpButton->move(std::max(0, x), std::max(0, y));
}

void ChatHistoryWindow::applyThemeStyle() {
    const bool dark = ThemeManager::instance().isDarkTheme();
    const QString background = dark ? QStringLiteral("#15181d")
                                    : QStringLiteral("#f5f6f8");
    const QString surface = dark ? QStringLiteral("#1b1f25")
                                 : QStringLiteral("#ffffff");
    const QString primary = dark ? QStringLiteral("#f1f3f5")
                                 : QStringLiteral("#202124");
    const QString caption = dark ? QStringLiteral("#aeb6c2")
                                 : QStringLiteral("#667085");
    const QString border = dark ? QStringLiteral("rgba(255,255,255,0.08)")
                                : QStringLiteral("rgba(32,33,36,0.09)");
    setStyleSheet(QStringLiteral(
        "ChatHistoryWindow { background: %1; }"
        "QWidget#chatHeader { background: %2; border-bottom: 1px solid %3; }"
        "QLabel#chatTitle { color: %4; font-size: 15px; font-weight: 600; }"
        "QLabel#chatSubtitle { color: %5; font-size: 12px; }"
        "QScrollArea#conversationScrollArea, QWidget#messageContainer { background: %1; border: none; }"
        "QWidget#chatInputPanel { background: %2; border-top: 1px solid %3; }"
        "QPlainTextEdit#messageInput { background: %1; color: %4; border: 1px solid %3; border-radius: 8px; padding: 8px; font-size: 13px; }"
        "QPushButton#mainActionButton { background: #0066cc; border: none; border-radius: 8px; padding: 5px; }"
        "QPushButton#mainActionButton:hover { background: #0b74de; }"
        "QPushButton#jumpToLatestButton { background: %2; border: 1px solid %3; border-radius: 8px; padding: 5px; }"
        "QPushButton#jumpToLatestButton:hover { background: %1; }"
        "QFrame#lastReadLine { color: %3; background: %3; border: none; max-height: 1px; }"
        "QLabel#lastReadCaption { color: %5; font-size: 12px; background: transparent; }")
                      .arg(background, surface, border, primary, caption));
    for (ChatMessageRow* row : m_rows) row->applyTheme(dark);
}

int ChatHistoryWindow::layoutIndexForModelIndex(int modelIndex) const {
    int seenRows = 0;
    for (int layoutIndex = 0; layoutIndex < m_messageLayout->count();
         ++layoutIndex) {
        QWidget* widget = m_messageLayout->itemAt(layoutIndex)->widget();
        if (widget && widget->objectName() == QLatin1String("chatMessageRow")) {
            if (seenRows == modelIndex) return layoutIndex;
            ++seenRows;
        }
    }
    return m_messageLayout->count();
}
