//
// Created by Inoriac on 2025/10/15.
//

#include "mainwindow.h"

#include "card_widget.h"
#include "configLoader/config_manager.h"
#include "navigation_widget.h"
#include "pet.h"
#include "statistic_manager.h"
#include "theme_manager.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>

namespace {
QLabel* createMetaLabel(const QString& text, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("MetaText"));
    label->setWordWrap(true);
    return label;
}

QWidget* createPageShell(const QString& title, const QString& subtitle, QVBoxLayout** contentLayoutOut) {
    auto* page = new QWidget;
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    auto* scrollArea = new QScrollArea(page);
    scrollArea->setObjectName(QStringLiteral("PageScrollArea"));
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scrollArea);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(4, 2, 16, 18);
    contentLayout->setSpacing(18);

    auto* headerTitle = new QLabel(title, content);
    headerTitle->setObjectName(QStringLiteral("SectionTitle"));
    contentLayout->addWidget(headerTitle);

    auto* headerSubtitle = createMetaLabel(subtitle, content);
    headerSubtitle->setObjectName(QStringLiteral("SectionSubtitle"));
    contentLayout->addWidget(headerSubtitle);

    scrollArea->setWidget(content);
    pageLayout->addWidget(scrollArea);

    if (contentLayoutOut) {
        *contentLayoutOut = contentLayout;
    }
    return page;
}

QWidget* createSettingRow(const QString& title,
                          const QString& subtitle,
                          QWidget* control,
                          QWidget* trailing = nullptr) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    auto* textColumn = new QWidget(row);
    auto* textLayout = new QVBoxLayout(textColumn);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(3);

    auto* titleLabel = new QLabel(title, textColumn);
    titleLabel->setObjectName(QStringLiteral("SettingLabel"));
    textLayout->addWidget(titleLabel);

    if (!subtitle.trimmed().isEmpty()) {
        textLayout->addWidget(createMetaLabel(subtitle, textColumn));
    }

    layout->addWidget(textColumn, 1);
    if (control) {
        layout->addWidget(control, 0, Qt::AlignVCenter);
    }
    if (trailing) {
        layout->addWidget(trailing, 0, Qt::AlignVCenter);
    }
    return row;
}

QLabel* createValuePill(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("ValuePill"));
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumWidth(56);
    return label;
}

QWidget* createHorizontalControls(std::initializer_list<QWidget*> widgets) {
    auto* container = new QWidget;
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    for (QWidget* widget : widgets) {
        if (widget) {
            layout->addWidget(widget);
        }
    }
    return container;
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Desktop 3D Pet"));
    setMinimumSize(980, 720);
    resize(1120, 760);

    createActions();
    createMenus();
    createStatusBar();
    createCentralWidget();
    setupConnections();

    Pet::instance().load();
    loadPetList();

    setWindowIcon(QIcon(QStringLiteral("assets/icons/icon.png")));
}

MainWindow::~MainWindow() {
    if (!activePetName.isEmpty()) {
        StatisticManager::getInstance().recordPetStop(activePetName);
    }

    if (activePetWindow) {
        activePetWindow->close();
        delete activePetWindow;
        activePetWindow = nullptr;
    }
}

