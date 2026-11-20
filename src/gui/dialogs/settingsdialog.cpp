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

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent), m_ui(new Ui::SettingsDialog) {
    m_ui->setupUi(this);
    m_checks = {
        {m_ui->loopSlideshowCheckBox, &Settings::loopSlideshow, &Settings::setLoopSlideshow},
        {m_ui->fullscreenCheckBox, &Settings::fullscreenMode, &Settings::setFullscreenMode},
        {m_ui->videoPlaybackCheckBox, &Settings::videoPlayback, &Settings::setVideoPlayback},
        {m_ui->playSoundsCheckBox, &Settings::playVideoSounds, &Settings::setPlayVideoSounds},
        {m_ui->enablePanelCheckBox, &Settings::panelEnabled, &Settings::setPanelEnabled},
        {m_ui->panelFullscreenOnlyCheckBox, &Settings::panelFullscreenOnly, &Settings::setPanelFullscreenOnly},
        {m_ui->squareThumbnailsCheckBox, &Settings::squareThumbnails, &Settings::setSquareThumbnails},
        {m_ui->transparencyGridCheckBox, &Settings::transparencyGrid, &Settings::setTransparencyGrid},
        {m_ui->showHiddenFilesCheckBox, &Settings::showHiddenFiles, &Settings::setShowHiddenFiles},
        {m_ui->enableSmoothScrollCheckBox, &Settings::enableSmoothScroll, &Settings::setEnableSmoothScroll},
        {m_ui->usePreloaderCheckBox, &Settings::usePreloader, &Settings::setUsePreloader},
        {m_ui->useThumbnailCacheCheckBox, &Settings::useThumbnailCache, &Settings::setUseThumbnailCache},
        {m_ui->smoothUpscalingCheckBox, &Settings::smoothUpscaling, &Settings::setSmoothUpscaling},
        {m_ui->expandImageCheckBox, &Settings::expandImage, &Settings::setExpandImage},
        {m_ui->smoothAnimatedImagesCheckBox, &Settings::smoothAnimatedImages, &Settings::setSmoothAnimatedImages},
        {m_ui->blurBackgroundCheckBox, &Settings::blurBackground, &Settings::setBlurBackground},
        {m_ui->confirmTrashCheckBox, &Settings::confirmTrash, &Settings::setConfirmTrash},
        {m_ui->unlockMinZoomCheckBox, &Settings::unlockMinZoom, &Settings::setUnlockMinZoom},
        {m_ui->trackpadDetectionCheckBox, &Settings::trackpadDetection, &Settings::setTrackpadDetection},
        {m_ui->clickableEdgesCheckBox, &Settings::clickableEdges, &Settings::setClickableEdges},
        {m_ui->clickableEdgesVisibleCheckBox, &Settings::clickableEdgesVisible, &Settings::setClickableEdgesVisible},
        {m_ui->showInfoBarFullscreen, &Settings::infoBarFullscreen, &Settings::setInfoBarFullscreen},
        {m_ui->showInfoBarWindowed, &Settings::infoBarWindowed, &Settings::setInfoBarWindowed},
        {m_ui->showExtendedInfoTitle, &Settings::windowTitleExtendedInfo, &Settings::setWindowTitleExtendedInfo},
        {m_ui->cursorAutohideCheckBox, &Settings::cursorAutohide, &Settings::setCursorAutohide},
        {m_ui->saveOverlayCheckBox, &Settings::showSaveOverlay, &Settings::setShowSaveOverlay},
        {m_ui->unloadThumbsCheckBox, &Settings::unloadThumbs, &Settings::setUnloadThumbs},
        {m_ui->animatedJxlCheckBox, &Settings::jxlAnimation, &Settings::setJxlAnimation},
        {m_ui->autoResizeWindowCheckBox, &Settings::autoResizeWindow, &Settings::setAutoResizeWindow},
        {m_ui->panelCenterSelectionCheckBox, &Settings::panelCenterSelection, &Settings::setPanelCenterSelection},
        {m_ui->useFixedZoomLevelsCheckBox, &Settings::useFixedZoomLevels, &Settings::setUseFixedZoomLevels},
        {m_ui->pinPanelCheckBox, &Settings::panelPinned, &Settings::setPanelPinned},
    };

    this->setWindowTitle(tr("Preferences — ") + qApp->applicationName());

    m_ui->shortcutsTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_ui->aboutAppTextBrowser->viewport()->setAutoFillBackground(false);
    m_ui->aboutAppTextBrowser->setHtml(aboutHtml());
    m_ui->versionLabel->setText("" + QApplication::applicationVersion());
    m_ui->qtVersionLabel->setText(qVersion());
    m_ui->appIconLabel->setPixmap(QIcon(":/res/icons/common/logo/app/22.png").pixmap(22, 22));
    m_ui->qtIconLabel->setPixmap(QIcon(":/res/icons/common/logo/3rdparty/qt22.png").pixmap(22, 16));

    // fake combobox that acts as a menu button
    // less code than using pushbutton with menu
    // will be replaced with something custom later
    connect(m_ui->themeSelectorComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_ui->themeSelectorComboBox->blockSignals(true);
        m_ui->themeSelectorComboBox->setCurrentIndex(index);
        m_ui->themeSelectorComboBox->blockSignals(false);
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

    connect(m_ui->useSystemColorsCheckBox, &QCheckBox::toggled, this, [this](bool useSystemTheme) {
        if(useSystemTheme) {
            m_ui->themeSelectorComboBox->setCurrentIndex(-1);
            setColorScheme(ThemeStore::colorScheme(COLORS_SYSTEM));
            settings->setColorTid(COLORS_SYSTEM);
        } else {
            readColorScheme();
            settings->setColorTid(COLORS_CUSTOMIZED);
        }
        m_ui->themeSelectorComboBox->setEnabled(!useSystemTheme);
        m_ui->colorConfigSubgroup->setEnabled(!useSystemTheme);
        m_ui->modifySystemSchemeLabel->setVisible(useSystemTheme);
    });

    connect(m_ui->modifySystemSchemeLabel, &ClickableLabel::clicked, this, [this]() {
        m_ui->useSystemColorsCheckBox->setChecked(false);
        setColorScheme(ThemeStore::colorScheme(COLORS_CUSTOMIZED));
        settings->setColorTid(COLORS_CUSTOMIZED);
    });

    m_ui->colorSelectorAccent->setDescription(tr("Accent color"));
    m_ui->colorSelectorBackground->setDescription(tr("Windowed mode background"));
    m_ui->colorSelectorFullscreen->setDescription(tr("Fullscreen mode background"));
    m_ui->colorSelectorFolderview->setDescription(tr("FolderView background"));
    m_ui->colorSelectorFolderviewPanel->setDescription(tr("FolderView top panel"));
    m_ui->colorSelectorText->setDescription(tr("Text color"));
    m_ui->colorSelectorWidget->setDescription(tr("Widget background"));
    m_ui->colorSelectorWidgetBorder->setDescription(tr("Widget border"));
    m_ui->colorSelectorOverlay->setDescription(tr("Overlay background"));
    m_ui->colorSelectorOverlayText->setDescription(tr("Overlay text"));
    m_ui->colorSelectorScrollbar->setDescription(tr("Scrollbars"));

#ifndef USE_KDE_BLUR
    m_ui->blurBackgroundCheckBox->setEnabled(false);
#endif

#ifndef USE_MPV
    ui->videoPlaybackGroup->setEnabled(false);
    //ui->novideoInfoLabel->setHidden(false);
#else
    //ui->novideoInfoLabel->setHidden(true);
#endif

#ifdef USE_OPENCV
    m_ui->scalingQualityComboBox->addItem("Bilinear+sharpen (OpenCV)");
    m_ui->scalingQualityComboBox->addItem("Bicubic (OpenCV)");
    m_ui->scalingQualityComboBox->addItem("Bicubic+sharpen (OpenCV)");
#endif

    if(!settings->supportedFormats().contains("jxl"))
        m_ui->animatedJxlCheckBox->hide();

    setupSidebar();

    // setup radioBtn groups
    m_fitModeGrp.addButton(m_ui->fitModeWindow);
    m_fitModeGrp.addButton(m_ui->fitModeWidth);
    m_fitModeGrp.addButton(m_ui->fitMode1to1);
    m_folderEndGrp.addButton(m_ui->folderEndSwitchFolder);
    m_folderEndGrp.addButton(m_ui->folderEndNoAction);
    m_folderEndGrp.addButton(m_ui->folderEndLoop);
    m_zoomIndGrp.addButton(m_ui->zoomIndicatorAuto);
    m_zoomIndGrp.addButton(m_ui->zoomIndicatorOff);
    m_zoomIndGrp.addButton(m_ui->zoomIndicatorOn);

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
    m_ui->langComboBox->addItems(langs.values());
    // insert system language entry manually at the beginning
    langs.insert("system", "System language");
    m_ui->langComboBox->insertItem(0, "System language");
    // the combo is [System language] + locales in map order; the stored value
    // for index 0 is "system". Unknown stored values fall back to en_US.
    QStringList langValues = langs.keys(); // sorted, "system" included
    langValues.removeAll("system");
    langValues.prepend("system"); // combo index 0 is the System entry
    m_combos = {{m_ui->langComboBox, &K_Language, langValues, QStringLiteral("en_US")}};

    connect(this, &SettingsDialog::settingsChanged, settings, &Settings::sendChangeNotification);
    readSettings();

    adjustSizeToContents();
}
//------------------------------------------------------------------------------
SettingsDialog::~SettingsDialog() {
    delete m_ui;
}
//------------------------------------------------------------------------------
// an attempt to force minimum width to fit contents
void SettingsDialog::adjustSizeToContents() {
    // general tab
    m_ui->gridLayout->activate();
    m_ui->horizontalLayout_28->activate();
    m_ui->horizontalLayout_19->activate();
    m_ui->gridLayout_3->activate();
    m_ui->horizontalLayout_18->activate();
    m_ui->gridLayout_4->activate();
    m_ui->horizontalLayout_24->activate();
    m_ui->gridLayout_5->activate();
    m_ui->slideshowGroupContents->activate();
    m_ui->scrollAreaWidgetContents->layout()->activate();
    m_ui->scrollArea->setMinimumWidth(m_ui->scrollAreaWidgetContents->minimumSizeHint().width());
    // view tab
    m_ui->horizontalLayout_29->activate();
    m_ui->horizontalLayout_31->activate();
    m_ui->widget->layout()->activate();
    m_ui->scrollAreaWidgetContents_3->layout()->activate();
    m_ui->scrollArea_3->setMinimumWidth(m_ui->scrollAreaWidgetContents_3->minimumSizeHint().width());
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
    for(const CheckBinding &b : std::as_const(m_checks))
        b.box->setChecked((settings->*b.get)());
    for(const ComboBinding &b : std::as_const(m_combos)) {
        int idx = b.values.indexOf(settings->load<QString>(*b.desc));
        if(idx < 0)
            idx = b.values.indexOf(b.fallback);
        b.box->setCurrentIndex(idx);
    }
    m_ui->loopSlideshowCheckBox->setChecked(settings->loopSlideshow());
    m_ui->videoPlaybackCheckBox->setChecked(settings->videoPlayback());
    m_ui->videoPlaybackGroupContents->setEnabled(settings->videoPlayback());
    m_ui->playSoundsCheckBox->setChecked(settings->playVideoSounds());
    m_ui->enablePanelCheckBox->setChecked(settings->panelEnabled());
    m_ui->thumbnailPanelGroupContents->setEnabled(settings->panelEnabled());
    m_ui->panelFullscreenOnlyCheckBox->setChecked(settings->panelFullscreenOnly());
    m_ui->squareThumbnailsCheckBox->setChecked(settings->squareThumbnails());
    m_ui->transparencyGridCheckBox->setChecked(settings->transparencyGrid());
    m_ui->enableSmoothScrollCheckBox->setChecked(settings->enableSmoothScroll());
    m_ui->usePreloaderCheckBox->setChecked(settings->usePreloader());
    m_ui->useThumbnailCacheCheckBox->setChecked(settings->useThumbnailCache());
    m_ui->smoothUpscalingCheckBox->setChecked(settings->smoothUpscaling());
    m_ui->expandImageCheckBox->setChecked(settings->expandImage());
    m_ui->expandImagesGroupContents->setEnabled(settings->expandImage());
    m_ui->smoothAnimatedImagesCheckBox->setChecked(settings->smoothAnimatedImages());
    m_ui->bgOpacitySlider->setValue(static_cast<int>(settings->backgroundOpacity() * 100));
    m_ui->blurBackgroundCheckBox->setChecked(settings->blurBackground());
    m_ui->sortingComboBox->setCurrentIndex(settings->sortingMode());
    m_ui->confirmTrashCheckBox->setChecked(settings->confirmTrash());
    m_ui->unlockMinZoomCheckBox->setChecked(settings->unlockMinZoom());
    m_ui->trackpadDetectionCheckBox->setChecked(settings->trackpadDetection());
    m_ui->clickableEdgesCheckBox->setChecked(settings->clickableEdges());
    m_ui->clickableEdgesVisibleCheckBox->setChecked(settings->clickableEdgesVisible());
    m_ui->clickableEdgesVisibleCheckBox->setEnabled(settings->clickableEdges());
    m_ui->showHiddenFilesCheckBox->setChecked(settings->showHiddenFiles());

    if(settings->zoomIndicatorMode() == INDICATOR_ENABLED)
        m_ui->zoomIndicatorOn->setChecked(true);
    else if(settings->zoomIndicatorMode() == INDICATOR_AUTO)
        m_ui->zoomIndicatorAuto->setChecked(true);
    else
        m_ui->zoomIndicatorOff->setChecked(true);
    m_ui->showInfoBarFullscreen->setChecked(settings->infoBarFullscreen());
    m_ui->showInfoBarWindowed->setChecked(settings->infoBarWindowed());
    m_ui->showExtendedInfoTitle->setChecked(settings->windowTitleExtendedInfo());
    m_ui->cursorAutohideCheckBox->setChecked(settings->cursorAutohide());
    if(settings->focusPointIn1to1Mode() == FOCUS_TOP)
        m_ui->focus1to1Top->setChecked(true);
    else if(settings->focusPointIn1to1Mode() == FOCUS_CENTER)
        m_ui->focus1to1Center->setChecked(true);
    else
        m_ui->focus1to1Cursor->setChecked(true);
    m_ui->slideshowIntervalSpinBox->setValue(settings->slideshowInterval());
    m_ui->imageScrollingComboBox->setCurrentIndex(settings->imageScrolling());
    m_ui->saveOverlayCheckBox->setChecked(settings->showSaveOverlay());
    m_ui->unloadThumbsCheckBox->setChecked(settings->unloadThumbs());
    if(settings->thumbPanelStyle() == TH_PANEL_SIMPLE)
        m_ui->thumbStyleSimple->setChecked(true);
    else
        m_ui->thumbStyleExtended->setChecked(true);
    m_ui->animatedJxlCheckBox->setChecked(settings->jxlAnimation());
    m_ui->autoResizeWindowCheckBox->setChecked(settings->autoResizeWindow());
    m_ui->panelCenterSelectionCheckBox->setChecked(settings->panelCenterSelection());
    m_ui->useFixedZoomLevelsCheckBox->setChecked(settings->useFixedZoomLevels());
    m_ui->zoomLevels->setText(settings->zoomLevels());

    if(settings->defaultViewMode() == MODE_FOLDERVIEW)
        m_ui->startInFolderViewCheckBox->setChecked(true);
    else
        m_ui->startInFolderViewCheckBox->setChecked(false);

    if(settings->folderEndAction() == FOLDER_END_NO_ACTION)
        m_ui->folderEndNoAction->setChecked(true);
    else if(settings->folderEndAction() == FOLDER_END_LOOP)
        m_ui->folderEndLoop->setChecked(true);
    else
        m_ui->folderEndSwitchFolder->setChecked(true);

    m_ui->mpvLineEdit->setText(settings->mpvBinary());
    m_ui->quickCopyLineEdit->setText(settings->quickCopyDirectory());

    m_ui->zoomStepSlider->setValue(static_cast<int>(settings->zoomStep() * 100.f));
    onZoomStepSliderChanged(m_ui->zoomStepSlider->value());

    m_ui->mouseScrollingSpeedSlider->setValue(static_cast<int>((settings->mouseScrollingSpeed() - 0.5f) / 0.25f));
    onMouseScrollingSpeedSliderChanged(m_ui->mouseScrollingSpeedSlider->value());

    m_ui->autoResizeLimitSlider->setValue(static_cast<int>(settings->autoResizeLimit() / 5.f));
    onAutoResizeLimitSliderChanged(m_ui->autoResizeLimitSlider->value());

    m_ui->JPEGQualitySlider->setValue(settings->JPEGSaveQuality());
    onJPEGQualitySliderChanged(m_ui->JPEGQualitySlider->value());

    m_ui->expandLimitSlider->setValue(settings->expandLimit());
    onExpandLimitSliderChanged(m_ui->expandLimitSlider->value());

    m_ui->memoryLimitSpinBox->setValue(settings->memoryAllocationLimit());

    // ##### fit mode #####
    if(settings->imageFitMode() == FIT_WINDOW)
        m_ui->fitModeWindow->setChecked(true);
    else if(settings->imageFitMode() == FIT_WIDTH)
        m_ui->fitModeWidth->setChecked(true);
    else
        m_ui->fitMode1to1->setChecked(true);

    // ##### UI #####
    m_ui->scalingQualityComboBox->setCurrentIndex(settings->scalingFilter());
    m_ui->fullscreenCheckBox->setChecked(settings->fullscreenMode());
    m_ui->pinPanelCheckBox->setChecked(settings->panelPinned());
    m_ui->panelPositionComboBox->setCurrentIndex(settings->panelPosition());

    // reduce by 10x to have nice granular control in qslider
    m_ui->panelSizeSlider->setValue(settings->panelPreviewsSize() / 10);

    m_ui->useSystemColorsCheckBox->setChecked(settings->useSystemColorScheme());
    m_ui->modifySystemSchemeLabel->setVisible(settings->useSystemColorScheme());
    m_ui->themeSelectorComboBox->setEnabled(!settings->useSystemColorScheme());
    m_ui->colorConfigSubgroup->setEnabled(!settings->useSystemColorScheme());

    readColorScheme();
    readShortcuts();
    readScripts();
}
//------------------------------------------------------------------------------
void SettingsDialog::saveSettings() {
    // background work is waited on where the new settings are consumed:
    // Core::readSettings() drains the shared worker pool before re-reading
    for(const CheckBinding &b : std::as_const(m_checks))
        (settings->*b.set)(b.box->isChecked());
    for(const ComboBinding &b : std::as_const(m_combos)) {
        int idx = b.box->currentIndex();
        if(idx >= 0 && idx < b.values.size())
            settings->store(*b.desc, b.values.at(idx));
    }

    settings->setLoopSlideshow(m_ui->loopSlideshowCheckBox->isChecked());
    settings->setFullscreenMode(m_ui->fullscreenCheckBox->isChecked());
    if(m_ui->fitModeWindow->isChecked())
        settings->setImageFitMode(FIT_WINDOW);
    else if(m_ui->fitModeWidth->isChecked())
        settings->setImageFitMode(FIT_WIDTH);
    else
        settings->setImageFitMode(FIT_ORIGINAL);

    settings->setVideoPlayback(m_ui->videoPlaybackCheckBox->isChecked());
    settings->setPlayVideoSounds(m_ui->playSoundsCheckBox->isChecked());
    settings->setPanelEnabled(m_ui->enablePanelCheckBox->isChecked());
    settings->setPanelFullscreenOnly(m_ui->panelFullscreenOnlyCheckBox->isChecked());
    settings->setSquareThumbnails(m_ui->squareThumbnailsCheckBox->isChecked());
    settings->setTransparencyGrid(m_ui->transparencyGridCheckBox->isChecked());
    settings->setShowHiddenFiles(m_ui->showHiddenFilesCheckBox->isChecked());
    settings->setEnableSmoothScroll(m_ui->enableSmoothScrollCheckBox->isChecked());
    settings->setUsePreloader(m_ui->usePreloaderCheckBox->isChecked());
    settings->setUseThumbnailCache(m_ui->useThumbnailCacheCheckBox->isChecked());
    settings->setSmoothUpscaling(m_ui->smoothUpscalingCheckBox->isChecked());
    settings->setExpandImage(m_ui->expandImageCheckBox->isChecked());
    settings->setSmoothAnimatedImages(m_ui->smoothAnimatedImagesCheckBox->isChecked());

    settings->setBackgroundOpacity(static_cast<qreal>(m_ui->bgOpacitySlider->value()) / 100.f);
    settings->setBlurBackground(m_ui->blurBackgroundCheckBox->isChecked());
    settings->setSortingMode(static_cast<SortingMode>(m_ui->sortingComboBox->currentIndex()));
    settings->setConfirmTrash(m_ui->confirmTrashCheckBox->isChecked());
    settings->setUnlockMinZoom(m_ui->unlockMinZoomCheckBox->isChecked());
    settings->setTrackpadDetection(m_ui->trackpadDetectionCheckBox->isChecked());
    settings->setClickableEdges(m_ui->clickableEdgesCheckBox->isChecked());
    settings->setClickableEdgesVisible(m_ui->clickableEdgesVisibleCheckBox->isChecked());

    if(m_ui->zoomIndicatorOn->isChecked())
        settings->setZoomIndicatorMode(INDICATOR_ENABLED);
    else if(m_ui->zoomIndicatorAuto->isChecked())
        settings->setZoomIndicatorMode(INDICATOR_AUTO);
    else
        settings->setZoomIndicatorMode(INDICATOR_DISABLED);
    settings->setInfoBarFullscreen(m_ui->showInfoBarFullscreen->isChecked());
    settings->setInfoBarWindowed(m_ui->showInfoBarWindowed->isChecked());
    settings->setWindowTitleExtendedInfo(m_ui->showExtendedInfoTitle->isChecked());
    settings->setCursorAutohide(m_ui->cursorAutohideCheckBox->isChecked());
    if(m_ui->focus1to1Top->isChecked())
        settings->setFocusPointIn1to1Mode(FOCUS_TOP);
    else if(m_ui->focus1to1Center->isChecked())
        settings->setFocusPointIn1to1Mode(FOCUS_CENTER);
    else
        settings->setFocusPointIn1to1Mode(FOCUS_CURSOR);

    settings->setSlideshowInterval(m_ui->slideshowIntervalSpinBox->value());

    if(m_ui->startInFolderViewCheckBox->isChecked())
        settings->setDefaultViewMode(MODE_FOLDERVIEW);
    else
        settings->setDefaultViewMode(MODE_DOCUMENT);

    if(m_ui->folderEndNoAction->isChecked())
        settings->setFolderEndAction(FOLDER_END_NO_ACTION);
    else if(m_ui->folderEndLoop->isChecked())
        settings->setFolderEndAction(FOLDER_END_LOOP);
    else
        settings->setFolderEndAction(FOLDER_END_GOTO_ADJACENT);

    settings->setMpvBinary(m_ui->mpvLineEdit->text());
    settings->setQuickCopyDirectory(m_ui->quickCopyLineEdit->text());
    settings->setScalingFilter(static_cast<ScalingFilter>(m_ui->scalingQualityComboBox->currentIndex()));
    settings->setImageScrolling(static_cast<ImageScrolling>(m_ui->imageScrollingComboBox->currentIndex()));
    settings->setShowSaveOverlay(m_ui->saveOverlayCheckBox->isChecked());
    settings->setUnloadThumbs(m_ui->unloadThumbsCheckBox->isChecked());
    if(m_ui->thumbStyleSimple->isChecked())
        settings->setThumbPanelStyle(TH_PANEL_SIMPLE);
    else
        settings->setThumbPanelStyle(TH_PANEL_EXTENDED);
    settings->setJxlAnimation(m_ui->animatedJxlCheckBox->isChecked());
    settings->setAutoResizeWindow(m_ui->autoResizeWindowCheckBox->isChecked());
    settings->setPanelCenterSelection(m_ui->panelCenterSelectionCheckBox->isChecked());
    settings->setUseFixedZoomLevels(m_ui->useFixedZoomLevelsCheckBox->isChecked());
    settings->setZoomLevels(m_ui->zoomLevels->text());

    settings->setPanelPinned(m_ui->pinPanelCheckBox->isChecked());
    int panelPos = m_ui->panelPositionComboBox->currentIndex();
    settings->setPanelPosition(static_cast<PanelPosition>(panelPos));

    settings->setPanelPreviewsSize(m_ui->panelSizeSlider->value() * 10);

    settings->setJPEGSaveQuality(m_ui->JPEGQualitySlider->value());
    settings->setZoomStep(static_cast<qreal>(m_ui->zoomStepSlider->value() / 100.f));
    settings->setMouseScrollingSpeed(static_cast<qreal>(0.5f + (m_ui->mouseScrollingSpeedSlider->value() * 0.25f)));
    settings->setAutoResizeLimit(m_ui->autoResizeLimitSlider->value() * 5);
    settings->setExpandLimit(m_ui->expandLimitSlider->value());
    settings->setMemoryAllocationLimit(m_ui->memoryLimitSpinBox->value());

    settings->setUseSystemColorScheme(m_ui->useSystemColorsCheckBox->isChecked());

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
    m_ui->zoomLevels->setText(settings->defaultZoomLevels());
}
//------------------------------------------------------------------------------
void SettingsDialog::selectMpvPath() {
    QFileDialog dialog;
    QString file;
    file = dialog.getOpenFileName(this, tr("Navigate to mpv binary"), "", "mpv*");
    if(!file.isEmpty()) {
        m_ui->mpvLineEdit->setText(file);
    }
}
//------------------------------------------------------------------------------
void SettingsDialog::onExpandLimitSliderChanged(int value) {
    if(value == 0)
        m_ui->expandLimitLabel->setText("-");
    else
        m_ui->expandLimitLabel->setText(QString::number(value) + "x");
}
//------------------------------------------------------------------------------
void SettingsDialog::onJPEGQualitySliderChanged(int value) {
    m_ui->JPEGQualityLabel->setText(QString::number(value) + "%");
}
//------------------------------------------------------------------------------
void SettingsDialog::onZoomStepSliderChanged(int value) {
    m_ui->zoomStepLabel->setText(QString::number(value / 100.f, 'f', 2) + "x");
}
//------------------------------------------------------------------------------
void SettingsDialog::onMouseScrollingSpeedSliderChanged(int value) {
    m_ui->mouseScrollingSpeedLabel->setText(QString::number(0.5f + (value * 0.25f), 'f', 2) + "x");
}
//------------------------------------------------------------------------------
void SettingsDialog::onBgOpacitySliderChanged(int value) {
    m_ui->bgOpacityPercentLabel->setText(QString::number(value) + "%");
}
//------------------------------------------------------------------------------
void SettingsDialog::onAutoResizeLimitSliderChanged(int value) {
    m_ui->autoResizeLimit->setText(QString::number(value * 5.f, 'f', 0) + "%");
}
//------------------------------------------------------------------------------
int SettingsDialog::exec() {
    return QDialog::exec();
}

void SettingsDialog::switchToPage(int number) {
    m_ui->sideBar2->selectEntry(number);
}
