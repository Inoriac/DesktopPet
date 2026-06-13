#include "chat_history_window.h"

#include "theme_manager.h"

#include <QApplication>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
QString displayNameForRole(const QString& role) {
    if (role == QStringLiteral("user")) {
        return QStringLiteral("你");
    }
    if (role == QStringLiteral("assistant")) {
        return QStringLiteral("桌宠");
    }
    if (role == QStringLiteral("system")) {
        return QStringLiteral("系统");
    }
    return role;
}

QColor bubbleColorForRole(const QString& role, bool dark) {
    if (role == QStringLiteral("user")) {
        return dark ? QColor(22, 101, 52) : QColor(149, 236, 105);
    }
    if (role == QStringLiteral("assistant")) {
        return dark ? QColor(31, 41, 55) : QColor(255, 255, 255);
    }
    return dark ? QColor(55, 65, 81) : QColor(235, 239, 245);
}

QColor textColorForRole(const QString& role, bool dark) {
    if (!dark) {
        return QColor(34, 34, 34);
    }
    if (role == QStringLiteral("user")) {
        return QColor(236, 253, 245);
    }
    return QColor(237, 242, 247);
}

QString colorToStyle(const QColor& color) {
    return QStringLiteral("rgb(%1,%2,%3)").arg(color.red()).arg(color.green()).arg(color.blue());
}
}

ChatHistoryWindow::ChatHistoryWindow(QWidget* parent)
    : QWidget(parent) {
    setWindowTitle(QStringLiteral("桌宠私聊"));
    resize(460, 680);
    setMinimumSize(360, 520);
    setAttribute(Qt::WA_DeleteOnClose, false);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_titleBar = new QLabel(QStringLiteral("  桌宠私聊"), this);
    QFont titleFont = m_titleBar->font();
    titleFont.setPointSize(13);
    titleFont.setBold(true);
    m_titleBar->setFont(titleFont);
    m_titleBar->setFixedHeight(44);
    rootLayout->addWidget(m_titleBar);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    m_messageContainer = new QWidget(m_scrollArea);
    m_messageLayout = new QVBoxLayout(m_messageContainer);
    m_messageLayout->setContentsMargins(14, 14, 14, 14);
    m_messageLayout->setSpacing(10);
    m_messageLayout->addStretch(1);
    m_scrollArea->setWidget(m_messageContainer);
    rootLayout->addWidget(m_scrollArea, 1);

    m_inputPanel = new QWidget(this);
    auto* inputLayout = new QHBoxLayout(m_inputPanel);
    inputLayout->setContentsMargins(12, 10, 12, 10);
    inputLayout->setSpacing(8);

    m_inputEdit = new QTextEdit(m_inputPanel);
    m_inputEdit->setAcceptRichText(false);
    m_inputEdit->setPlaceholderText(QStringLiteral("和桌宠说点什么..."));
    m_inputEdit->setFixedHeight(72);
    m_inputEdit->installEventFilter(this);
    inputLayout->addWidget(m_inputEdit, 1);

    m_sendButton = new QPushButton(QStringLiteral("发送"), m_inputPanel);
    m_sendButton->setFixedSize(70, 36);
    m_sendButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #07c160; color: white; border: none; border-radius: 8px; font-weight: 600; }"
        "QPushButton:hover { background: #06ad56; }"
        "QPushButton:pressed { background: #05964a; }"));
    inputLayout->addWidget(m_sendButton, 0, Qt::AlignBottom);

    rootLayout->addWidget(m_inputPanel);

    connect(m_sendButton, &QPushButton::clicked, this, &ChatHistoryWindow::sendCurrentMessage);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &ChatHistoryWindow::applyThemeStyle);
    applyThemeStyle();
}