void MainWindow::createActions() {
    exitAction = new QAction(QStringLiteral("退出"), this);
    exitAction->setShortcut(QKeySequence::Quit);
    exitAction->setStatusTip(QStringLiteral("退出应用程序"));
    connect(exitAction, &QAction::triggered, this, &MainWindow::close);

    preferencesAction = new QAction(QStringLiteral("偏好设置"), this);
    preferencesAction->setStatusTip(QStringLiteral("打开偏好设置"));
    connect(preferencesAction, &QAction::triggered, this, [this]() {
        navigateToPage(QStringLiteral("advanced"));
    });

    aboutAction = new QAction(QStringLiteral("关于"), this);
    aboutAction->setStatusTip(QStringLiteral("关于 Desktop 3D Pet"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::OnAbout);
}

void MainWindow::createMenus() {
    fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    fileMenu->addAction(exitAction);

    settingsMenu = menuBar()->addMenu(tr("设置(&S)"));
    settingsMenu->addAction(preferencesAction);

    helpMenu = menuBar()->addMenu(tr("帮助(&H)"));
    helpMenu->addAction(aboutAction);
}

void MainWindow::createStatusBar() {
    statusLabel = new QLabel(QStringLiteral("就绪"), this);
    statusBar()->addWidget(statusLabel);
}

void MainWindow::createCentralWidget() {
    centralWidget = new QWidget(this);
    centralWidget->setObjectName(QStringLiteral("AppRoot"));
    setCentralWidget(centralWidget);

    mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(24, 24, 24, 20);
    mainLayout->setSpacing(22);

    heroBanner = createHeroBanner();
    mainLayout->addWidget(heroBanner);

    auto* bodyLayout = new QHBoxLayout;
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(18);

    navigationWidget = new NavigationWidget(centralWidget);
    navigationWidget->addItem(QStringLiteral("pet"), QStringLiteral("桌宠"));
    navigationWidget->addItem(QStringLiteral("ai"), QStringLiteral("AI 助手"));
    navigationWidget->addItem(QStringLiteral("voice"), QStringLiteral("语音"));
    navigationWidget->addItem(QStringLiteral("bubble"), QStringLiteral("气泡"));
    navigationWidget->addItem(QStringLiteral("advanced"), QStringLiteral("高级"));

    pageStack = new QStackedWidget(centralWidget);
    pageStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    pageIndexById.insert(QStringLiteral("pet"), pageStack->addWidget(createPetPage()));
    pageIndexById.insert(QStringLiteral("ai"), pageStack->addWidget(createAiPage()));
    pageIndexById.insert(QStringLiteral("voice"), pageStack->addWidget(createVoicePage()));
    pageIndexById.insert(QStringLiteral("bubble"), pageStack->addWidget(createBubblePage()));
    pageIndexById.insert(QStringLiteral("advanced"), pageStack->addWidget(createAdvancedPage()));

    bodyLayout->addWidget(navigationWidget);
    bodyLayout->addWidget(pageStack, 1);
    mainLayout->addLayout(bodyLayout, 1);

    navigateToPage(QStringLiteral("pet"));
}

QWidget* MainWindow::createHeroBanner() {
    auto* hero = new QWidget(centralWidget);
    hero->setObjectName(QStringLiteral("HeroBanner"));
    hero->setAttribute(Qt::WA_StyledBackground, true);
    hero->setMinimumHeight(190);
    hero->setMaximumHeight(210);

    auto* shadow = new QGraphicsDropShadowEffect(hero);
    shadow->setBlurRadius(34.0);
    shadow->setOffset(0, 12);
    shadow->setColor(QColor(0, 0, 0, 34));
    hero->setGraphicsEffect(shadow);

    auto* layout = new QHBoxLayout(hero);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(18);

    auto* textColumn = new QWidget(hero);
    auto* textLayout = new QVBoxLayout(textColumn);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("Desktop Pet Control Center"), textColumn);
    title->setObjectName(QStringLiteral("HeroTitle"));
    title->setWordWrap(true);
    textLayout->addWidget(title);

    auto* subtitle = new QLabel(QStringLiteral("管理桌宠角色、AI 回复、语音播报与聊天气泡。界面采用 Fluent Dashboard 风格，功能逻辑保持不变。"), textColumn);
    subtitle->setObjectName(QStringLiteral("HeroSubtitle"));
    subtitle->setWordWrap(true);
    textLayout->addWidget(subtitle);
    textLayout->addStretch();

    auto* quickTip = createValuePill(QStringLiteral("Qt6 Widgets"));
    quickTip->setMinimumWidth(110);
    textLayout->addWidget(quickTip, 0, Qt::AlignLeft);

    layout->addWidget(textColumn, 1);

    auto* rightPanel = new QWidget(hero);
    rightPanel->setObjectName(QStringLiteral("PreviewSurface"));
    rightPanel->setAttribute(Qt::WA_StyledBackground, true);
    rightPanel->setMinimumWidth(230);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(20, 18, 20, 18);
    rightLayout->setSpacing(8);

    auto* statusTitle = new QLabel(QStringLiteral("Companion Ready"), rightPanel);
    statusTitle->setObjectName(QStringLiteral("CardTitle"));
    rightLayout->addWidget(statusTitle);
    rightLayout->addWidget(createMetaLabel(QStringLiteral("选择角色后点击启动，设置会实时同步到运行中的桌宠。"), rightPanel));
    rightLayout->addStretch();

    layout->addWidget(rightPanel, 0, Qt::AlignRight | Qt::AlignVCenter);

    ThemeManager::instance().applyHeroPalette(hero);
    return hero;
}

