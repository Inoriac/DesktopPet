#include "navigation_widget.h"

#include <QButtonGroup>
#include <QEasingCurve>
#include <QEvent>
#include <QFrame>
#include <QPushButton>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QTimer>
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

    m_selectionPill = new QFrame(this);
    m_selectionPill->setObjectName(QStringLiteral("NavSelectionPill"));
    m_selectionPill->setAttribute(Qt::WA_StyledBackground, true);
    m_selectionPill->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_selectionPill->hide();
    m_selectionPill->lower();

    m_selectionAnimation = new QPropertyAnimation(m_selectionPill, "geometry", this);
    m_selectionAnimation->setDuration(260);
    m_selectionAnimation->setEasingCurve(QEasingCurve::OutCubic);

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
        setCurrentItem(id);
    }
}

void NavigationWidget::setCurrentItem(const QString& id) {
    if (QPushButton* button = m_buttons.value(id, nullptr)) {
        button->setChecked(true);
        const bool changed = m_currentId != id;
        m_currentId = id;
        moveSelectionTo(id, changed);
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

void NavigationWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!m_currentId.isEmpty()) {
        moveSelectionTo(m_currentId, false);
    }
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

void NavigationWidget::moveSelectionTo(const QString& id, bool animated) {
    QPushButton* button = m_buttons.value(id, nullptr);
    if (!button || !m_selectionPill) {
        return;
    }

    const QRect target = button->geometry().adjusted(0, 1, 0, -1);
    if (target.width() <= 0 || target.height() <= 0) {
        QTimer::singleShot(0, this, [this, id, animated]() {
            moveSelectionTo(id, animated);
        });
        return;
    }

    m_selectionPill->show();
    m_selectionPill->lower();

    if (!animated || !m_selectionPill->geometry().isValid() || m_selectionPill->geometry().isNull()) {
        if (m_selectionAnimation) {
            m_selectionAnimation->stop();
        }
        m_selectionPill->setGeometry(target);
        return;
    }

    m_selectionAnimation->stop();
    m_selectionAnimation->setStartValue(m_selectionPill->geometry());
    m_selectionAnimation->setEndValue(target);
    m_selectionAnimation->start();
}
