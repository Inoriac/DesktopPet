#include "switch_button.h"

#include "theme_manager.h"

#include <QEasingCurve>
#include <QPainter>
#include <QPropertyAnimation>
#include <QSizePolicy>
#include <QShowEvent>

#include <algorithm>

SwitchButton::SwitchButton(QWidget* parent)
    : QCheckBox(parent) {
    initialize(QString());
}

SwitchButton::SwitchButton(const QString& accessibleText, QWidget* parent)
    : QCheckBox(parent) {
    initialize(accessibleText);
}

void SwitchButton::initialize(const QString& accessibleText) {
    setObjectName(QStringLiteral("SwitchButton"));
    setCursor(Qt::PointingHandCursor);
    setText(QString());
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(accessibleText);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_knobAnimation = new QPropertyAnimation(this, "knobPosition", this);
    m_knobAnimation->setDuration(180);
    m_knobAnimation->setEasingCurve(QEasingCurve::OutCubic);

    connect(this, &QCheckBox::toggled, this, &SwitchButton::animateToState);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        update();
    });
}

QSize SwitchButton::sizeHint() const {
    return {48, 28};
}

QSize SwitchButton::minimumSizeHint() const {
    return sizeHint();
}

qreal SwitchButton::knobPosition() const {
    return m_knobPosition;
}

void SwitchButton::setKnobPosition(qreal position) {
    m_knobPosition = std::clamp(position, 0.0, 1.0);
    update();
}

bool SwitchButton::hitButton(const QPoint& position) const {
    return rect().contains(position);
}

void SwitchButton::showEvent(QShowEvent* event) {
    QCheckBox::showEvent(event);
    setKnobPosition(isChecked() ? 1.0 : 0.0);
}

void SwitchButton::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDarkTheme();
    const bool enabled = isEnabled();
    const QRectF trackRect(1.0, 3.0, width() - 2.0, height() - 6.0);
    const qreal radius = trackRect.height() / 2.0;

    QColor offColor = dark ? QColor(57, 65, 81) : QColor(208, 213, 221);
    QColor onColor = dark ? QColor(37, 99, 235) : QColor(0, 102, 204);
    QColor borderColor = dark ? QColor(255, 255, 255, 28) : QColor(32, 33, 36, 24);
    QColor knobColor = dark ? QColor(248, 250, 252) : QColor(255, 255, 255);

    if (!enabled) {
        offColor = dark ? QColor(43, 48, 60) : QColor(230, 234, 240);
        onColor = dark ? QColor(54, 72, 112) : QColor(164, 195, 230);
        borderColor = dark ? QColor(255, 255, 255, 18) : QColor(32, 33, 36, 14);
        knobColor = dark ? QColor(140, 149, 164) : QColor(248, 250, 252);
    }

    painter.setPen(QPen(borderColor, 1.0));
    painter.setBrush(isChecked() ? onColor : offColor);
    painter.drawRoundedRect(trackRect, radius, radius);

    const qreal knobSize = 20.0;
    const qreal xMin = trackRect.left() + 3.0;
    const qreal xMax = trackRect.right() - knobSize - 3.0;
    const qreal knobX = xMin + (xMax - xMin) * m_knobPosition;
    const QRectF knobRect(knobX, trackRect.center().y() - knobSize / 2.0, knobSize, knobSize);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, dark ? 42 : 26));
    painter.drawEllipse(knobRect.translated(0.0, 1.0));
    painter.setBrush(knobColor);
    painter.drawEllipse(knobRect);
}

void SwitchButton::animateToState(bool checked) {
    if (!m_knobAnimation) {
        setKnobPosition(checked ? 1.0 : 0.0);
        return;
    }
    m_knobAnimation->stop();
    m_knobAnimation->setStartValue(m_knobPosition);
    m_knobAnimation->setEndValue(checked ? 1.0 : 0.0);
    m_knobAnimation->start();
}
