#ifndef DESKTOP_PET_ROUND_SLIDER_H
#define DESKTOP_PET_ROUND_SLIDER_H

#include <QSlider>

class RoundSlider : public QSlider {
    Q_OBJECT

public:
    explicit RoundSlider(Qt::Orientation orientation, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

#endif // DESKTOP_PET_ROUND_SLIDER_H
