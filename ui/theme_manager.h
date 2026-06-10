#ifndef DESKTOP_PET_THEME_MANAGER_H
#define DESKTOP_PET_THEME_MANAGER_H

#include <QObject>
#include <QString>

class QApplication;
class QWidget;

class ThemeManager : public QObject {
    Q_OBJECT

public:
    enum class Theme {
        Light,
        Dark
    };
    Q_ENUM(Theme)

    static ThemeManager& instance();

    Theme currentTheme() const;
    bool isDarkTheme() const;
    void setTheme(Theme theme);
    void toggleTheme();
    void applyTo(QApplication* app);
    void applyHeroPalette(QWidget* hero) const;
    QString comboArrowColor() const;

signals:
    void themeChanged(Theme theme);

private:
    explicit ThemeManager(QObject* parent = nullptr);
    QString styleSheet() const;
    QString lightStyleSheet() const;
    QString darkStyleSheet() const;

    Theme m_theme = Theme::Dark;
};

#endif // DESKTOP_PET_THEME_MANAGER_H
