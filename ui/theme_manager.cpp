#include "theme_manager.h"

#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QWidget>

ThemeManager& ThemeManager::instance() {
    static ThemeManager manager;
    return manager;
}

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent) {
    loadPersistedTheme();
}

ThemeManager::Theme ThemeManager::currentTheme() const {
    return m_theme;
}

bool ThemeManager::isDarkTheme() const {
    return m_theme == Theme::Dark;
}

void ThemeManager::setTheme(Theme theme) {
    if (m_theme == theme) {
        return;
    }
    m_theme = theme;
    persistTheme();
    if (auto* app = qobject_cast<QApplication*>(QApplication::instance())) {
        applyTo(app);
    }
    emit themeChanged(m_theme);
}

void ThemeManager::toggleTheme() {
    setTheme(isDarkTheme() ? Theme::Light : Theme::Dark);
}

void ThemeManager::applyTo(QApplication* app) {
    if (!app) {
        return;
    }
    QPalette palette = app->palette();
    if (isDarkTheme()) {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#0f1117")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#111827")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#edf2f7")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#edf2f7")));
    } else {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#f4f7fb")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#202124")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#202124")));
    }
    app->setPalette(palette);
    app->setStyleSheet(styleSheet());
}

void ThemeManager::applyHeroPalette(QWidget* hero) const {
    if (!hero) {
        return;
    }
    hero->setProperty("theme", isDarkTheme() ? "dark" : "light");
    hero->style()->unpolish(hero);
    hero->style()->polish(hero);
}

QString ThemeManager::comboArrowColor() const {
    return isDarkTheme() ? QStringLiteral("#9aa4b2") : QStringLiteral("#667085");
}

QString ThemeManager::styleSheet() const {
    return isDarkTheme() ? darkStyleSheet() : lightStyleSheet();
}

void ThemeManager::loadPersistedTheme() {
    const QString value = QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("light")).toString().trimmed().toLower();
    m_theme = value == QStringLiteral("dark") ? Theme::Dark : Theme::Light;
}

void ThemeManager::persistTheme() const {
    QSettings().setValue(QStringLiteral("ui/theme"), isDarkTheme() ? QStringLiteral("dark") : QStringLiteral("light"));
}