QWidget* MainWindow::createPetPage() {
    QVBoxLayout* contentLayout = nullptr;
    QWidget* page = createPageShell(QStringLiteral("桌宠"),
                                    QStringLiteral("选择角色、启动桌宠，并配置窗口行为。"),
                                    &contentLayout);

    auto* characterCard = new CardWidget(QStringLiteral("角色选择"),
                                         QStringLiteral("从已登记模型中选择一个桌宠角色。"),
                                         page);
    characterComboBox = new QComboBox(characterCard);
    characterComboBox->setMinimumWidth(240);
    characterCard->addWidget(createSettingRow(QStringLiteral("当前角色"),
                                              QStringLiteral("角色来自 pet_info.json，可通过高级页添加新模型。"),
                                              characterComboBox));

    auto* previewSurface = new QWidget(characterCard);
    previewSurface->setObjectName(QStringLiteral("PreviewSurface"));
    previewSurface->setAttribute(Qt::WA_StyledBackground, true);
    auto* previewLayout = new QVBoxLayout(previewSurface);
    previewLayout->setContentsMargins(18, 16, 18, 16);
    previewLayout->setSpacing(8);

    characterPreviewTitle = new QLabel(QStringLiteral("未选择角色"), previewSurface);
    characterPreviewTitle->setObjectName(QStringLiteral("CardTitle"));
    previewLayout->addWidget(characterPreviewTitle);

    characterPreviewMeta = createMetaLabel(QStringLiteral("加载角色列表后会显示模型路径。"), previewSurface);
    previewLayout->addWidget(characterPreviewMeta);
    characterCard->addWidget(previewSurface);
    contentLayout->addWidget(characterCard);

    auto* runtimeCard = new CardWidget(QStringLiteral("运行控制"),
                                       QStringLiteral("启动或停止桌宠窗口。"),
                                       page);
    startPetButton = new QPushButton(QStringLiteral("启动桌宠"), runtimeCard);
    startPetButton->setObjectName(QStringLiteral("PrimaryButton"));
    stopPetButton = new QPushButton(QStringLiteral("停止桌宠"), runtimeCard);
    stopPetButton->setObjectName(QStringLiteral("DangerButton"));
    stopPetButton->setEnabled(false);
    runtimeCard->addWidget(createSettingRow(QStringLiteral("桌宠进程"),
                                            QStringLiteral("启动后可继续调整 AI、语音和气泡设置。"),
                                            createHorizontalControls({startPetButton, stopPetButton})));
    contentLayout->addWidget(runtimeCard);

    auto* displayCard = new CardWidget(QStringLiteral("显示与窗口行为"),
                                       QStringLiteral("大小可以实时调整；置顶与穿透在桌宠运行时会锁定。"),
                                       page);
    sizeSlider = new QSlider(Qt::Horizontal, displayCard);
    sizeSlider->setRange(50, 200);
    sizeSlider->setValue(100);
    sizeSlider->setMinimumWidth(220);
    sizeSpinBox = new QSpinBox(displayCard);
    sizeSpinBox->setRange(50, 200);
    sizeSpinBox->setValue(100);
    sizeSpinBox->setSuffix(QStringLiteral("%"));
    sizeValueLabel = createValuePill(QStringLiteral("100%"));
    displayCard->addWidget(createSettingRow(QStringLiteral("角色大小"),
                                            QStringLiteral("控制桌宠渲染窗口尺寸比例。"),
                                            createHorizontalControls({sizeSlider, sizeSpinBox}),
                                            sizeValueLabel));

    alwaysOnTopCheckBox = new QCheckBox(QStringLiteral("启用"), displayCard);
    alwaysOnTopCheckBox->setChecked(true);
    displayCard->addWidget(createSettingRow(QStringLiteral("窗口置顶"),
                                            QStringLiteral("让桌宠保持在其他窗口上方。"),
                                            alwaysOnTopCheckBox));

    clickThroughCheckBox = new QCheckBox(QStringLiteral("启用"), displayCard);
    clickThroughCheckBox->setChecked(false);
    displayCard->addWidget(createSettingRow(QStringLiteral("鼠标穿透"),
                                            QStringLiteral("启用后鼠标事件会穿过桌宠窗口。"),
                                            clickThroughCheckBox));
    contentLayout->addWidget(displayCard);
    contentLayout->addStretch();
    return page;
}

