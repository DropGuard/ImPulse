#include "settingsdialog.h"

#include "appversion.h"

#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <algorithm>

#include <utility>
#include "ui_settingsdialog.h"

// the about text's single home: the version line is dynamic, and a second
// copy living in the .ui would be the next change-one-miss-the-rest drift
static QString aboutHtml() {
    return QStringLiteral("<p><b>ImPulse %1</b></p>"
                          "<p>A fast and easy to use image viewer. Video support via libmpv.</p>"
                          "<p>ImPulse is a fork of qimgv: the backend now runs on a Rust engine "
                          "that owns the directory listing and the decode schedule — it decides "
                          "which decode runs next, while the C++ side submits the requests and "
                          "executes the work.</p>"
                          "<p><b>GitHub page:</b> "
                          "<a href=\"https://github.com/DropGuard/ImPulse\">"
                          "https://github.com/DropGuard/ImPulse</a></p>"
                          "<p><b>Based on qimgv</b> by "
                          "<a href=\"https://github.com/easymodo/qimgv\">easymodo</a> and "
                          "<a href=\"https://github.com/easymodo/qimgv/graphs/contributors\">"
                          "contributors</a> — the interface and the artwork come from there.</p>"
                          "<p>OpenCV enabled builds use "
                          "<a href=\"https://github.com/dbzhang800/QtOpenCV\">QtOpenCV</a> "
                          "by dbzhang800</p>"
                          "<p>ImPulse is licensed under the "
                          "<a href=\"https://www.gnu.org/licenses/gpl-3.0.en.html\">"
                          "GNU GPL Version 3</a>.</p>"
                          "<p>Report any issues / request features "
                          "<a href=\"https://github.com/DropGuard/ImPulse/issues\">here</a>.</p>")
        .arg(appVersion.toString());
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent), ui(new Ui::SettingsDialog) {
    ui->setupUi(this);
    mChecks = {
        {ui->loopSlideshowCheckBox, &Settings::loopSlideshow, &Settings::setLoopSlideshow},
        {ui->fullscreenCheckBox, &Settings::fullscreenMode, &Settings::setFullscreenMode},
        {ui->videoPlaybackCheckBox, &Settings::videoPlayback, &Settings::setVideoPlayback},
        {ui->playSoundsCheckBox, &Settings::playVideoSounds, &Settings::setPlayVideoSounds},
        {ui->enablePanelCheckBox, &Settings::panelEnabled, &Settings::setPanelEnabled},
        {ui->panelFullscreenOnlyCheckBox, &Settings::panelFullscreenOnly, &Settings::setPanelFullscreenOnly},
        {ui->squareThumbnailsCheckBox, &Settings::squareThumbnails, &Settings::setSquareThumbnails},
        {ui->transparencyGridCheckBox, &Settings::transparencyGrid, &Settings::setTransparencyGrid},
        {ui->showHiddenFilesCheckBox, &Settings::showHiddenFiles, &Settings::setShowHiddenFiles},
        {ui->enableSmoothScrollCheckBox, &Settings::enableSmoothScroll, &Settings::setEnableSmoothScroll},
        {ui->usePreloaderCheckBox, &Settings::usePreloader, &Settings::setUsePreloader},
        {ui->useThumbnailCacheCheckBox, &Settings::useThumbnailCache, &Settings::setUseThumbnailCache},
        {ui->smoothUpscalingCheckBox, &Settings::smoothUpscaling, &Settings::setSmoothUpscaling},
        {ui->expandImageCheckBox, &Settings::expandImage, &Settings::setExpandImage},
        {ui->smoothAnimatedImagesCheckBox, &Settings::smoothAnimatedImages, &Settings::setSmoothAnimatedImages},
        {ui->blurBackgroundCheckBox, &Settings::blurBackground, &Settings::setBlurBackground},
        {ui->confirmTrashCheckBox, &Settings::confirmTrash, &Settings::setConfirmTrash},
        {ui->unlockMinZoomCheckBox, &Settings::unlockMinZoom, &Settings::setUnlockMinZoom},
        {ui->trackpadDetectionCheckBox, &Settings::trackpadDetection, &Settings::setTrackpadDetection},
        {ui->clickableEdgesCheckBox, &Settings::clickableEdges, &Settings::setClickableEdges},
        {ui->clickableEdgesVisibleCheckBox, &Settings::clickableEdgesVisible, &Settings::setClickableEdgesVisible},
        {ui->showInfoBarFullscreen, &Settings::infoBarFullscreen, &Settings::setInfoBarFullscreen},
        {ui->showInfoBarWindowed, &Settings::infoBarWindowed, &Settings::setInfoBarWindowed},
        {ui->showExtendedInfoTitle, &Settings::windowTitleExtendedInfo, &Settings::setWindowTitleExtendedInfo},
        {ui->cursorAutohideCheckBox, &Settings::cursorAutohide, &Settings::setCursorAutohide},
        {ui->saveOverlayCheckBox, &Settings::showSaveOverlay, &Settings::setShowSaveOverlay},
        {ui->unloadThumbsCheckBox, &Settings::unloadThumbs, &Settings::setUnloadThumbs},
        {ui->animatedJxlCheckBox, &Settings::jxlAnimation, &Settings::setJxlAnimation},
        {ui->autoResizeWindowCheckBox, &Settings::autoResizeWindow, &Settings::setAutoResizeWindow},
        {ui->panelCenterSelectionCheckBox, &Settings::panelCenterSelection, &Settings::setPanelCenterSelection},
        {ui->useFixedZoomLevelsCheckBox, &Settings::useFixedZoomLevels, &Settings::setUseFixedZoomLevels},
        {ui->pinPanelCheckBox, &Settings::panelPinned, &Settings::setPanelPinned},
    };

    this->setWindowTitle(tr("Preferences — ") + qApp->applicationName());

    ui->shortcutsTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->aboutAppTextBrowser->viewport()->setAutoFillBackground(false);
    ui->aboutAppTextBrowser->setHtml(aboutHtml());
    ui->versionLabel->setText("" + QApplication::applicationVersion());
    ui->qtVersionLabel->setText(qVersion());
    ui->appIconLabel->setPixmap(QIcon(":/res/icons/common/logo/app/22.png").pixmap(22, 22));
    ui->qtIconLabel->setPixmap(QIcon(":/res/icons/common/logo/3rdparty/qt22.png").pixmap(22, 16));

    // fake combobox that acts as a menu button
    // less code than using pushbutton with menu
    // will be replaced with something custom later
    connect(ui->themeSelectorComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        ui->themeSelectorComboBox->blockSignals(true);
        ui->themeSelectorComboBox->setCurrentIndex(index);
        ui->themeSelectorComboBox->blockSignals(false);
        switch(index) {
        case 0:
            setColorScheme(ThemeStore::colorScheme(COLORS_BLACK));
            settings->setColorTid(COLORS_BLACK);
            break;
        case 1:
            setColorScheme(ThemeStore::colorScheme(COLORS_DARK));
            settings->setColorTid(COLORS_DARK);
            break;
        case 2:
            setColorScheme(ThemeStore::colorScheme(COLORS_DARKBLUE));
            settings->setColorTid(COLORS_DARKBLUE);
            break;
        case 3:
            setColorScheme(ThemeStore::colorScheme(COLORS_LIGHT));
            settings->setColorTid(COLORS_LIGHT);
            break;
        default: break;
        }
    });

    connect(ui->useSystemColorsCheckBox, &QCheckBox::toggled, this, [this](bool useSystemTheme) {
        if(useSystemTheme) {
            ui->themeSelectorComboBox->setCurrentIndex(-1);
            setColorScheme(ThemeStore::colorScheme(COLORS_SYSTEM));
            settings->setColorTid(COLORS_SYSTEM);
        } else {
            readColorScheme();
            settings->setColorTid(COLORS_CUSTOMIZED);
        }
        ui->themeSelectorComboBox->setEnabled(!useSystemTheme);
        ui->colorConfigSubgroup->setEnabled(!useSystemTheme);
        ui->modifySystemSchemeLabel->setVisible(useSystemTheme);
    });

    connect(ui->modifySystemSchemeLabel, &ClickableLabel::clicked, this, [this]() {
        ui->useSystemColorsCheckBox->setChecked(false);
        setColorScheme(ThemeStore::colorScheme(COLORS_CUSTOMIZED));
        settings->setColorTid(COLORS_CUSTOMIZED);
    });

    ui->colorSelectorAccent->setDescription(tr("Accent color"));
    ui->colorSelectorBackground->setDescription(tr("Windowed mode background"));
    ui->colorSelectorFullscreen->setDescription(tr("Fullscreen mode background"));
    ui->colorSelectorFolderview->setDescription(tr("FolderView background"));
    ui->colorSelectorFolderviewPanel->setDescription(tr("FolderView top panel"));
    ui->colorSelectorText->setDescription(tr("Text color"));
    ui->colorSelectorWidget->setDescription(tr("Widget background"));
    ui->colorSelectorWidgetBorder->setDescription(tr("Widget border"));
    ui->colorSelectorOverlay->setDescription(tr("Overlay background"));
    ui->colorSelectorOverlayText->setDescription(tr("Overlay text"));
    ui->colorSelectorScrollbar->setDescription(tr("Scrollbars"));

