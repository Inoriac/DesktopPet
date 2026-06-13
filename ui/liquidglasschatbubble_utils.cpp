#include "liquidglasschatbubble.h"

#include <QAbstractAnimation>
#include <QPalette>

QRect LiquidGlassChatBubble::moreIndicatorRect() const {
    return QRect(width() - m_paddingH - 14, height() - m_paddingV - 10, 14, 10);
}

void LiquidGlassChatBubble::setInputRevealed(bool revealed) {
    if (m_inputRevealed == revealed && m_opacityAnimation && m_opacityAnimation->state() == QAbstractAnimation::Stopped) {
        return;
    }

    m_inputRevealed = revealed;
    const qreal targetOpacity = revealed ? 1.0 : 0.0;
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

int LiquidGlassChatBubble::contentWidth() const {
    return 380 - m_paddingH * 2;
}