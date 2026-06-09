//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_MAINWINDOW_H
#define DESKTOP_PET_MAINWINDOW_H

#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QGroupBox>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QString>
#include "ai_types.h"

#include "petwindow.h"

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class RenderViewport;

class MainWindow : public QMainWindow{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

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

    // UI组件
    QWidget *centralWidget{};
    QVBoxLayout *mainLayout{};

    // 宠物选择区域
    QGroupBox *characterSelectionGroup{};
    QListWidget *petListWidget{};
    QPushButton *startPetButton{};
    QPushButton *stopPetButton{};

    // PetWindow
    PetWindow *activePetWindow {nullptr};
    QString activePetName;

    // 设置区域
    QGroupBox *settingsGroup{};
    QLabel *sizeLabel{};
    QSlider *sizeSlider{};
    QSpinBox *sizeSpinBox{};
    QCheckBox *alwaysOnTopCheckBox{};
    QCheckBox *clickThroughCheckBox{};
    QCheckBox *aiEnabledCheckBox{};
    QCheckBox *soundEnabledCheckBox{};
    QCheckBox *voiceEnabledCheckBox{};
    QComboBox *voiceSpeakerComboBox{};
    QSlider *volumeSlider{};
    QLabel *volumeLabel{};

    QCheckBox *autoScreenChatCheckBox{};
    QSlider *bubbleOpacitySlider{};
    QSpinBox *bubbleOpacitySpinBox{};
    QSpinBox *bubbleFontSizeSpinBox{};
    QSpinBox *bubbleOffsetXSpinBox{};
    QSpinBox *bubbleOffsetYSpinBox{};

    // 菜单和动作
    QMenu *fileMenu{};
    QMenu *settingsMenu{};
    QMenu *helpMenu{};
    QAction *exitAction{};
    QAction *preferencesAction{};
    QAction *aboutAction{};

    // 状态栏
    QLabel *statusLabel{};

    std::string const modelBasePath = "/assets/models/";

    RenderViewport *renderViewport{};

    ScreenChatConfig buildScreenChatConfigFromUi() const;
    VoiceConfig buildVoiceConfigFromUi() const;
};


#endif //DESKTOP_PET_MAINWINDOW_H