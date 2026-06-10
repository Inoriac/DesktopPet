#include "round_slider.h"

#include "theme_manager.h"

#include <QPainter>
#include <QStyleOptionSlider>

RoundSlider::RoundSlider(Qt::Orientation orientation, QWidget* parent)
    : QSlider(orientation, parent) {
    setObjectName(QStringLiteral("RoundSlider"));
    setMouseTracking(true);
    setMinimumHeight(28);
    setSingleStep(1);
}

void RoundSlider::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)

    if (orientation() != Qt::Horizontal) {
        QSlider::paintEvent(event);
        return;
    }

    QStyleOptionSlider option;
    initStyleOption(&option);

    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);

    const bool dark = ThemeManager::instance().isDarkTheme();
    const bool hovered = rect().contains(mapFromGlobal(QCursor::pos()));
    const bool pressed = isSliderDown();
    const int grooveHeight = pressed ? 6 : 5;
    const int grooveY = rect().center().y() - grooveHeight / 2;
    const QRectF grooveRect(groove.left(), grooveY, groove.width(), grooveHeight);
    const QRectF activeRect(groove.left(), grooveY, handle.center().x() - groove.left(), grooveHeight);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    painter.setBrush(dark ? QColor(QStringLiteral("#2b3344")) : QColor(QStringLiteral("#d9e2ef")));
    painter.drawRoundedRect(grooveRect, grooveHeight / 2.0, grooveHeight / 2.0);

    painter.setBrush(dark ? QColor(QStringLiteral("#60a5fa")) : QColor(QStringLiteral("#0066cc")));
    painter.drawRoundedRect(activeRect, grooveHeight / 2.0, grooveHeight / 2.0);

    const qreal radius = (hovered || pressed) ? 8.5 : 7.5;
    const QPointF center = handle.center();
    QRectF knob(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);

    painter.setBrush(dark ? QColor(QStringLiteral("#111827")) : QColor(QStringLiteral("#f8fafc")));
    painter.setPen(QPen(dark ? QColor(QStringLiteral("#93c5fd")) : QColor(QStringLiteral("#005fb8")), 2.2));
    if (hovered || pressed) {
        painter.setPen(QPen(dark ? QColor(QStringLiteral("#dbeafe")) : QColor(QStringLiteral("#003f7d")), 2.4));
        painter.setBrush(dark ? QColor(QStringLiteral("#0b1120")) : QColor(QStringLiteral("#ffffff")));
    }
    painter.drawEllipse(knob);
}