QWidget* MainWindow::createAiPage() {
    QVBoxLayout* contentLayout = nullptr;
    QWidget* page = createPageShell(QStringLiteral("AI 助手"),
                                    QStringLiteral("控制 AI 回复与自动屏幕观察对话。"),
                                    &contentLayout);

    const auto& llmConfig = ConfigManager::instance().getLlmConfig();
    const ScreenChatConfig& screenChat = ConfigManager::instance().getScreenChatConfig();
    const int defaultIntervalMinutes = std::clamp(((screenChat.minIntervalMs + screenChat.maxIntervalMs) / 2) / (60 * 1000), 1, 120);

    auto* aiCard = new CardWidget(QStringLiteral("智能行为"),
                                  QStringLiteral("这些开关会同步到已运行的桌宠。"),
                                  page);
    aiEnabledCheckBox = new QCheckBox(QStringLiteral("启用"), aiCard);
    aiEnabledCheckBox->setChecked(llmConfig.enabled);
    aiCard->addWidget(createSettingRow(QStringLiteral("AI 回复"),
                                       QStringLiteral("允许桌宠调用配置中的大语言模型生成回复。"),
                                       aiEnabledCheckBox));

    autoScreenChatCheckBox = new QCheckBox(QStringLiteral("启用"), aiCard);
    autoScreenChatCheckBox->setChecked(screenChat.enabled);
    aiCard->addWidget(createSettingRow(QStringLiteral("自动屏幕聊天"),
                                       QStringLiteral("桌宠会周期性观察屏幕并主动聊天。"),
                                       autoScreenChatCheckBox));

    chatIntervalSpinBox = new QSpinBox(aiCard);
    chatIntervalSpinBox->setRange(1, 120);
    chatIntervalSpinBox->setValue(defaultIntervalMinutes);
    chatIntervalSpinBox->setSuffix(QStringLiteral(" min"));
    aiCard->addWidget(createSettingRow(QStringLiteral("主动聊天间隔"),
                                       QStringLiteral("实际触发会在该值附近随机浮动，避免过于机械。"),
                                       chatIntervalSpinBox));
    contentLayout->addWidget(aiCard);

    auto* modelCard = new CardWidget(QStringLiteral("模型与提示词"),
                                     QStringLiteral("当前项目仍使用原有 JSON 配置；这里提供可视化入口占位，不改变配置来源。"),
                                     page);
    auto* modelComboBox = new QComboBox(modelCard);
    modelComboBox->addItem(QStringLiteral("OpenAI Compatible / JSON 配置"));
    modelComboBox->setEnabled(false);
    modelCard->addWidget(createSettingRow(QStringLiteral("模型选择"),
                                          QStringLiteral("请继续在 config/default_common_config.json 中管理模型参数。"),
                                          modelComboBox));

    auto* promptLineEdit = new QLineEdit(modelCard);
    promptLineEdit->setPlaceholderText(QStringLiteral("系统提示词入口预留"));
    promptLineEdit->setEnabled(false);
    modelCard->addWidget(createSettingRow(QStringLiteral("Prompt Settings"),
                                          QStringLiteral("预留扩展位，不影响现有 AI 运行逻辑。"),
                                          promptLineEdit));
    contentLayout->addWidget(modelCard);
    contentLayout->addStretch();
    return page;
}

QWidget* MainWindow::createVoicePage() {
    QVBoxLayout* contentLayout = nullptr;
    QWidget* page = createPageShell(QStringLiteral("语音"),
                                    QStringLiteral("配置音效与 GENIE / Python 语音合成。"),
                                    &contentLayout);

    const VoiceConfig& voice = ConfigManager::instance().getVoiceConfig();

    auto* voiceCard = new CardWidget(QStringLiteral("语音播报"),
                                     QStringLiteral("开启后，桌宠展示回复气泡的同时会把同一文本发送给语音合成。"),
                                     page);
    soundEnabledCheckBox = new QCheckBox(QStringLiteral("启用"), voiceCard);
    soundEnabledCheckBox->setChecked(false);
    voiceCard->addWidget(createSettingRow(QStringLiteral("音效"),
                                          QStringLiteral("保留原音效开关。"),
                                          soundEnabledCheckBox));

    voiceEnabledCheckBox = new QCheckBox(QStringLiteral("启用"), voiceCard);
    voiceEnabledCheckBox->setChecked(voice.enabled);
    voiceCard->addWidget(createSettingRow(QStringLiteral("语音合成"),
                                          QStringLiteral("可选启动 Python worker，不需要时不会预加载。"),
                                          voiceEnabledCheckBox));

    voiceSpeakerComboBox = new QComboBox(voiceCard);
    voiceSpeakerComboBox->addItem(QStringLiteral("Feibi / 菲比（中文）"), QStringLiteral("feibi"));
    voiceSpeakerComboBox->addItem(QStringLiteral("Mika / 聖園ミカ（日语）"), QStringLiteral("mika"));
    voiceSpeakerComboBox->addItem(QStringLiteral("ThirtySeven / 37（英语）"), QStringLiteral("thirtyseven"));
    voiceSpeakerComboBox->addItem(QStringLiteral("自定义角色（按配置加载）"), QStringLiteral("custom"));
    const QString selectedVoice = voice.speakerMode == QStringLiteral("custom")
        ? QStringLiteral("custom")
        : voice.selectedSpeaker;
    const int selectedVoiceIndex = voiceSpeakerComboBox->findData(selectedVoice);
    voiceSpeakerComboBox->setCurrentIndex(selectedVoiceIndex >= 0 ? selectedVoiceIndex : 0);
    voiceSpeakerComboBox->setEnabled(voice.enabled);
    voiceCard->addWidget(createSettingRow(QStringLiteral("说话人角色"),
                                          QStringLiteral("预设角色与自定义角色目录沿用现有语音配置。"),
                                          voiceSpeakerComboBox));

    volumeSlider = new QSlider(Qt::Horizontal, voiceCard);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(75);
    volumeSlider->setMinimumWidth(220);
    volumeValueLabel = createValuePill(QStringLiteral("75%"));
    voiceCard->addWidget(createSettingRow(QStringLiteral("音量"),
                                          QStringLiteral("保留原音量控件；当前不会改变 GENIE 输出文本内容。"),
                                          volumeSlider,
                                          volumeValueLabel));
    contentLayout->addWidget(voiceCard);
    contentLayout->addStretch();
    return page;
}

