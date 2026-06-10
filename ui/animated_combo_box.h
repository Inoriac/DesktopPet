#ifndef DESKTOP_PET_ANIMATED_COMBO_BOX_H
#define DESKTOP_PET_ANIMATED_COMBO_BOX_H

#include <QComboBox>

class QPropertyAnimation;

class AnimatedComboBox : public QComboBox {
    Q_OBJECT
    Q_PROPERTY(qreal arrowRotation READ arrowRotation WRITE setArrowRotation)

public:
    explicit AnimatedComboBox(QWidget* parent = nullptr);

    qreal arrowRotation() const;
    void setArrowRotation(qreal rotation);

    void showPopup() override;
    void hidePopup() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void animateArrow(qreal targetRotation);

    QPropertyAnimation* m_arrowAnimation = nullptr;
    qreal m_arrowRotation = 0.0;
};

#endif // DESKTOP_PET_ANIMATED_COMBO_BOX_H
