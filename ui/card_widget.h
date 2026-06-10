#ifndef DESKTOP_PET_CARD_WIDGET_H
#define DESKTOP_PET_CARD_WIDGET_H

#include <QWidget>

class QLabel;
class QVBoxLayout;

class CardWidget : public QWidget {
    Q_OBJECT

public:
    explicit CardWidget(const QString& title,
                        const QString& subtitle = QString(),
                        QWidget* parent = nullptr);

    QVBoxLayout* contentLayout() const;
    void addWidget(QWidget* widget);

private:
    QLabel* m_titleLabel = nullptr;
    QLabel* m_subtitleLabel = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
};

#endif // DESKTOP_PET_CARD_WIDGET_H