#ifndef USE_KDE_BLUR
    ui->blurBackgroundCheckBox->setEnabled(false);
#endif

#ifndef USE_MPV
    ui->videoPlaybackGroup->setEnabled(false);
    //ui->novideoInfoLabel->setHidden(false);
#else
    //ui->novideoInfoLabel->setHidden(true);
#endif

#ifdef USE_OPENCV
    ui->scalingQualityComboBox->addItem("Bilinear+sharpen (OpenCV)");
    ui->scalingQualityComboBox->addItem("Bicubic (OpenCV)");
    ui->scalingQualityComboBox->addItem("Bicubic+sharpen (OpenCV)");
#endif

    if(!settings->supportedFormats().contains("jxl"))
        ui->animatedJxlCheckBox->hide();

    setupSidebar();

    // setup radioBtn groups
    fitModeGrp.addButton(ui->fitModeWindow);
    fitModeGrp.addButton(ui->fitModeWidth);
    fitModeGrp.addButton(ui->fitMode1to1);
    folderEndGrp.addButton(ui->folderEndSwitchFolder);
    folderEndGrp.addButton(ui->folderEndNoAction);
    folderEndGrp.addButton(ui->folderEndLoop);
    zoomIndGrp.addButton(ui->zoomIndicatorAuto);
    zoomIndGrp.addButton(ui->zoomIndicatorOff);
    zoomIndGrp.addButton(ui->zoomIndicatorOn);

    // readable language names (ctor-local: consumed by the binding below)
    QMap<QString, QString> langs;
    langs.insert("de_DE", "Deutsch");
    langs.insert("en_US", "English");
    langs.insert("es_ES", "Español");
    langs.insert("fr_FR", "Français");
    langs.insert("ja_JP", "日本語");
    langs.insert("tr_TR", "Türkçe");
    langs.insert("uk_UA", "Українська");
    langs.insert("zh_CN", "简体中文");
    // fill langs combobox, sorted by locale
    ui->langComboBox->addItems(langs.values());
    // insert system language entry manually at the beginning
    langs.insert("system", "System language");
    ui->langComboBox->insertItem(0, "System language");
    // the combo is [System language] + locales in map order; the stored value
    // for index 0 is "system". Unknown stored values fall back to en_US.
    QStringList langValues = langs.keys(); // sorted, "system" included
    langValues.removeAll("system");
    langValues.prepend("system"); // combo index 0 is the System entry
    mCombos = {{ui->langComboBox, &K_Language, langValues, QStringLiteral("en_US")}};

    connect(this, &SettingsDialog::settingsChanged, settings, &Settings::sendChangeNotification);
    readSettings();

    adjustSizeToContents();
}
//------------------------------------------------------------------------------
SettingsDialog::~SettingsDialog() {
    delete ui;
}
//------------------------------------------------------------------------------
// an attempt to force minimum width to fit contents
void SettingsDialog::adjustSizeToContents() {
    // general tab
    ui->gridLayout->activate();
    ui->horizontalLayout_28->activate();
    ui->horizontalLayout_19->activate();
    ui->gridLayout_3->activate();
    ui->horizontalLayout_18->activate();
    ui->gridLayout_4->activate();
    ui->horizontalLayout_24->activate();
    ui->gridLayout_5->activate();
    ui->slideshowGroupContents->activate();
    ui->scrollAreaWidgetContents->layout()->activate();
    ui->scrollArea->setMinimumWidth(ui->scrollAreaWidgetContents->minimumSizeHint().width());
    // view tab
    ui->horizontalLayout_29->activate();
    ui->horizontalLayout_31->activate();
    ui->widget->layout()->activate();
    ui->scrollAreaWidgetContents_3->layout()->activate();
    ui->scrollArea_3->setMinimumWidth(ui->scrollAreaWidgetContents_3->minimumSizeHint().width());
    // container
    //ui->stackedWidget->layout()->activate();
    this->setMinimumWidth(sizeHint().width() + 22);

    //qDebug() << "window:" << this->sizeHint() << this->minimumSizeHint() << this->size();
    //qDebug() << "stackedwidget:" << ui->stackedWidget->sizeHint() << ui->stackedWidget->minimumSizeHint() << ui->stackedWidget->size();
    //qDebug() << "scrollarea:" << ui->scrollArea->sizeHint() << ui->scrollArea->minimumSizeHint() << ui->scrollArea->size();
    //qDebug() << "scrollareawidget:" << ui->scrollAreaWidgetContents->sizeHint() << ui->scrollAreaWidgetContents->minimumSizeHint() << ui->scrollAreaWidgetContents->size();
    //qDebug() << "grid" << ui->gridLayout_15->sizeHint();
    //qDebug() << "wtf" << ui->startInFolderViewCheckBox->sizeHint() << ui->startInFolderViewCheckBox->minimumSizeHint();
}
//------------------------------------------------------------------------------
void SettingsDialog::resetToDesktopTheme() {
    settings->setColorScheme(ThemeStore::colorScheme(ColorSchemes::COLORS_SYSTEM));
    this->readColorScheme();
}
//------------------------------------------------------------------------------
void SettingsDialog::setupSidebar() {
}
//------------------------------------------------------------------------------
void SettingsDialog::readSettings() {
    // checkbox bindings: one declaration, both directions
    for(const CheckBinding &b : std::as_const(mChecks))
        b.box->setChecked((settings->*b.get)());
    for(const ComboBinding &b : std::as_const(mCombos)) {
        int idx = b.values.indexOf(settings->load<QString>(*b.desc));
        if(idx < 0)
            idx = b.values.indexOf(b.fallback);
        b.box->setCurrentIndex(idx);
    }
    ui->loopSlideshowCheckBox->setChecked(settings->loopSlideshow());
    ui->videoPlaybackCheckBox->setChecked(settings->videoPlayback());
    ui->videoPlaybackGroupContents->setEnabled(settings->videoPlayback());
    ui->playSoundsCheckBox->setChecked(settings->playVideoSounds());
    ui->enablePanelCheckBox->setChecked(settings->panelEnabled());
    ui->thumbnailPanelGroupContents->setEnabled(settings->panelEnabled());
    ui->panelFullscreenOnlyCheckBox->setChecked(settings->panelFullscreenOnly());
    ui->squareThumbnailsCheckBox->setChecked(settings->squareThumbnails());
    ui->transparencyGridCheckBox->setChecked(settings->transparencyGrid());
    ui->enableSmoothScrollCheckBox->setChecked(settings->enableSmoothScroll());
    ui->usePreloaderCheckBox->setChecked(settings->usePreloader());
    ui->useThumbnailCacheCheckBox->setChecked(settings->useThumbnailCache());
    ui->smoothUpscalingCheckBox->setChecked(settings->smoothUpscaling());
    ui->expandImageCheckBox->setChecked(settings->expandImage());
    ui->expandImagesGroupContents->setEnabled(settings->expandImage());
    ui->smoothAnimatedImagesCheckBox->setChecked(settings->smoothAnimatedImages());
    ui->bgOpacitySlider->setValue(static_cast<int>(settings->backgroundOpacity() * 100));
    ui->blurBackgroundCheckBox->setChecked(settings->blurBackground());
    ui->sortingComboBox->setCurrentIndex(settings->sortingMode());
    ui->confirmTrashCheckBox->setChecked(settings->confirmTrash());
    ui->unlockMinZoomCheckBox->setChecked(settings->unlockMinZoom());
    ui->trackpadDetectionCheckBox->setChecked(settings->trackpadDetection());
    ui->clickableEdgesCheckBox->setChecked(settings->clickableEdges());
    ui->clickableEdgesVisibleCheckBox->setChecked(settings->clickableEdgesVisible());
    ui->clickableEdgesVisibleCheckBox->setEnabled(settings->clickableEdges());
    ui->showHiddenFilesCheckBox->setChecked(settings->showHiddenFiles());

    if(settings->zoomIndicatorMode() == INDICATOR_ENABLED)
        ui->zoomIndicatorOn->setChecked(true);
    else if(settings->zoomIndicatorMode() == INDICATOR_AUTO)
        ui->zoomIndicatorAuto->setChecked(true);
    else
        ui->zoomIndicatorOff->setChecked(true);
    ui->showInfoBarFullscreen->setChecked(settings->infoBarFullscreen());
    ui->showInfoBarWindowed->setChecked(settings->infoBarWindowed());
    ui->showExtendedInfoTitle->setChecked(settings->windowTitleExtendedInfo());
    ui->cursorAutohideCheckBox->setChecked(settings->cursorAutohide());
    if(settings->focusPointIn1to1Mode() == FOCUS_TOP)
        ui->focus1to1Top->setChecked(true);
    else if(settings->focusPointIn1to1Mode() == FOCUS_CENTER)
        ui->focus1to1Center->setChecked(true);
    else
        ui->focus1to1Cursor->setChecked(true);
    ui->slideshowIntervalSpinBox->setValue(settings->slideshowInterval());
    ui->imageScrollingComboBox->setCurrentIndex(settings->imageScrolling());
    ui->saveOverlayCheckBox->setChecked(settings->showSaveOverlay());
    ui->unloadThumbsCheckBox->setChecked(settings->unloadThumbs());
    if(settings->thumbPanelStyle() == TH_PANEL_SIMPLE)
        ui->thumbStyleSimple->setChecked(true);
    else
        ui->thumbStyleExtended->setChecked(true);
    ui->animatedJxlCheckBox->setChecked(settings->jxlAnimation());
    ui->autoResizeWindowCheckBox->setChecked(settings->autoResizeWindow());
    ui->panelCenterSelectionCheckBox->setChecked(settings->panelCenterSelection());
    ui->useFixedZoomLevelsCheckBox->setChecked(settings->useFixedZoomLevels());
    ui->zoomLevels->setText(settings->zoomLevels());

    if(settings->defaultViewMode() == MODE_FOLDERVIEW)
        ui->startInFolderViewCheckBox->setChecked(true);
    else
        ui->startInFolderViewCheckBox->setChecked(false);

    if(settings->folderEndAction() == FOLDER_END_NO_ACTION)
        ui->folderEndNoAction->setChecked(true);
    else if(settings->folderEndAction() == FOLDER_END_LOOP)
        ui->folderEndLoop->setChecked(true);
    else
        ui->folderEndSwitchFolder->setChecked(true);

    ui->mpvLineEdit->setText(settings->mpvBinary());
    ui->quickCopyLineEdit->setText(settings->quickCopyDirectory());

    ui->zoomStepSlider->setValue(static_cast<int>(settings->zoomStep() * 100.f));
    onZoomStepSliderChanged(ui->zoomStepSlider->value());

    ui->mouseScrollingSpeedSlider->setValue(static_cast<int>((settings->mouseScrollingSpeed() - 0.5f) / 0.25f));
    onMouseScrollingSpeedSliderChanged(ui->mouseScrollingSpeedSlider->value());

    ui->autoResizeLimitSlider->setValue(static_cast<int>(settings->autoResizeLimit() / 5.f));
    onAutoResizeLimitSliderChanged(ui->autoResizeLimitSlider->value());

    ui->JPEGQualitySlider->setValue(settings->JPEGSaveQuality());
    onJPEGQualitySliderChanged(ui->JPEGQualitySlider->value());

    ui->expandLimitSlider->setValue(settings->expandLimit());
    onExpandLimitSliderChanged(ui->expandLimitSlider->value());

    // thumbnailer threads
    ui->thumbnailerThreadsSlider->setValue(settings->thumbnailerThreadCount());
    onThumbnailerThreadsSliderChanged(ui->thumbnailerThreadsSlider->value());

    ui->memoryLimitSpinBox->setValue(settings->memoryAllocationLimit());

    // ##### fit mode #####
    if(settings->imageFitMode() == FIT_WINDOW)
        ui->fitModeWindow->setChecked(true);
    else if(settings->imageFitMode() == FIT_WIDTH)
        ui->fitModeWidth->setChecked(true);
    else
        ui->fitMode1to1->setChecked(true);

    // ##### UI #####
    ui->scalingQualityComboBox->setCurrentIndex(settings->scalingFilter());
    ui->fullscreenCheckBox->setChecked(settings->fullscreenMode());
    ui->pinPanelCheckBox->setChecked(settings->panelPinned());
    ui->panelPositionComboBox->setCurrentIndex(settings->panelPosition());

    // reduce by 10x to have nice granular control in qslider
    ui->panelSizeSlider->setValue(settings->panelPreviewsSize() / 10);

    ui->useSystemColorsCheckBox->setChecked(settings->useSystemColorScheme());
    ui->modifySystemSchemeLabel->setVisible(settings->useSystemColorScheme());
    ui->themeSelectorComboBox->setEnabled(!settings->useSystemColorScheme());
    ui->colorConfigSubgroup->setEnabled(!settings->useSystemColorScheme());

    readColorScheme();
    readShortcuts();
    readScripts();
}
//------------------------------------------------------------------------------
void SettingsDialog::saveSettings() {
    // background work is waited on where the new settings are consumed:
    // Core::readSettings() drains the shared worker pool before re-reading
    for(const CheckBinding &b : std::as_const(mChecks))
        (settings->*b.set)(b.box->isChecked());
    for(const ComboBinding &b : std::as_const(mCombos)) {
        int idx = b.box->currentIndex();
        if(idx >= 0 && idx < b.values.size())
            settings->store(*b.desc, b.values.at(idx));
    }

    settings->setLoopSlideshow(ui->loopSlideshowCheckBox->isChecked());
    settings->setFullscreenMode(ui->fullscreenCheckBox->isChecked());
    if(ui->fitModeWindow->isChecked())
        settings->setImageFitMode(FIT_WINDOW);
    else if(ui->fitModeWidth->isChecked())
        settings->setImageFitMode(FIT_WIDTH);
    else
        settings->setImageFitMode(FIT_ORIGINAL);

    settings->setVideoPlayback(ui->videoPlaybackCheckBox->isChecked());
    settings->setPlayVideoSounds(ui->playSoundsCheckBox->isChecked());
    settings->setPanelEnabled(ui->enablePanelCheckBox->isChecked());
    settings->setPanelFullscreenOnly(ui->panelFullscreenOnlyCheckBox->isChecked());
    settings->setSquareThumbnails(ui->squareThumbnailsCheckBox->isChecked());
    settings->setTransparencyGrid(ui->transparencyGridCheckBox->isChecked());
    settings->setShowHiddenFiles(ui->showHiddenFilesCheckBox->isChecked());
    settings->setEnableSmoothScroll(ui->enableSmoothScrollCheckBox->isChecked());
    settings->setUsePreloader(ui->usePreloaderCheckBox->isChecked());
    settings->setUseThumbnailCache(ui->useThumbnailCacheCheckBox->isChecked());
    settings->setSmoothUpscaling(ui->smoothUpscalingCheckBox->isChecked());
    settings->setExpandImage(ui->expandImageCheckBox->isChecked());
    settings->setSmoothAnimatedImages(ui->smoothAnimatedImagesCheckBox->isChecked());

    settings->setBackgroundOpacity(static_cast<qreal>(ui->bgOpacitySlider->value()) / 100.f);
    settings->setBlurBackground(ui->blurBackgroundCheckBox->isChecked());
    settings->setSortingMode(static_cast<SortingMode>(ui->sortingComboBox->currentIndex()));
    settings->setConfirmTrash(ui->confirmTrashCheckBox->isChecked());
    settings->setUnlockMinZoom(ui->unlockMinZoomCheckBox->isChecked());
    settings->setTrackpadDetection(ui->trackpadDetectionCheckBox->isChecked());
    settings->setClickableEdges(ui->clickableEdgesCheckBox->isChecked());
    settings->setClickableEdgesVisible(ui->clickableEdgesVisibleCheckBox->isChecked());

    if(ui->zoomIndicatorOn->isChecked())
        settings->setZoomIndicatorMode(INDICATOR_ENABLED);
    else if(ui->zoomIndicatorAuto->isChecked())
        settings->setZoomIndicatorMode(INDICATOR_AUTO);
    else
        settings->setZoomIndicatorMode(INDICATOR_DISABLED);
    settings->setInfoBarFullscreen(ui->showInfoBarFullscreen->isChecked());
    settings->setInfoBarWindowed(ui->showInfoBarWindowed->isChecked());
    settings->setWindowTitleExtendedInfo(ui->showExtendedInfoTitle->isChecked());
    settings->setCursorAutohide(ui->cursorAutohideCheckBox->isChecked());
    if(ui->focus1to1Top->isChecked())
        settings->setFocusPointIn1to1Mode(FOCUS_TOP);
    else if(ui->focus1to1Center->isChecked())
        settings->setFocusPointIn1to1Mode(FOCUS_CENTER);
    else
        settings->setFocusPointIn1to1Mode(FOCUS_CURSOR);

    settings->setSlideshowInterval(ui->slideshowIntervalSpinBox->value());

    if(ui->startInFolderViewCheckBox->isChecked())
        settings->setDefaultViewMode(MODE_FOLDERVIEW);
    else
        settings->setDefaultViewMode(MODE_DOCUMENT);

    if(ui->folderEndNoAction->isChecked())
        settings->setFolderEndAction(FOLDER_END_NO_ACTION);
    else if(ui->folderEndLoop->isChecked())
        settings->setFolderEndAction(FOLDER_END_LOOP);
    else
        settings->setFolderEndAction(FOLDER_END_GOTO_ADJACENT);

    settings->setMpvBinary(ui->mpvLineEdit->text());
    settings->setQuickCopyDirectory(ui->quickCopyLineEdit->text());
    settings->setScalingFilter(static_cast<ScalingFilter>(ui->scalingQualityComboBox->currentIndex()));
    settings->setImageScrolling(static_cast<ImageScrolling>(ui->imageScrollingComboBox->currentIndex()));
    settings->setShowSaveOverlay(ui->saveOverlayCheckBox->isChecked());
    settings->setUnloadThumbs(ui->unloadThumbsCheckBox->isChecked());
    if(ui->thumbStyleSimple->isChecked())
        settings->setThumbPanelStyle(TH_PANEL_SIMPLE);
    else
        settings->setThumbPanelStyle(TH_PANEL_EXTENDED);
    settings->setJxlAnimation(ui->animatedJxlCheckBox->isChecked());
    settings->setAutoResizeWindow(ui->autoResizeWindowCheckBox->isChecked());
    settings->setPanelCenterSelection(ui->panelCenterSelectionCheckBox->isChecked());
    settings->setUseFixedZoomLevels(ui->useFixedZoomLevelsCheckBox->isChecked());
    settings->setZoomLevels(ui->zoomLevels->text());

    settings->setPanelPinned(ui->pinPanelCheckBox->isChecked());
    int panelPos = ui->panelPositionComboBox->currentIndex();
    settings->setPanelPosition(static_cast<PanelPosition>(panelPos));

    settings->setPanelPreviewsSize(ui->panelSizeSlider->value() * 10);

    settings->setJPEGSaveQuality(ui->JPEGQualitySlider->value());
    settings->setZoomStep(static_cast<qreal>(ui->zoomStepSlider->value() / 100.f));
    settings->setMouseScrollingSpeed(static_cast<qreal>(0.5f + (ui->mouseScrollingSpeedSlider->value() * 0.25f)));
    settings->setAutoResizeLimit(ui->autoResizeLimitSlider->value() * 5);
    settings->setExpandLimit(ui->expandLimitSlider->value());
    settings->setThumbnailerThreadCount(ui->thumbnailerThreadsSlider->value());
    settings->setMemoryAllocationLimit(ui->memoryLimitSpinBox->value());

    settings->setUseSystemColorScheme(ui->useSystemColorsCheckBox->isChecked());

    saveColorScheme();
    saveShortcuts();

    scriptManager->saveScripts();
    // one loop over the family: the four hand-written calls were a fifth
    // enumeration of the scopes in the same function that already loops over
    // them to save, and an enumeration of a family goes stale (this one had
    // the order Global, Grid, Document, Inspect — arbitrary, and unread).
    for(Scope scope : kAllScopes)
        actionManager->saveScope(scope);
    emit settingsChanged();
}
//------------------------------------------------------------------------------
void SettingsDialog::saveSettingsAndClose() {
    saveSettings();
    this->close();
}
//------------------------------------------------------------------------------
void SettingsDialog::resetZoomLevels() {
    ui->zoomLevels->setText(settings->defaultZoomLevels());
}
//------------------------------------------------------------------------------
void SettingsDialog::selectMpvPath() {
    QFileDialog dialog;
    QString file;
    file = dialog.getOpenFileName(this, tr("Navigate to mpv binary"), "", "mpv*");
    if(!file.isEmpty()) {
        ui->mpvLineEdit->setText(file);
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::onExpandLimitSliderChanged(int value) {
    if(value == 0)
        ui->expandLimitLabel->setText("-");
    else
        ui->expandLimitLabel->setText(QString::number(value) + "x");
}
//------------------------------------------------------------------------------
void SettingsDialog::onJPEGQualitySliderChanged(int value) {
    ui->JPEGQualityLabel->setText(QString::number(value) + "%");
}
//------------------------------------------------------------------------------
void SettingsDialog::onZoomStepSliderChanged(int value) {
    ui->zoomStepLabel->setText(QString::number(value / 100.f, 'f', 2) + "x");
}
//------------------------------------------------------------------------------
void SettingsDialog::onMouseScrollingSpeedSliderChanged(int value) {
    ui->mouseScrollingSpeedLabel->setText(QString::number(0.5f + (value * 0.25f), 'f', 2) + "x");
}
//------------------------------------------------------------------------------
void SettingsDialog::onThumbnailerThreadsSliderChanged(int value) {
    ui->thumbnailerThreadsLabel->setText(QString::number(value));
}
//------------------------------------------------------------------------------
void SettingsDialog::onBgOpacitySliderChanged(int value) {
    ui->bgOpacityPercentLabel->setText(QString::number(value) + "%");
}
//------------------------------------------------------------------------------
void SettingsDialog::onAutoResizeLimitSliderChanged(int value) {
    ui->autoResizeLimit->setText(QString::number(value * 5.f, 'f', 0) + "%");
}
//------------------------------------------------------------------------------
int SettingsDialog::exec() {
    return QDialog::exec();
}

void SettingsDialog::switchToPage(int number) {
    ui->sideBar2->selectEntry(number);
}
