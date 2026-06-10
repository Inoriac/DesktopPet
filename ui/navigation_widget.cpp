#include "navigation_widget.h"

#include <QButtonGroup>
#include <QEasingCurve>
#include <QEvent>
#include <QPushButton>
#include <QPropertyAnimation>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>

NavigationWidget::NavigationWidget(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("NavigationWidget"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(80);
    setMaximumWidth(m_railWidth);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    m_buttonGroup = new QButtonGroup(this);
    m_buttonGroup->setExclusive(true);

    m_widthAnimation = new QPropertyAnimation(this, "railWidth", this);
    m_widthAnimation->setDuration(180);
    m_widthAnimation->setEasingCurve(QEasingCurve::OutCubic);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(12, 14, 12, 14);
    m_layout->setSpacing(8);
    m_layout->addStretch();
}

int NavigationWidget::railWidth() const {
    return m_railWidth;
}

void NavigationWidget::setRailWidth(int width) {
    m_railWidth = std::clamp(width, 80, 220);
    setMinimumWidth(m_railWidth);
    setMaximumWidth(m_railWidth);
}

void NavigationWidget::addItem(const QString& id, const QString& text) {
    QPushButton* button = createButton(id, text);
    m_buttons.insert(id, button);
    m_buttonGroup->addButton(button);
    m_layout->insertWidget(std::max(0, m_layout->count() - 1), button);

    connect(button, &QPushButton::clicked, this, [this, id]() {
        emit navigationRequested(id);
    });

    if (m_buttons.size() == 1) {
        button->setChecked(true);
    }
}

void NavigationWidget::setCurrentItem(const QString& id) {
    if (QPushButton* button = m_buttons.value(id, nullptr)) {
        button->setChecked(true);
    }
}

void NavigationWidget::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    animateRailWidth(188);
}

void NavigationWidget::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    animateRailWidth(168);
}

QPushButton* NavigationWidget::createButton(const QString& id, const QString& text) {
    auto* button = new QPushButton(text, this);
    button->setObjectName(QStringLiteral("NavButton"));
    button->setProperty("navId", id);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(42);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return button;
}

void NavigationWidget::animateRailWidth(int targetWidth) {
    if (!m_widthAnimation) {
        setRailWidth(targetWidth);
        return;
    }
    m_widthAnimation->stop();
    m_widthAnimation->setStartValue(m_railWidth);
    m_widthAnimation->setEndValue(targetWidth);
    m_widthAnimation->start();
}