QWidget* MainWindow::createBubblePage() {
    QVBoxLayout* contentLayout = nullptr;
    QWidget* page = createPageShell(QStringLiteral("聊天气泡"),
                                    QStringLiteral("设置桌宠回复气泡的透明度、字号和位置偏移。"),
                                    &contentLayout);

    const ScreenChatConfig& screenChat = ConfigManager::instance().getScreenChatConfig();

    auto* bubbleCard = new CardWidget(QStringLiteral("气泡外观"),
                                      QStringLiteral("运行中修改会立即预览到桌宠气泡。"),
                                      page);
    bubbleOpacitySlider = new QSlider(Qt::Horizontal, bubbleCard);
    bubbleOpacitySlider->setRange(10, 100);
    bubbleOpacitySlider->setValue(screenChat.bubbleOpacityPercent);
    bubbleOpacitySlider->setMinimumWidth(220);
    bubbleOpacitySpinBox = new QSpinBox(bubbleCard);
    bubbleOpacitySpinBox->setRange(10, 100);
    bubbleOpacitySpinBox->setValue(screenChat.bubbleOpacityPercent);
    bubbleOpacitySpinBox->setSuffix(QStringLiteral("%"));
    bubbleCard->addWidget(createSettingRow(QStringLiteral("透明度"),
                                           QStringLiteral("控制气泡背景透明程度。"),
                                           createHorizontalControls({bubbleOpacitySlider, bubbleOpacitySpinBox})));

    bubbleFontSizeSpinBox = new QSpinBox(bubbleCard);
    bubbleFontSizeSpinBox->setRange(10, 36);
    bubbleFontSizeSpinBox->setValue(screenChat.bubbleFontSize);
    bubbleFontSizeSpinBox->setSuffix(QStringLiteral("px"));
    bubbleCard->addWidget(createSettingRow(QStringLiteral("字号"),
                                           QStringLiteral("控制输出气泡文本大小。"),
                                           bubbleFontSizeSpinBox));

    bubbleOffsetXSpinBox = new QSpinBox(bubbleCard);
    bubbleOffsetXSpinBox->setRange(-300, 300);
    bubbleOffsetXSpinBox->setValue(screenChat.bubbleOffsetX);
    bubbleOffsetYSpinBox = new QSpinBox(bubbleCard);
    bubbleOffsetYSpinBox->setRange(-300, 300);
    bubbleOffsetYSpinBox->setValue(screenChat.bubbleOffsetY);
    bubbleCard->addWidget(createSettingRow(QStringLiteral("位置偏移"),
                                           QStringLiteral("相对桌宠窗口调整气泡 X/Y 坐标。"),
                                           createHorizontalControls({bubbleOffsetXSpinBox, bubbleOffsetYSpinBox})));
    contentLayout->addWidget(bubbleCard);
    contentLayout->addStretch();
    return page;
}