QString ThemeManager::lightStyleSheet() const {
    return QStringLiteral(R"qss(
* {
    font-family: "Segoe UI", "Microsoft YaHei UI", "Microsoft YaHei";
    font-size: 14px;
}
QMainWindow, QMainWindow#MainWindowRoot, QWidget#AppRoot, QScrollArea#PageScrollArea {
    background: #f4f7fb;
    color: #202124;
}
QMainWindow#MainWindowRoot {
    border: 1px solid rgba(32, 33, 36, 0.10);
}
QScrollArea#PageScrollArea QWidget#qt_scrollarea_viewport, QWidget#PageContent {
    background: #f4f7fb;
    color: #202124;
}
QWidget#PageActionBar {
    background: #f4f7fb;
    border-top: 1px solid rgba(32, 33, 36, 0.08);
}
QWidget#PageHeader {
    background: rgba(255, 255, 255, 0.62);
    border: 1px solid rgba(32, 33, 36, 0.06);
    border-radius: 16px;
}
QMenuBar {
    background: #f4f7fb;
    color: #202124;
    padding: 4px;
    border: none;
    border-bottom: 1px solid rgba(32, 33, 36, 0.08);
}
QMenu {
    background: #ffffff;
    color: #202124;
    border: 1px solid rgba(32, 33, 36, 0.10);
    border-radius: 10px;
    padding: 6px;
}
QMenu::item {
    padding: 7px 22px;
    border-radius: 8px;
}
QMenuBar::item:selected, QMenu::item:selected {
    background: rgba(0, 102, 204, 0.10);
    border-radius: 8px;
}
QStatusBar {
    background: #f4f7fb;
    color: #667085;
    border-top: 1px solid rgba(32, 33, 36, 0.08);
}
QStatusBar QLabel {
    color: #667085;
    background: transparent;
}
QWidget#HeroBanner[theme="light"] {
    border-radius: 16px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                stop:0 #ffffff,
                                stop:0.45 #eaf3ff,
                                stop:1 #d9e8ff);
    border: 1px solid rgba(0, 102, 204, 0.14);
}
QWidget#HeroBanner[theme="dark"] {
    border-radius: 16px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                stop:0 #172033,
                                stop:0.45 #111827,
                                stop:1 #243b68);
    border: 1px solid rgba(255, 255, 255, 0.10);
}
QLabel#HeroTitle {
    color: #111827;
    font-size: 34px;
    font-weight: 700;
}
QLabel#HeroSubtitle {
    color: #475467;
    font-size: 15px;
}
QLabel#SectionTitle {
    color: #111827;
    font-size: 24px;
    font-weight: 700;
}
QLabel#SectionSubtitle, QLabel#CardSubtitle, QLabel#MetaText {
    color: #667085;
}
QWidget#NavigationWidget {
    background: rgba(255, 255, 255, 0.78);
    border: 1px solid rgba(32, 33, 36, 0.08);
    border-radius: 16px;
}
QFrame#NavSelectionPill {
    background: rgba(0, 102, 204, 0.16);
    border-radius: 12px;
}
QPushButton#NavButton {
    background: transparent;
    border: none;
    border-radius: 12px;
    color: #344054;
    padding: 9px 14px;
    text-align: left;
    font-weight: 600;
}
QPushButton#NavButton:hover {
    background: rgba(0, 102, 204, 0.08);
    color: #0b5cad;
}
QPushButton#NavButton:checked {
    background: transparent;
    color: #005fb8;
}
QWidget#CardWidget, QWidget#PreviewSurface {
    background: rgba(255, 255, 255, 0.88);
    border: 1px solid rgba(32, 33, 36, 0.08);
    border-radius: 16px;
}
QWidget#CardHeader {
    background: rgba(246, 250, 255, 0.92);
    border-top-left-radius: 16px;
    border-top-right-radius: 16px;
    border-bottom: 1px solid rgba(32, 33, 36, 0.07);
}
QWidget#CardBody {
    background: rgba(255, 255, 255, 0.64);
    border-bottom-left-radius: 16px;
    border-bottom-right-radius: 16px;
}
QFrame#SettingSeparator {
    background: rgba(32, 33, 36, 0.08);
    border: none;
}
QLabel#CardTitle {
    color: #111827;
    font-size: 17px;
    font-weight: 700;
}
QLabel#SettingLabel {
    color: #344054;
    font-weight: 600;
}
QLabel#ValuePill {
    background: rgba(0, 102, 204, 0.10);
    color: #005fb8;
    border-radius: 10px;
    padding: 4px 9px;
    font-weight: 700;
}
QPushButton {
    background: #ffffff;
    color: #202124;
    border: 1px solid rgba(32, 33, 36, 0.12);
    border-radius: 12px;
    padding: 9px 16px;
    font-weight: 600;
}
QPushButton:hover {
    background: #f1f6ff;
    border-color: rgba(0, 102, 204, 0.28);
}
QPushButton:pressed {
    background: #e6f0ff;
}
QPushButton:disabled {
    color: #98a2b3;
    background: #eef2f6;
    border-color: rgba(32, 33, 36, 0.08);
}
QPushButton#PrimaryButton {
    background: #0066cc;
    color: white;
    border-color: #0066cc;
}
QPushButton#PrimaryButton:hover {
    background: #0b74de;
}
QPushButton#DangerButton {
    color: #b42318;
    border-color: rgba(180, 35, 24, 0.22);
}
QPushButton#DangerButton:disabled {
    color: #c7cdd7;
    background: #eef2f6;
    border-color: rgba(32, 33, 36, 0.06);
}
QComboBox, QSpinBox, QLineEdit {
    background: #ffffff;
    color: #202124;
    border: 1px solid rgba(32, 33, 36, 0.14);
    border-radius: 10px;
    padding: 7px 12px;
    min-height: 24px;
}
QComboBox {
    padding-right: 34px;
}
QComboBox::drop-down {
    width: 30px;
    border: none;
    border-top-right-radius: 10px;
    border-bottom-right-radius: 10px;
    background: transparent;
    subcontrol-origin: padding;
    subcontrol-position: top right;
}
QComboBox::down-arrow {
    image: none;
    width: 10px;
    height: 6px;
    margin-right: 9px;
}
QComboBox QAbstractItemView {
    background: #ffffff;
    color: #202124;
    border: 1px solid rgba(32, 33, 36, 0.10);
    border-radius: 10px;
    padding: 6px;
    selection-background-color: rgba(0, 102, 204, 0.12);
    selection-color: #005fb8;
    outline: 0;
}
QSpinBox {
    padding-right: 28px;
}
QSpinBox::up-button, QSpinBox::down-button {
    width: 24px;
    border: none;
    background: transparent;
    subcontrol-origin: border;
}
QSpinBox::up-button {
    subcontrol-position: top right;
    border-top-right-radius: 10px;
}
QSpinBox::down-button {
    subcontrol-position: bottom right;
    border-bottom-right-radius: 10px;
}
QSpinBox::up-arrow {
    image: url(assets/icons/spin_up_light.svg);
    width: 8px;
    height: 5px;
}
QSpinBox::down-arrow {
    image: url(assets/icons/spin_down_light.svg);
    width: 8px;
    height: 5px;
}
QComboBox:hover, QSpinBox:hover, QLineEdit:hover {
    border-color: rgba(0, 102, 204, 0.40);
}
QComboBox:focus, QSpinBox:focus, QLineEdit:focus {
    border-color: #0066cc;
}
QListWidget {
    background: transparent;
    border: none;
    outline: 0;
}
QListWidget::item {
    padding: 12px 14px;
    border-radius: 12px;
    margin: 3px 0;
}
QListWidget::item:hover {
    background: rgba(0, 102, 204, 0.08);
}
QListWidget::item:selected {
    background: rgba(0, 102, 204, 0.16);
    color: #005fb8;
}
QSlider::groove:horizontal {
    min-height: 24px;
    height: 5px;
    background: #d9e2ef;
    border-radius: 3px;
}
QSlider::sub-page:horizontal {
    background: #0066cc;
    border-radius: 3px;
}
QSlider::handle:horizontal {
    width: 13px;
    height: 13px;
    margin: -5px 0;
    border-radius: 8px;
    background: #f8fafc;
    border: 2px solid #005fb8;
}
QSlider::handle:horizontal:hover {
    background: #ffffff;
    border: 2px solid #003f7d;
}
QScrollBar:vertical {
    background: transparent;
    width: 12px;
    border: none;
}
QScrollBar::handle:vertical {
    background: rgba(52, 64, 84, 0.30);
    border-radius: 6px;
    min-height: 34px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
    background: transparent;
    border: none;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
}
)qss");
}

