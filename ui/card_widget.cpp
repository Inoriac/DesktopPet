#include "card_widget.h"

#include <QColor>
#include <QFrame>
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
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    m_headerWidget = new QWidget(this);
    m_headerWidget->setObjectName(QStringLiteral("CardHeader"));
    m_headerWidget->setAttribute(Qt::WA_StyledBackground, true);
    auto* headerLayout = new QVBoxLayout(m_headerWidget);
    headerLayout->setContentsMargins(22, 18, 22, 16);
    headerLayout->setSpacing(4);

    m_titleLabel = new QLabel(title, m_headerWidget);
    m_titleLabel->setObjectName(QStringLiteral("CardTitle"));
    headerLayout->addWidget(m_titleLabel);

    if (!subtitle.trimmed().isEmpty()) {
        m_subtitleLabel = new QLabel(subtitle, m_headerWidget);
        m_subtitleLabel->setObjectName(QStringLiteral("CardSubtitle"));
        m_subtitleLabel->setWordWrap(true);
        headerLayout->addWidget(m_subtitleLabel);
    }

    outerLayout->addWidget(m_headerWidget);

    m_bodyWidget = new QWidget(this);
    m_bodyWidget->setObjectName(QStringLiteral("CardBody"));
    m_bodyWidget->setAttribute(Qt::WA_StyledBackground, true);
    m_contentLayout = new QVBoxLayout(m_bodyWidget);
    m_contentLayout->setContentsMargins(22, 16, 22, 20);
    m_contentLayout->setSpacing(0);
    outerLayout->addWidget(m_bodyWidget);
}

QVBoxLayout* CardWidget::contentLayout() const {
    return m_contentLayout;
}

void CardWidget::addWidget(QWidget* widget) {
    if (m_contentLayout && widget) {
        if (m_hasContent) {
            auto* separator = new QFrame(m_bodyWidget ? m_bodyWidget : this);
            separator->setObjectName(QStringLiteral("SettingSeparator"));
            separator->setFrameShape(QFrame::NoFrame);
            separator->setFixedHeight(1);
            m_contentLayout->addSpacing(14);
            m_contentLayout->addWidget(separator);
            m_contentLayout->addSpacing(14);
        }
        m_contentLayout->addWidget(widget);
        m_hasContent = true;
    }
}