QWidget* MainWindow::createAdvancedPage() {
    QVBoxLayout* contentLayout = nullptr;
    QWidget* page = createPageShell(QStringLiteral("高级"),
                                    QStringLiteral("主题、资源管理和关于信息。"),
                                    &contentLayout);

    auto* themeCard = new CardWidget(QStringLiteral("外观主题"),
                                     QStringLiteral("Light / Dark 主题由 ThemeManager 统一应用。"),
                                     page);
    themeToggleButton = new QPushButton(themeCard);
    updateThemeButtonText();
    themeCard->addWidget(createSettingRow(QStringLiteral("界面主题"),
                                          QStringLiteral("切换后会刷新全局 QSS 与 Hero Banner。"),
                                          themeToggleButton));
    contentLayout->addWidget(themeCard);

    auto* resourceCard = new CardWidget(QStringLiteral("资源与信息"),
                                        QStringLiteral("保留原添加桌宠与关于功能。"),
                                        page);
    auto* addPetButton = new QPushButton(QStringLiteral("添加桌宠"), resourceCard);
    addPetButton->setObjectName(QStringLiteral("PrimaryButton"));
    connect(addPetButton, &QPushButton::clicked, this, &MainWindow::OnAddPet);

    auto* aboutButton = new QPushButton(QStringLiteral("关于 Desktop Pet"), resourceCard);
    connect(aboutButton, &QPushButton::clicked, this, &MainWindow::OnAbout);

    resourceCard->addWidget(createSettingRow(QStringLiteral("模型库"),
                                             QStringLiteral("选择 GLTF / GLB 模型并登记为新桌宠。"),
                                             addPetButton));
    resourceCard->addWidget(createSettingRow(QStringLiteral("应用信息"),
                                             QStringLiteral("查看版本和开发信息。"),
                                             aboutButton));
    contentLayout->addWidget(resourceCard);
    contentLayout->addStretch();
    return page;
}

