#ifndef DESKTOP_PET_SWITCH_BUTTON_H
#define DESKTOP_PET_SWITCH_BUTTON_H

#include <QCheckBox>

class QPropertyAnimation;

class SwitchButton : public QCheckBox {
    Q_OBJECT
    Q_PROPERTY(qreal knobPosition READ knobPosition WRITE setKnobPosition)

public:
    explicit SwitchButton(QWidget* parent = nullptr);
    explicit SwitchButton(const QString& accessibleText, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    qreal knobPosition() const;
    void setKnobPosition(qreal position);

protected:
    bool hitButton(const QPoint& position) const override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void initialize(const QString& accessibleText);
    void animateToState(bool checked);

    QPropertyAnimation* m_knobAnimation = nullptr;
    qreal m_knobPosition = 0.0;
};

#endif // DESKTOP_PET_SWITCH_BUTTON_H
