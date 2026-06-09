#include "chat_history_window.h"

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

QColor bubbleColorForRole(const QString& role) {
    if (role == QStringLiteral("user")) {
        return QColor(149, 236, 105);
    }
    if (role == QStringLiteral("assistant")) {
        return QColor(255, 255, 255);
    }
    return QColor(235, 239, 245);
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

    auto* titleBar = new QLabel(QStringLiteral("  桌宠私聊"), this);
    QFont titleFont = titleBar->font();
    titleFont.setPointSize(13);
    titleFont.setBold(true);
    titleBar->setFont(titleFont);
    titleBar->setFixedHeight(44);
    titleBar->setStyleSheet(QStringLiteral(
        "QLabel { background: #f4f6fb; color: #222; border-bottom: 1px solid #dde2eb; }"));
    rootLayout->addWidget(titleBar);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(QStringLiteral("QScrollArea { background: #eef2f8; border: none; }"));

    m_messageContainer = new QWidget(m_scrollArea);
    m_messageContainer->setStyleSheet(QStringLiteral("QWidget { background: #eef2f8; }"));
    m_messageLayout = new QVBoxLayout(m_messageContainer);
    m_messageLayout->setContentsMargins(14, 14, 14, 14);
    m_messageLayout->setSpacing(10);
    m_messageLayout->addStretch(1);
    m_scrollArea->setWidget(m_messageContainer);
    rootLayout->addWidget(m_scrollArea, 1);

    auto* inputPanel = new QWidget(this);
    inputPanel->setStyleSheet(QStringLiteral(
        "QWidget { background: #f7f9fc; border-top: 1px solid #dde2eb; }"));
    auto* inputLayout = new QHBoxLayout(inputPanel);
    inputLayout->setContentsMargins(12, 10, 12, 10);
    inputLayout->setSpacing(8);

    m_inputEdit = new QTextEdit(inputPanel);
    m_inputEdit->setAcceptRichText(false);
    m_inputEdit->setPlaceholderText(QStringLiteral("和桌宠说点什么..."));
    m_inputEdit->setFixedHeight(72);
    m_inputEdit->installEventFilter(this);
    m_inputEdit->setStyleSheet(QStringLiteral(
        "QTextEdit { background: white; border: 1px solid #d6dce6; border-radius: 8px; padding: 8px; font-size: 13px; }"));
    inputLayout->addWidget(m_inputEdit, 1);

    m_sendButton = new QPushButton(QStringLiteral("发送"), inputPanel);
    m_sendButton->setFixedSize(70, 36);
    m_sendButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #07c160; color: white; border: none; border-radius: 8px; font-weight: 600; }"
        "QPushButton:hover { background: #06ad56; }"
        "QPushButton:pressed { background: #05964a; }"));
    inputLayout->addWidget(m_sendButton, 0, Qt::AlignBottom);

    rootLayout->addWidget(inputPanel);

    connect(m_sendButton, &QPushButton::clicked, this, &ChatHistoryWindow::sendCurrentMessage);
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
    bubble->setTextFormat(Qt::PlainText);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bubble->setWordWrap(true);
    bubble->setMaximumWidth(300);
    bubble->setText(QStringLiteral("%1  %2\n%3")
        .arg(displayNameForRole(role), timestamp.toString(QStringLiteral("HH:mm")), content.trimmed()));
    bubble->setStyleSheet(QStringLiteral(
        "QLabel { background: %1; color: #222; border-radius: 10px; padding: 9px 11px; font-size: 13px; line-height: 1.35; }")
        .arg(colorToStyle(bubbleColorForRole(role))));

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
