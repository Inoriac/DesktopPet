#include "liquidglasschatbubble.h"

#include <QAbstractAnimation>
#include <QFontMetrics>
#include <QPalette>
#include <QStyle>
#include <QToolButton>

#include <algorithm>

namespace {
constexpr int kControlsHeight = 40;
constexpr int kControlButtonSize = 32;
constexpr int kControlGap = 4;
constexpr qreal kIdleInputOpacity = 0.32;
}

QRect LiquidGlassChatBubble::moreIndicatorRect() const {
    return QRect(width() - m_paddingH - 14, height() - m_paddingV - 10, 14, 10);
}

void LiquidGlassChatBubble::setInputRevealed(bool revealed) {
    if (m_inputRevealed == revealed && m_opacityAnimation && m_opacityAnimation->state() == QAbstractAnimation::Stopped) {
        return;
    }

    m_inputRevealed = revealed;
    const qreal targetOpacity = revealed ? 1.0 : kIdleInputOpacity;
    if (!isVisible()) {
        if (m_opacityAnimation) {
            m_opacityAnimation->stop();
        }
        setWindowOpacity(targetOpacity);
        return;
    }

    if (!m_opacityAnimation) {
        m_opacityAnimation = new QPropertyAnimation(this, "windowOpacity", this);
        m_opacityAnimation->setDuration(200);
    }
    m_opacityAnimation->stop();
    m_opacityAnimation->setStartValue(windowOpacity());
    m_opacityAnimation->setEndValue(targetOpacity);
    m_opacityAnimation->start();
}

void LiquidGlassChatBubble::updateTextPalette(const QColor& color) {
    if (!m_input) {
        return;
    }
    QPalette pal = m_input->palette();
    pal.setColor(QPalette::Text, color);
    pal.setColor(QPalette::PlaceholderText, QColor(color.red(), color.green(), color.blue(), 150));
    m_input->setPalette(pal);
    m_input->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; }") );
}

void LiquidGlassChatBubble::updateInputGeometry() {
    if (m_input) {
        m_input->setGeometry(rect().adjusted(m_paddingH, 8, -m_paddingH, -8));
    }
}

QRect LiquidGlassChatBubble::displayedTextRect() const {
    QRect textRect = rect().adjusted(
        m_paddingH + 1, m_paddingV + 1,
        -m_paddingH - 1, -m_paddingV - 1);
    if (m_streamingMode) textRect.adjust(0, 0, 0, -kControlsHeight);
    return textRect;
}

QRect LiquidGlassChatBubble::displayedTextBoundingRect() const {
    QRect textRect = displayedTextRect();
    if (m_hasMorePages) textRect.adjust(0, 0, 0, -16);
    const QString displayedText = m_text.isEmpty()
        ? m_activityText : m_text;
    const QFontMetrics metrics(fittedOutputFont(textRect, displayedText));
    QRect bounds = metrics.boundingRect(
        QRect(0, 0, textRect.width(), 10000),
        Qt::TextWordWrap | Qt::TextWrapAnywhere,
        displayedText);
    bounds.moveTopLeft(QPoint(
        textRect.left() + std::max(0, (textRect.width() - bounds.width()) / 2),
        textRect.top() + std::max(0, (textRect.height() - bounds.height()) / 2)));
    return bounds;
}

QRect LiquidGlassChatBubble::playbackControlsRect() const {
    if (!m_streamingMode) return {};
    return QRect(m_paddingH,
                 height() - m_paddingV - kControlsHeight,
                 std::max(0, width() - m_paddingH * 2),
                 kControlsHeight);
}

void LiquidGlassChatBubble::updateOutputControlsGeometry() {
    if (!m_streamingMode) return;
    const QRect controls = playbackControlsRect();
    const int top = controls.top() + (controls.height() - kControlButtonSize) / 2;
    int left = controls.right() + 1 - kControlButtonSize;
    for (QToolButton* button : {m_openButton.data(), m_playbackButton.data(),
                                m_nextButton.data(), m_previousButton.data()}) {
        if (!button) continue;
        button->setGeometry(left, top, kControlButtonSize, kControlButtonSize);
        left -= kControlButtonSize + kControlGap;
    }
}

void LiquidGlassChatBubble::updateOutputControlsState() {
    for (QToolButton* button : {m_previousButton.data(), m_nextButton.data(),
                                m_playbackButton.data(), m_openButton.data()}) {
        if (button) button->setVisible(m_streamingMode);
    }
    if (!m_streamingMode) return;
    if (m_previousButton) m_previousButton->setEnabled(m_displayedPageIndex > 0);
    if (m_nextButton) {
        m_nextButton->setEnabled(
            m_displayedPageIndex + 1 < m_displayedPageTotal);
    }
    if (m_playbackButton) {
        m_playbackButton->setEnabled(m_displayedPageTotal > 1);
        m_playbackButton->setIcon(style()->standardIcon(
            m_playbackPaused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
        const QString label = m_playbackPaused
            ? QStringLiteral("继续自动播放")
            : QStringLiteral("暂停自动播放");
        m_playbackButton->setToolTip(label);
        m_playbackButton->setAccessibleName(label);
    }
    if (m_openButton) m_openButton->setEnabled(!m_messageId.isEmpty());
    updateOutputControlsGeometry();
}

bool LiquidGlassChatBubble::inputShouldStayRevealed() const {
    return underMouse() || (m_input && (m_input->hasFocus()
        || !m_input->text().isEmpty()));
}

QFont LiquidGlassChatBubble::fittedOutputFont(const QRect& textRect,
                                              const QString& text) const {
    QFont candidate = font();
    candidate.setWeight(QFont::DemiBold);
    const QString measured = text.isEmpty() ? QStringLiteral("...") : text;
    for (int pointSize = m_fontSize; pointSize >= 6; --pointSize) {
        candidate.setPointSize(pointSize);
        const QRect bounds = QFontMetrics(candidate).boundingRect(
            QRect(0, 0, textRect.width(), 10000),
            Qt::TextWordWrap | Qt::TextWrapAnywhere,
            measured);
        if (bounds.width() <= textRect.width()
            && bounds.height() <= textRect.height()) {
            return candidate;
        }
    }
    candidate.setPointSize(6);
    return candidate;
}

int LiquidGlassChatBubble::contentWidth() const {
    return 380 - m_paddingH * 2;
}