QString ThemeManager::darkStyleSheet() const {
    return QStringLiteral(R"qss(
* {
    font-family: "Segoe UI", "Microsoft YaHei UI", "Microsoft YaHei";
    font-size: 14px;
}
QMainWindow, QMainWindow#MainWindowRoot, QWidget#AppRoot, QScrollArea#PageScrollArea {
    background: #0f1117;
    color: #edf2f7;
}
QMainWindow#MainWindowRoot {
    border: 1px solid rgba(255, 255, 255, 0.08);
}
QScrollArea#PageScrollArea QWidget#qt_scrollarea_viewport, QWidget#PageContent {
    background: #0f1117;
    color: #edf2f7;
}
QWidget#PageActionBar {
    background: #0f1117;
    border-top: 1px solid rgba(255, 255, 255, 0.07);
}
QWidget#PageHeader {
    background: rgba(24, 28, 38, 0.62);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 16px;
}
QMenuBar {
    background: #0f1117;
    color: #d8dee9;
    padding: 4px;
    border: none;
    border-bottom: 1px solid rgba(255, 255, 255, 0.07);
}
QMenu {
    background: #171c27;
    color: #edf2f7;
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 10px;
    padding: 6px;
}
QMenu::item {
    padding: 7px 22px;
    border-radius: 8px;
}
QMenuBar::item:selected, QMenu::item:selected {
    background: rgba(96, 165, 250, 0.14);
    border-radius: 8px;
}
QStatusBar {
    background: #0f1117;
    color: #9aa4b2;
    border-top: 1px solid rgba(255, 255, 255, 0.07);
}
QStatusBar QLabel {
    color: #9aa4b2;
    background: transparent;
}
QWidget#HeroBanner[theme="light"] {
    border-radius: 16px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                stop:0 #ffffff,
                                stop:0.45 #eaf3ff,
                                stop:1 #d9e8ff);
    border: 1px solid rgba(0, 102, 204, 0.14);
}
QWidget#HeroBanner[theme="dark"] {
    border-radius: 16px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                stop:0 #1b2335,
                                stop:0.42 #131720,
                                stop:1 #203a67);
    border: 1px solid rgba(255, 255, 255, 0.10);
}
QLabel#HeroTitle {
    color: #f8fafc;
    font-size: 34px;
    font-weight: 700;
}
QLabel#HeroSubtitle {
    color: #b7c3d5;
    font-size: 15px;
}
QLabel#SectionTitle {
    color: #f8fafc;
    font-size: 24px;
    font-weight: 700;
}
QLabel#SectionSubtitle, QLabel#CardSubtitle, QLabel#MetaText {
    color: #9aa4b2;
}
QWidget#NavigationWidget {
    background: rgba(24, 28, 38, 0.82);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 16px;
}
QFrame#NavSelectionPill {
    background: rgba(96, 165, 250, 0.18);
    border-radius: 12px;
}
QPushButton#NavButton {
    background: transparent;
    border: none;
    border-radius: 12px;
    color: #cbd5e1;
    padding: 9px 14px;
    text-align: left;
    font-weight: 600;
}
QPushButton#NavButton:hover {
    background: rgba(96, 165, 250, 0.10);
    color: #f8fafc;
}
QPushButton#NavButton:checked {
    background: transparent;
    color: #93c5fd;
}
QWidget#CardWidget, QWidget#PreviewSurface {
    background: rgba(24, 28, 38, 0.88);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 16px;
}
QWidget#CardHeader {
    background: rgba(31, 38, 52, 0.92);
    border-top-left-radius: 16px;
    border-top-right-radius: 16px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.07);
}
QWidget#CardBody {
    background: rgba(17, 24, 39, 0.48);
    border-bottom-left-radius: 16px;
    border-bottom-right-radius: 16px;
}
QFrame#SettingSeparator {
    background: rgba(255, 255, 255, 0.08);
    border: none;
}
QLabel#CardTitle {
    color: #f8fafc;
    font-size: 17px;
    font-weight: 700;
}
QLabel#SettingLabel {
    color: #d8dee9;
    font-weight: 600;
}
QLabel#ValuePill {
    background: rgba(96, 165, 250, 0.14);
    color: #93c5fd;
    border-radius: 10px;
    padding: 4px 9px;
    font-weight: 700;
}
QPushButton {
    background: #1d2330;
    color: #edf2f7;
    border: 1px solid rgba(255, 255, 255, 0.09);
    border-radius: 12px;
    padding: 9px 16px;
    font-weight: 600;
}
QPushButton:hover {
    background: #252d3d;
    border-color: rgba(96, 165, 250, 0.34);
}
QPushButton:pressed {
    background: #2e3748;
}
QPushButton:disabled {
    color: #687283;
    background: #161a23;
    border-color: rgba(255, 255, 255, 0.05);
}
QPushButton#PrimaryButton {
    background: #2563eb;
    color: white;
    border-color: #2563eb;
}
QPushButton#PrimaryButton:hover {
    background: #2f6ff2;
}
QPushButton#DangerButton {
    color: #fca5a5;
    border-color: rgba(248, 113, 113, 0.22);
}
QPushButton#DangerButton:disabled {
    color: #596274;
    background: #151922;
    border-color: rgba(255, 255, 255, 0.05);
}
QComboBox, QSpinBox, QLineEdit {
    background: #111827;
    color: #edf2f7;
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 10px;
    padding: 7px 12px;
    min-height: 24px;
}
QComboBox {
    padding-right: 34px;
}
QComboBox::drop-down {
    width: 30px;
    border: none;
    border-top-right-radius: 10px;
    border-bottom-right-radius: 10px;
    background: transparent;
    subcontrol-origin: padding;
    subcontrol-position: top right;
}
QComboBox::down-arrow {
    image: none;
    width: 10px;
    height: 6px;
    margin-right: 9px;
}
QComboBox QAbstractItemView {
    background: #171c27;
    color: #edf2f7;
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 10px;
    padding: 6px;
    selection-background-color: rgba(96, 165, 250, 0.16);
    selection-color: #93c5fd;
    outline: 0;
}
QSpinBox {
    padding-right: 28px;
}
QSpinBox::up-button, QSpinBox::down-button {
    width: 24px;
    border: none;
    background: transparent;
    subcontrol-origin: border;
}
QSpinBox::up-button {
    subcontrol-position: top right;
    border-top-right-radius: 10px;
}
QSpinBox::down-button {
    subcontrol-position: bottom right;
    border-bottom-right-radius: 10px;
}
QSpinBox::up-arrow {
    image: url(assets/icons/spin_up_dark.svg);
    width: 8px;
    height: 5px;
}
QSpinBox::down-arrow {
    image: url(assets/icons/spin_down_dark.svg);
    width: 8px;
    height: 5px;
}
QComboBox:hover, QSpinBox:hover, QLineEdit:hover {
    border-color: rgba(96, 165, 250, 0.42);
}
QComboBox:focus, QSpinBox:focus, QLineEdit:focus {
    border-color: #60a5fa;
}
QListWidget {
    background: transparent;
    border: none;
    outline: 0;
}
QListWidget::item {
    padding: 12px 14px;
    border-radius: 12px;
    margin: 3px 0;
}
QListWidget::item:hover {
    background: rgba(96, 165, 250, 0.10);
}
QListWidget::item:selected {
    background: rgba(96, 165, 250, 0.18);
    color: #93c5fd;
}
QSlider::groove:horizontal {
    min-height: 24px;
    height: 5px;
    background: #2b3344;
    border-radius: 3px;
}
QSlider::sub-page:horizontal {
    background: #60a5fa;
    border-radius: 3px;
}
QSlider::handle:horizontal {
    width: 13px;
    height: 13px;
    margin: -5px 0;
    border-radius: 8px;
    background: #111827;
    border: 2px solid #93c5fd;
}
QSlider::handle:horizontal:hover {
    background: #0b1120;
    border: 2px solid #dbeafe;
}
QScrollBar:vertical {
    background: transparent;
    width: 12px;
    border: none;
}
QScrollBar::handle:vertical {
    background: rgba(203, 213, 225, 0.30);
    border-radius: 6px;
    min-height: 34px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
    background: transparent;
    border: none;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
}
)qss");
}
