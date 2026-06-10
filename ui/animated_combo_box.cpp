#include "animated_combo_box.h"

#include "theme_manager.h"

#include <QEasingCurve>
#include <QPainter>
#include <QPropertyAnimation>
#include <QStylePainter>
#include <QStyleOptionComboBox>

AnimatedComboBox::AnimatedComboBox(QWidget* parent)
    : QComboBox(parent) {
    setObjectName(QStringLiteral("AnimatedComboBox"));
    m_arrowAnimation = new QPropertyAnimation(this, "arrowRotation", this);
    m_arrowAnimation->setDuration(180);
    m_arrowAnimation->setEasingCurve(QEasingCurve::OutCubic);
}

qreal AnimatedComboBox::arrowRotation() const {
    return m_arrowRotation;
}

void AnimatedComboBox::setArrowRotation(qreal rotation) {
    m_arrowRotation = rotation;
    update();
}

void AnimatedComboBox::showPopup() {
    animateArrow(180.0);
    QComboBox::showPopup();
}

void AnimatedComboBox::hidePopup() {
    QComboBox::hidePopup();
    animateArrow(0.0);
}

void AnimatedComboBox::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)

    QStylePainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QStyleOptionComboBox option;
    initStyleOption(&option);
    option.subControls &= ~QStyle::SC_ComboBoxArrow;
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    painter.drawControl(QStyle::CE_ComboBoxLabel, option);

    const QRect arrowRect = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
    const QPointF center(arrowRect.center().x() + 0.5, arrowRect.center().y() + 0.5);
    const QColor arrowColor(ThemeManager::instance().comboArrowColor());

    QPolygonF triangle;
    triangle << QPointF(0.0, 3.0)
             << QPointF(5.0, -3.0)
             << QPointF(-5.0, -3.0);

    painter.save();
    painter.translate(center);
    painter.rotate(m_arrowRotation);
    painter.setPen(Qt::NoPen);
    painter.setBrush(arrowColor);
    painter.drawPolygon(triangle);
    painter.restore();
}

void AnimatedComboBox::animateArrow(qreal targetRotation) {
    if (!m_arrowAnimation) {
        setArrowRotation(targetRotation);
        return;
    }
    m_arrowAnimation->stop();
    m_arrowAnimation->setStartValue(m_arrowRotation);
    m_arrowAnimation->setEndValue(targetRotation);
    m_arrowAnimation->start();
}
