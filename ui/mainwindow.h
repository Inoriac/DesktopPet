//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_MAINWINDOW_H
#define DESKTOP_PET_MAINWINDOW_H

#include <QHash>
#include <QMainWindow>
#include <QString>

#include <string>

#include "ai_types.h"
#include "petwindow.h"

QT_BEGIN_NAMESPACE
class QAction;
class QComboBox;
class QLabel;
class QMenu;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QVBoxLayout;
class QWidget;
QT_END_NAMESPACE

class NavigationWidget;
class SwitchButton;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString& autoStartPet = QString(), QWidget *parent = nullptr);
    ~MainWindow() override;

    // 指定角色启动后自动开宠（供 launcher 经 --pet 调用）。角色无效则静默忽略。
    void autoStartPet();

private slots:
    void OnPetSelected();
    void OnStartPet();
    void OnStopPet();
    void OnSettingsChanged();
    void OnBubbleAppearanceChanged();
    void OnAbout();
    void OnAddPet();

private:
    void createActions();
    void createMenus();
    void createStatusBar();
    void createCentralWidget();
    void setupConnections();
    void loadPetList();
    void setWindowFlagControlsLocked(bool locked);

    QWidget* createHeroBanner();
    QWidget* createPetPage();
    QWidget* createAiPage();
    QWidget* createVoicePage();
    QWidget* createBubblePage();
    QWidget* createAdvancedPage();

    void navigateToPage(const QString& pageId);
    void toggleThemeWithTransition();
    void updateCharacterPreview();
    void updateThemeButtonText();
    void updateRunButtonState();
    QString currentPetName() const;

    // UI shell
    QWidget *centralWidget{};
    QWidget *heroBanner{};
    QVBoxLayout *mainLayout{};
    NavigationWidget *navigationWidget{};
    QStackedWidget *pageStack{};
    QHash<QString, int> pageIndexById;

    // Pet page
    QComboBox *characterComboBox{};
    QLabel *characterPreviewTitle{};
    QLabel *characterPreviewMeta{};
    QPushButton *runPetButton{};
    QLabel *sizeValueLabel{};
    QSlider *sizeSlider{};
    QSpinBox *sizeSpinBox{};
    SwitchButton *alwaysOnTopCheckBox{};
    SwitchButton *clickThroughCheckBox{};

    // AI page
    SwitchButton *aiEnabledCheckBox{};
    SwitchButton *autoScreenChatCheckBox{};
    QSpinBox *chatIntervalSpinBox{};

    // Voice page
    SwitchButton *soundEnabledCheckBox{};
    SwitchButton *voiceEnabledCheckBox{};
    QComboBox *voiceSpeakerComboBox{};
    QSlider *volumeSlider{};
    QLabel *volumeValueLabel{};

    // Bubble page
    QSlider *bubbleOpacitySlider{};
    QSpinBox *bubbleOpacitySpinBox{};
    QSpinBox *bubbleFontSizeSpinBox{};
    QSpinBox *bubbleOffsetXSpinBox{};
    QSpinBox *bubbleOffsetYSpinBox{};

    // Advanced page
    QPushButton *themeToggleButton{};

    // PetWindow
    PetWindow *activePetWindow {nullptr};
    QString activePetName;
    QString m_autoStartPetName;  // --pet 传入的启动角色（若有效则构造后自动开宠）

    // Menus and actions
    QMenu *fileMenu{};
    QMenu *settingsMenu{};
    QMenu *helpMenu{};
    QAction *exitAction{};
    QAction *preferencesAction{};
    QAction *aboutAction{};

    // Status bar
    QLabel *statusLabel{};

    std::string const modelBasePath = "/assets/models/";

    ScreenChatConfig buildScreenChatConfigFromUi() const;
    VoiceConfig buildVoiceConfigFromUi() const;
};

#endif // DESKTOP_PET_MAINWINDOW_H
