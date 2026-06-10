#include "card_widget.h"

#include <QColor>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QVBoxLayout>

CardWidget::CardWidget(const QString& title, const QString& subtitle, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("CardWidget"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(28.0);
    shadow->setOffset(0, 8);
    shadow->setColor(QColor(0, 0, 0, 28));
    setGraphicsEffect(shadow);

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(22, 20, 22, 22);
    outerLayout->setSpacing(16);

    auto* headerLayout = new QVBoxLayout;
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(4);

    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setObjectName(QStringLiteral("CardTitle"));
    headerLayout->addWidget(m_titleLabel);

    if (!subtitle.trimmed().isEmpty()) {
        m_subtitleLabel = new QLabel(subtitle, this);
        m_subtitleLabel->setObjectName(QStringLiteral("CardSubtitle"));
        m_subtitleLabel->setWordWrap(true);
        headerLayout->addWidget(m_subtitleLabel);
    }

    outerLayout->addLayout(headerLayout);

    m_contentLayout = new QVBoxLayout;
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->setSpacing(12);
    outerLayout->addLayout(m_contentLayout);
}

QVBoxLayout* CardWidget::contentLayout() const {
    return m_contentLayout;
}

void CardWidget::addWidget(QWidget* widget) {
    if (m_contentLayout && widget) {
        m_contentLayout->addWidget(widget);
    }
}