bool ChatHistoryWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_inputEdit && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const bool enterPressed = keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
        const bool wantsNewLine = keyEvent->modifiers().testFlag(Qt::ShiftModifier);
        if (enterPressed && !wantsNewLine) {
            sendCurrentMessage();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ChatHistoryWindow::addMessage(const QString& role, const QString& content, const QDateTime& timestamp) {
    if (!m_messageLayout || content.trimmed().isEmpty()) {
        return;
    }

    const bool isUser = role == QStringLiteral("user");
    auto* row = new QWidget(m_messageContainer);
    row->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    auto* bubble = new QLabel(row);
    bubble->setProperty("chatRole", role);
    bubble->setTextFormat(Qt::PlainText);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bubble->setWordWrap(true);
    bubble->setMaximumWidth(300);
    bubble->setText(QStringLiteral("%1  %2\n%3")
        .arg(displayNameForRole(role), timestamp.toString(QStringLiteral("HH:mm")), content.trimmed()));
    bubble->setStyleSheet(messageBubbleStyle(role));

    if (isUser) {
        rowLayout->addStretch(1);
        rowLayout->addWidget(bubble, 0, Qt::AlignRight);
    } else {
        rowLayout->addWidget(bubble, 0, Qt::AlignLeft);
        rowLayout->addStretch(1);
    }

    const int insertIndex = std::max(0, m_messageLayout->count() - 1);
    m_messageLayout->insertWidget(insertIndex, row);
    scrollToBottom();
}

void ChatHistoryWindow::clearMessages() {
    if (!m_messageLayout) {
        return;
    }

    while (m_messageLayout->count() > 1) {
        QLayoutItem* item = m_messageLayout->takeAt(0);
        if (QWidget* widget = item ? item->widget() : nullptr) {
            widget->deleteLater();
        }
        delete item;
    }
}

void ChatHistoryWindow::sendCurrentMessage() {
    if (!m_inputEdit) {
        return;
    }

    const QString text = m_inputEdit->toPlainText().trimmed();
    if (text.isEmpty()) {
        return;
    }

    m_inputEdit->clear();
    emit messageSubmitted(text);
}

void ChatHistoryWindow::scrollToBottom() {
    if (!m_scrollArea) {
        return;
    }

    QTimer::singleShot(0, this, [this]() {
        if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
            m_scrollArea->verticalScrollBar()->setValue(m_scrollArea->verticalScrollBar()->maximum());
        }
    });
}

void ChatHistoryWindow::applyThemeStyle() {
    const bool dark = ThemeManager::instance().isDarkTheme();
    const QString titleBg = dark ? QStringLiteral("#111827") : QStringLiteral("#f4f6fb");
    const QString titleText = dark ? QStringLiteral("#edf2f7") : QStringLiteral("#222222");
    const QString border = dark ? QStringLiteral("rgba(255,255,255,0.08)") : QStringLiteral("#dde2eb");
    const QString chatBg = dark ? QStringLiteral("#0f172a") : QStringLiteral("#eef2f8");
    const QString inputBg = dark ? QStringLiteral("#111827") : QStringLiteral("#f7f9fc");
    const QString editBg = dark ? QStringLiteral("#0b1220") : QStringLiteral("#ffffff");
    const QString editText = dark ? QStringLiteral("#edf2f7") : QStringLiteral("#202124");
    const QString editBorder = dark ? QStringLiteral("rgba(255,255,255,0.12)") : QStringLiteral("#d6dce6");

    if (m_titleBar) {
        m_titleBar->setStyleSheet(QStringLiteral(
            "QLabel { background: %1; color: %2; border-bottom: 1px solid %3; }")
            .arg(titleBg, titleText, border));
    }
    if (m_scrollArea) {
        m_scrollArea->setStyleSheet(QStringLiteral("QScrollArea { background: %1; border: none; }").arg(chatBg));
    }
    if (m_messageContainer) {
        m_messageContainer->setStyleSheet(QStringLiteral("QWidget { background: %1; }").arg(chatBg));
        const QList<QLabel*> bubbles = m_messageContainer->findChildren<QLabel*>();
        for (QLabel* bubble : bubbles) {
            const QString role = bubble->property("chatRole").toString();
            if (!role.isEmpty()) {
                bubble->setStyleSheet(messageBubbleStyle(role));
            }
        }
    }
    if (m_inputPanel) {
        m_inputPanel->setStyleSheet(QStringLiteral(
            "QWidget { background: %1; border-top: 1px solid %2; }").arg(inputBg, border));
    }
    if (m_inputEdit) {
        m_inputEdit->setStyleSheet(QStringLiteral(
            "QTextEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 8px; font-size: 13px; }")
            .arg(editBg, editText, editBorder));
    }
}

QString ChatHistoryWindow::messageBubbleStyle(const QString& role) const {
    const bool dark = ThemeManager::instance().isDarkTheme();
    return QStringLiteral(
        "QLabel { background: %1; color: %2; border-radius: 10px; padding: 9px 11px; font-size: 13px; line-height: 1.35; }")
        .arg(colorToStyle(bubbleColorForRole(role, dark)), colorToStyle(textColorForRole(role, dark)));
}