void MainWindow::setupConnections() {
    connect(navigationWidget, &NavigationWidget::navigationRequested, this, &MainWindow::navigateToPage);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        ThemeManager::instance().applyHeroPalette(heroBanner);
        updateThemeButtonText();
    });

    connect(characterComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::OnPetSelected);
    connect(startPetButton, &QPushButton::clicked, this, &MainWindow::OnStartPet);
    connect(stopPetButton, &QPushButton::clicked, this, &MainWindow::OnStopPet);

    connect(sizeSlider, &QSlider::valueChanged, sizeSpinBox, &QSpinBox::setValue);
    connect(sizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), sizeSlider, &QSlider::setValue);
    connect(sizeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (sizeValueLabel) {
            sizeValueLabel->setText(QStringLiteral("%1%").arg(value));
        }
        OnSettingsChanged();
    });

    connect(bubbleOpacitySlider, &QSlider::valueChanged, bubbleOpacitySpinBox, &QSpinBox::setValue);
    connect(bubbleOpacitySpinBox, QOverload<int>::of(&QSpinBox::valueChanged), bubbleOpacitySlider, &QSlider::setValue);

    connect(alwaysOnTopCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(clickThroughCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(aiEnabledCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(autoScreenChatCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(chatIntervalSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::OnSettingsChanged);

    connect(voiceEnabledCheckBox, &QCheckBox::toggled, this, [this](bool enabled) {
        if (voiceSpeakerComboBox) {
            voiceSpeakerComboBox->setEnabled(enabled);
        }
        OnSettingsChanged();
    });
    connect(voiceSpeakerComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::OnSettingsChanged);
    connect(soundEnabledCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (volumeValueLabel) {
            volumeValueLabel->setText(QStringLiteral("%1%").arg(value));
        }
        OnSettingsChanged();
    });

    connect(bubbleOpacitySlider, &QSlider::valueChanged, this, &MainWindow::OnBubbleAppearanceChanged);
    connect(bubbleFontSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::OnBubbleAppearanceChanged);
    connect(bubbleOffsetXSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::OnBubbleAppearanceChanged);
    connect(bubbleOffsetYSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::OnBubbleAppearanceChanged);

    connect(themeToggleButton, &QPushButton::clicked, this, []() {
        ThemeManager::instance().toggleTheme();
    });
}

void MainWindow::loadPetList() {
    if (!characterComboBox) {
        return;
    }

    const QSignalBlocker blocker(characterComboBox);
    characterComboBox->clear();

    const QStringList petNames = Pet::instance().getPetNames();
    for (const QString& name : petNames) {
        characterComboBox->addItem(name, name);
    }

    characterComboBox->setEnabled(characterComboBox->count() > 0);
    if (startPetButton) {
        startPetButton->setEnabled(characterComboBox->count() > 0 && !activePetWindow);
    }

    if (characterComboBox->count() > 0) {
        characterComboBox->setCurrentIndex(0);
    }
    updateCharacterPreview();
}

void MainWindow::navigateToPage(const QString& pageId) {
    if (!pageStack || !pageIndexById.contains(pageId)) {
        return;
    }
    pageStack->setCurrentIndex(pageIndexById.value(pageId));
    if (navigationWidget) {
        navigationWidget->setCurrentItem(pageId);
    }
}

QString MainWindow::currentPetName() const {
    if (!characterComboBox || characterComboBox->currentIndex() < 0) {
        return {};
    }
    const QString dataName = characterComboBox->currentData().toString();
    return dataName.isEmpty() ? characterComboBox->currentText() : dataName;
}

void MainWindow::updateCharacterPreview() {
    const QString petName = currentPetName();
    if (characterPreviewTitle) {
        characterPreviewTitle->setText(petName.isEmpty() ? QStringLiteral("未选择角色") : petName);
    }
    if (characterPreviewMeta) {
        const QString modelPath = petName.isEmpty() ? QString() : Pet::instance().getModelPath(petName);
        characterPreviewMeta->setText(modelPath.isEmpty()
            ? QStringLiteral("暂无可用模型路径。请在高级页添加桌宠模型。")
            : QStringLiteral("模型路径：%1").arg(modelPath));
    }
}

void MainWindow::updateThemeButtonText() {
    if (!themeToggleButton) {
        return;
    }
    themeToggleButton->setText(ThemeManager::instance().isDarkTheme()
        ? QStringLiteral("切换到 Light Theme")
        : QStringLiteral("切换到 Dark Theme"));
}

void MainWindow::OnPetSelected() {
    updateCharacterPreview();
    const QString petName = currentPetName();
    if (statusLabel) {
        statusLabel->setText(petName.isEmpty()
            ? QStringLiteral("未选择桌宠")
            : QStringLiteral("已选择：%1").arg(petName));
    }
}

void MainWindow::OnStartPet() {
    if (activePetWindow) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("已有宠物正在运行，请先停止当前宠物"));
        return;
    }

    const QString petName = currentPetName();
    if (petName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择一个桌宠!"));
        return;
    }

    qDebug() << "Selected pet:" << petName;

    const QString modelPath = Pet::instance().getModelPath(petName);
    qDebug() << "Model path:" << modelPath;

    if (modelPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("无法找到宠物模型"));
        return;
    }

    statusLabel->setText(QStringLiteral("正在启动 %1...").arg(petName));

    activePetWindow = new PetWindow(petName, nullptr);
    activePetName = petName;

    StatisticManager::getInstance().recordPetStart(activePetName);

    connect(activePetWindow, &PetWindow::requestStop, this, &MainWindow::OnStopPet);

    const int sizePercent = sizeSlider->value();
    const bool alwaysOnTop = alwaysOnTopCheckBox->isChecked();
    const bool clickThrough = clickThroughCheckBox->isChecked();
    const bool aiEnabled = aiEnabledCheckBox->isChecked();
    const ScreenChatConfig screenChatConfig = buildScreenChatConfigFromUi();
    const VoiceConfig voiceConfig = buildVoiceConfigFromUi();
    ConfigManager::instance().setLlmEnabled(aiEnabled);
    ConfigManager::instance().setVoiceConfig(voiceConfig);
    activePetWindow->applySettings(sizePercent, alwaysOnTop, clickThrough, aiEnabled, screenChatConfig, voiceConfig);

    activePetWindow->show();

    startPetButton->setEnabled(false);
    stopPetButton->setEnabled(true);
    setWindowFlagControlsLocked(true);

    statusLabel->setText(QStringLiteral("%1 正在运行").arg(petName));
}

void MainWindow::OnStopPet() {
    const QString petName = !activePetName.isEmpty() ? activePetName : currentPetName();

    if (petName.isEmpty()) {
        statusLabel->setText(QStringLiteral("就绪"));
        return;
    }

    statusLabel->setText(QStringLiteral("正在停止 %1...").arg(petName));

    StatisticManager::getInstance().recordPetStop(petName);

    if (activePetWindow) {
        activePetWindow->close();
        activePetWindow->deleteLater();
        activePetWindow = nullptr;
    }
    activePetName.clear();

    startPetButton->setEnabled(characterComboBox && characterComboBox->count() > 0);
    stopPetButton->setEnabled(false);
    setWindowFlagControlsLocked(false);

    statusLabel->setText(QStringLiteral("就绪"));
}

void MainWindow::OnSettingsChanged() {
    if (statusLabel) {
        statusLabel->setText(QStringLiteral("设置已更改"));
    }

    const bool aiEnabled = aiEnabledCheckBox && aiEnabledCheckBox->isChecked();
    const VoiceConfig voiceConfig = buildVoiceConfigFromUi();
    ConfigManager::instance().setLlmEnabled(aiEnabled);
    ConfigManager::instance().setVoiceConfig(voiceConfig);

    if (activePetWindow) {
        const int sizePercent = sizeSlider ? sizeSlider->value() : 100;
        const ScreenChatConfig screenChatConfig = buildScreenChatConfigFromUi();
        activePetWindow->applyRuntimeSettings(sizePercent, aiEnabled, screenChatConfig, voiceConfig);
    }
}

void MainWindow::OnBubbleAppearanceChanged() {
    OnSettingsChanged();
    if (activePetWindow) {
        activePetWindow->previewBubble();
    }
}

void MainWindow::setWindowFlagControlsLocked(bool locked) {
    if (alwaysOnTopCheckBox) {
        alwaysOnTopCheckBox->setEnabled(!locked);
    }
    if (clickThroughCheckBox) {
        clickThroughCheckBox->setEnabled(!locked);
    }
}

ScreenChatConfig MainWindow::buildScreenChatConfigFromUi() const {
    ScreenChatConfig cfg = ConfigManager::instance().getScreenChatConfig();
    cfg.enabled = autoScreenChatCheckBox ? autoScreenChatCheckBox->isChecked() : cfg.enabled;
    cfg.bubbleOpacityPercent = bubbleOpacitySlider ? bubbleOpacitySlider->value() : cfg.bubbleOpacityPercent;
    cfg.bubbleFontSize = bubbleFontSizeSpinBox ? bubbleFontSizeSpinBox->value() : cfg.bubbleFontSize;
    cfg.bubbleOffsetX = bubbleOffsetXSpinBox ? bubbleOffsetXSpinBox->value() : cfg.bubbleOffsetX;
    cfg.bubbleOffsetY = bubbleOffsetYSpinBox ? bubbleOffsetYSpinBox->value() : cfg.bubbleOffsetY;
    if (chatIntervalSpinBox) {
        const int minutes = chatIntervalSpinBox->value();
        const int minMinutes = std::max(1, minutes - 2);
        const int maxMinutes = std::max(minMinutes, minutes + 2);
        cfg.minIntervalMs = minMinutes * 60 * 1000;
        cfg.maxIntervalMs = maxMinutes * 60 * 1000;
    } else {
        cfg.minIntervalMs = 8 * 60 * 1000;
        cfg.maxIntervalMs = 12 * 60 * 1000;
    }
    cfg.petGender = QStringLiteral("female");
    return cfg;
}

VoiceConfig MainWindow::buildVoiceConfigFromUi() const {
    VoiceConfig cfg = ConfigManager::instance().getVoiceConfig();
    cfg.enabled = voiceEnabledCheckBox ? voiceEnabledCheckBox->isChecked() : cfg.enabled;
    cfg.preloadOnStart = cfg.enabled;

    const QString selected = voiceSpeakerComboBox
        ? voiceSpeakerComboBox->currentData().toString().trimmed().toLower()
        : cfg.selectedSpeaker;
    if (selected == QStringLiteral("custom")) {
        cfg.speakerMode = QStringLiteral("custom");
    } else if (selected == QStringLiteral("feibi")
               || selected == QStringLiteral("mika")
               || selected == QStringLiteral("thirtyseven")) {
        cfg.speakerMode = QStringLiteral("predefined");
        cfg.selectedSpeaker = selected;
    }

    return cfg;
}

void MainWindow::OnAbout() {
    QMessageBox::about(this, QStringLiteral("关于 Desktop Pet"),
                       QStringLiteral("<h2>Desktop Pet</h2>"
                                      "<p>一个可爱的桌面宠物应用程序</p>"
                                      "<p>版本: 1.0.3</p>"
                                      "<p>使用 Qt6 Widgets 开发</p>"));
}

void MainWindow::OnAddPet() {
    const QString petName = QInputDialog::getText(this, QStringLiteral("添加桌宠"), QStringLiteral("请输入桌宠名称:"));
    if (petName.isEmpty()) {
        return;
    }

    if (Pet::instance().hasPet(petName)) {
        QMessageBox::warning(this, QStringLiteral("错误"), QStringLiteral("桌宠名称已存在!"));
        return;
    }

    const QString modelPath = QFileDialog::getOpenFileName(this,
                                                           QStringLiteral("选择模型文件"),
                                                           QStringLiteral("assets/models/"),
                                                           QStringLiteral("GLTF Files (*.gltf *.glb)"));
    if (modelPath.isEmpty()) {
        return;
    }

    Pet::instance().addPet(petName, modelPath);
    loadPetList();

    const int index = characterComboBox ? characterComboBox->findData(petName) : -1;
    if (index >= 0) {
        characterComboBox->setCurrentIndex(index);
    }
}
