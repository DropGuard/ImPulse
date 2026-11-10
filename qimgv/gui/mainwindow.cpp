#include "core.h"
#include "mainwindow.h"

// TODO: nuke this and rewrite

MW::MW(State *state, QWidget *parent)
    : FloatingWidgetContainer(parent), appState(state), currentDisplay(0), maximized(false),
      activeSidePanel(SIDEPANEL_NONE), copyOverlay(nullptr), saveOverlay(nullptr), renameOverlay(nullptr),
      infoBarFullscreen(nullptr), imageInfoOverlay(nullptr), floatingMessage(nullptr), cropPanel(nullptr),
      cropOverlay(nullptr) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    layout.setContentsMargins(0, 0, 0, 0);
    layout.setSpacing(0);

    setMinimumSize(10, 10);

    // do not steal focus when clicked
    // this is just a container. accept key events only
    // via passthrough from child widgets
    setFocusPolicy(Qt::NoFocus);

    this->setLayout(&layout);

    setWindowTitle(QGuiApplication::applicationDisplayName() + " " + QCoreApplication::applicationVersion());

    this->setMouseTracking(true);
    this->setAcceptDrops(true);
    this->setAccessibleName("mainwindow");
    windowGeometryChangeTimer.setSingleShot(true);
    windowGeometryChangeTimer.setInterval(30);
    setupUi();

    connect(settings, &Settings::settingsChanged, this, &MW::readSettings);
    connect(&windowGeometryChangeTimer, &QTimer::timeout, this, &MW::onWindowGeometryChanged);
    connect(this, &MW::fullscreenStateChanged, this, &MW::adaptToWindowState);

    readSettings();
    currentDisplay = settings->lastDisplay();
    maximized = settings->maximizedWindow();
    restoreWindowGeometry();

    // the projection drives the views: any rewrite of the snapshot
    // re-derives title/info bars/overlay names (issue #505 semantics live
    // in onLoadFailed), and the folder view re-derives its directory the
    // same way. addNotifier, never onValueChanged: the latter returns
    // a temporary QPropertyChangeHandler whose functor lives inside the
    // handler object — moving it into a bare QPropertyObserver member leaves
    // the trampoline reading adjacent member bytes as the lambda, i.e. calling
    // refreshInfoViews with a tagged null as `this` (titleFollowsTheProjection
    // SIGSEGV, 18420f70 follow-up). Notifiers own their functor; they die with
    // this window, before the State they watch does. The wiring is the one
    // non-const touch of State — it goes through the constructor parameter,
    // because appState itself is stored const.
    infoObserver = state->currentInfo.addNotifier([this] {
        refreshInfoViews();
    });
    slideshowObserver = state->slideshow.addNotifier([this] {
        refreshInfoViews();
    });
    shuffleObserver = state->shuffle.addNotifier([this] {
        refreshInfoViews();
    });
    directoryObserver = state->directoryPath.addNotifier([this] {
        folderView->setDirectoryPath(appState->directoryPath.value());
        refreshInfoViews();
    });
}

/*                                                             |--[ImageViewer]
 *                        |--[DocumentWidget]--[ViewerWidget]--|
 * [MW]--[CentralWidget]--|                                    |--[VideoPlayer]
 *                        |--[FolderView]
 *
 *  (not counting floating widgets)
 *  ViewerWidget exists for input handling reasons (correct overlay hover handling)
 */
MW::~MW() {
    // the three non-QObject proxies are ordinary C++ members: nothing else
    // deletes them, and their deleteLater dtors only matter while an event
    // loop is alive — the inner overlays themselves are Qt-parented and go
    // with the tree sweep right after this
    delete infoBarFullscreen;
    delete imageInfoOverlay;
    delete floatingMessage;
}

void MW::setupUi() {
    viewerWidget = new ViewerWidget(this);
    infoBarWindowed = new InfoBarProxy(this);
    docWidget = new DocumentWidget(viewerWidget, infoBarWindowed);
    folderView = new FolderViewProxy(this);
    connect(folderView, &FolderViewProxy::sortingSelected, this, &MW::sortingSelected);
    connect(folderView, &FolderViewProxy::directorySelected, this, &MW::opened);
    connect(folderView, &FolderViewProxy::copyUrlsRequested, this, &MW::copyUrlsRequested);
    connect(folderView, &FolderViewProxy::moveUrlsRequested, this, &MW::moveUrlsRequested);
    connect(folderView, &FolderViewProxy::showFoldersChanged, this, &MW::showFoldersChanged);

    centralWidget = new CentralWidget(docWidget, folderView, this);
    layout.addWidget(centralWidget);
    controlsOverlay = new ControlsOverlay(docWidget);
    infoBarFullscreen = new FullscreenInfoOverlayProxy(viewerWidget);
    sidePanel = new SidePanel(this);
    layout.addWidget(sidePanel);
    imageInfoOverlay = new ImageInfoOverlayProxy(viewerWidget);
    floatingMessage = new FloatingMessageProxy(viewerWidget); // todo: use additional one for folderview?
    connect(viewerWidget, &ViewerWidget::scalingRequested, this, &MW::scalingRequested);
    connect(viewerWidget, &ViewerWidget::draggedOut, this, qOverload<>(&MW::draggedOut));
    connect(viewerWidget, &ViewerWidget::playbackFinished, this, &MW::playbackFinished);
    connect(viewerWidget, &ViewerWidget::showScriptSettings, this, &MW::showScriptSettings);
    connect(this, &MW::zoomIn, viewerWidget, &ViewerWidget::zoomIn);
    connect(this, &MW::zoomOut, viewerWidget, &ViewerWidget::zoomOut);
    connect(this, &MW::zoomInCursor, viewerWidget, &ViewerWidget::zoomInCursor);
    connect(this, &MW::zoomOutCursor, viewerWidget, &ViewerWidget::zoomOutCursor);
    connect(this, &MW::scrollUp, viewerWidget, &ViewerWidget::scrollUp);
    connect(this, &MW::scrollDown, viewerWidget, &ViewerWidget::scrollDown);
    connect(this, &MW::scrollLeft, viewerWidget, &ViewerWidget::scrollLeft);
    connect(this, &MW::scrollRight, viewerWidget, &ViewerWidget::scrollRight);
    connect(this, &MW::pauseVideo, viewerWidget, &ViewerWidget::pauseResumePlayback);
    connect(this, &MW::seekVideoForward, viewerWidget, &ViewerWidget::seekForward);
    connect(this, &MW::seekVideoBackward, viewerWidget, &ViewerWidget::seekBackward);
    connect(this, &MW::frameStep, viewerWidget, &ViewerWidget::frameStep);
    connect(this, &MW::frameStepBack, viewerWidget, &ViewerWidget::frameStepBack);
    connect(this, &MW::toggleMute, viewerWidget, &ViewerWidget::toggleMute);
    connect(this, &MW::volumeUp, viewerWidget, &ViewerWidget::volumeUp);
    connect(this, &MW::volumeDown, viewerWidget, &ViewerWidget::volumeDown);
    connect(this, &MW::toggleTransparencyGrid, viewerWidget, &ViewerWidget::toggleTransparencyGrid);
    connect(this, &MW::setLoopPlayback, viewerWidget, &ViewerWidget::setLoopPlayback);
}

void MW::setupFullUi() {
    setupCropPanel();
    docWidget->allowPanelInit();
    docWidget->setupMainPanel();
    infoBarWindowed->init();
    infoBarFullscreen->init();
}

void MW::setupCropPanel() {
    if(cropPanel)
        return;
    cropOverlay = new CropOverlay(viewerWidget);
    cropPanel = new CropPanel(cropOverlay, this);
    connect(cropPanel, &CropPanel::cancel, this, &MW::hideCropPanel);
    connect(cropPanel, &CropPanel::crop, this, &MW::hideCropPanel);
    connect(cropPanel, &CropPanel::crop, this, &MW::cropRequested);
    connect(cropPanel, &CropPanel::cropAndSave, this, &MW::hideCropPanel);
    connect(cropPanel, &CropPanel::cropAndSave, this, &MW::cropAndSaveRequested);
}

void MW::setupCopyOverlay() {
    copyOverlay = new CopyOverlay(viewerWidget);
    connect(copyOverlay, &CopyOverlay::copyRequested, this, &MW::copyRequested);
    connect(copyOverlay, &CopyOverlay::moveRequested, this, &MW::moveRequested);
}

void MW::setupSaveOverlay() {
    saveOverlay = new SaveConfirmOverlay(viewerWidget);
    connect(saveOverlay, &SaveConfirmOverlay::saveClicked, this, &MW::saveRequested);
    connect(saveOverlay, &SaveConfirmOverlay::saveAsClicked, this, &MW::saveAsClicked);
    connect(saveOverlay, &SaveConfirmOverlay::discardClicked, this, &MW::discardEditsRequested);
}

void MW::setupRenameOverlay() {
    renameOverlay = new RenameOverlay(this);
    renameOverlay->setName(QString());
    connect(renameOverlay, &RenameOverlay::renameRequested, this, &MW::renameRequested);
}

void MW::enableFolderView() {
    hideCropPanel();
    if(copyOverlay)
        copyOverlay->hide();
    if(renameOverlay)
        renameOverlay->hide();
    docWidget->hideFloatingPanel();
    imageInfoOverlay->hide();
    centralWidget->showFolderView();
    refreshInfoViews();
}

void MW::enableDocumentView() {
    centralWidget->showDocumentView();
    refreshInfoViews();
}

ViewMode MW::currentViewMode() {
    return centralWidget->currentViewMode();
}

void MW::fitWindow() {
    if(viewerWidget->interactionEnabled()) {
        emit viewerWidget->fitWindow();
    } else {
        showMessage("Zoom temporary disabled");
    }
}

void MW::fitWidth() {
    if(viewerWidget->interactionEnabled()) {
        emit viewerWidget->fitWidth();
    } else {
        showMessage("Zoom temporary disabled");
    }
}

void MW::fitOriginal() {
    if(viewerWidget->interactionEnabled()) {
        emit viewerWidget->fitOriginal();
    } else {
        showMessage("Zoom temporary disabled");
    }
}

void MW::fitWindowStretch() {
    if(viewerWidget->interactionEnabled()) {
        emit viewerWidget->fitWindowStretch();
    } else {
        showMessage("Zoom temporary disabled");
    }
}

// switch between 1:1 and Fit All
// TODO: move to viewerWidget?
void MW::switchFitMode() {
    // Space in video mode is play/pause — the context check used to live in
    // ViewerWidget::keyPressEvent, an input side door; the action handler
    // owns the context now
    if(viewerWidget->currentWidgetType() == VIDEOPLAYER) {
        viewerWidget->toggleVideoPlayback();
        return;
    }
    if(viewerWidget->fitMode() == FIT_WINDOW)
        viewerWidget->setFitMode(FIT_ORIGINAL);
    else
        viewerWidget->setFitMode(FIT_WINDOW);
}

// Shift+Space: cycle Window -> Width -> Original -> Window
void MW::cycleFitMode() {
    if(viewerWidget->fitMode() == FIT_WINDOW)
        viewerWidget->setFitMode(FIT_WIDTH);
    else if(viewerWidget->fitMode() == FIT_WIDTH)
        viewerWidget->setFitMode(FIT_ORIGINAL);
    else
        viewerWidget->setFitMode(FIT_WINDOW);
}

void MW::closeImage() {
    viewerWidget->closeImage();
}

// todo: fix flicker somehow
// ideally it should change img & resize in one go
void MW::preShowResize(QSize sz) {
    auto screens = qApp->screens();
    if(this->windowState() != Qt::WindowNoState || !screens.count() || screens.count() <= currentDisplay)
        return;
    int decorationSize = frameGeometry().height() - height();
    float maxSzMulti = settings->autoResizeLimit() / 100.f;
    QRect availableGeom = screens.at(currentDisplay)->availableGeometry();
    QSize maxSz = availableGeom.size() * maxSzMulti;
    maxSz.setHeight(maxSz.height() - decorationSize);
    if(!sz.isEmpty()) {
        if(sz.width() > maxSz.width() || sz.height() > maxSz.height())
            sz.scale(maxSz, Qt::KeepAspectRatio);
    } else {
        sz = maxSz;
    }
    QRect newGeom(0, 0, sz.width(), sz.height());
    newGeom.moveCenter(availableGeom.center());
    newGeom.translate(0, decorationSize / 2);

    if(this->isVisible())
        setGeometry(newGeom);
    else // setGeometry wont work on hidden windows, so we just save for it to be restored later
        settings->setWindowGeometry(newGeom);
    qApp->processEvents(); // not needed anymore with patched qt?
}

void MW::showImage(std::unique_ptr<QPixmap> pixmap) {
    if(settings->autoResizeWindow())
        preShowResize(pixmap->size());
    viewerWidget->showImage(std::move(pixmap));
    updateCropPanelData();
}

void MW::showAnimation(const std::shared_ptr<QMovie> &movie) {
    if(settings->autoResizeWindow())
        preShowResize(movie->frameRect().size());
    viewerWidget->showAnimation(movie);
    updateCropPanelData();
}

void MW::showVideo(const QString &file) {
    if(settings->autoResizeWindow())
        preShowResize(QSize()); // tmp. find a way to get this though mpv BEFORE playback
    viewerWidget->showVideo(file);
}

void MW::showContextMenu() {
    viewerWidget->showContextMenu();
}

void MW::onSortingChanged(SortingMode mode) {
    folderView->onSortingChanged(mode);
    if(centralWidget->currentViewMode() == ViewMode::MODE_DOCUMENT) {
        switch(mode) {
        case SortingMode::SORT_NAME: showMessage("Sorting: By Name"); break;
        case SortingMode::SORT_NAME_DESC: showMessage("Sorting: By Name (desc.)"); break;
        case SortingMode::SORT_TIME: showMessage("Sorting: By Time"); break;
        case SortingMode::SORT_TIME_DESC: showMessage("Sorting: By Time (desc.)"); break;
        case SortingMode::SORT_SIZE: showMessage("Sorting: By File Size"); break;
        case SortingMode::SORT_SIZE_DESC: showMessage("Sorting: By File Size (desc.)"); break;
        }
    }
}

void MW::toggleLockZoom() {
    emit viewerWidget->toggleLockZoom();
    if(viewerWidget->lockZoomEnabled())
        showMessage("Zoom lock: ON");
    else
        showMessage("Zoom lock: OFF");
    refreshInfoViews();
}

void MW::toggleLockView() {
    emit viewerWidget->toggleLockView();
    if(viewerWidget->lockViewEnabled())
        showMessage("View lock: ON");
    else
        showMessage("View lock: OFF");
    refreshInfoViews();
}

void MW::toggleFullscreenInfoBar() {
    if(!this->isFullScreen())
        return;
    showInfoBarFullscreen = !showInfoBarFullscreen;
    if(showInfoBarFullscreen)
        infoBarFullscreen->showWhenReady();
    else
        infoBarFullscreen->hide();
}

void MW::toggleImageInfoOverlay() {
    if(centralWidget->currentViewMode() == MODE_FOLDERVIEW)
        return;
    if(imageInfoOverlay->isHidden())
        imageInfoOverlay->show();
    else
        imageInfoOverlay->hide();
}

void MW::toggleRenameOverlay(const QString &currentName) {
    if(!renameOverlay)
        setupRenameOverlay();
    if(renameOverlay->isHidden()) {
        renameOverlay->setBackdropEnabled((centralWidget->currentViewMode() == MODE_FOLDERVIEW));
        renameOverlay->setName(currentName);
        renameOverlay->show();
    } else {
        renameOverlay->hide();
    }
}

void MW::toggleScalingFilter() {
    ScalingFilter configuredFilter = settings->scalingFilter();
    if(viewerWidget->scalingFilter() == configuredFilter) {
        setFilterNearest();
    } else {
        setFilter(configuredFilter);
    }
}

void MW::setFilterNearest() {
    showMessage("Filter: nearest", 600);
    emit viewerWidget->setFilterNearest();
}

void MW::setFilterBilinear() {
    showMessage("Filter: bilinear", 600);
    emit viewerWidget->setFilterBilinear();
}

void MW::setFilter(ScalingFilter filter) {
    QString filterName;
    switch(filter) {
    case QI_FILTER_NEAREST: filterName = "nearest"; break;
    case ScalingFilter::QI_FILTER_BILINEAR: filterName = "bilinear"; break;
    case QI_FILTER_CV_BILINEAR_SHARPEN: filterName = "bilinear + sharpen"; break;
    case QI_FILTER_CV_CUBIC: filterName = "bicubic"; break;
    case QI_FILTER_CV_CUBIC_SHARPEN: filterName = "bicubic + sharpen"; break;
    default: filterName = "configured " + QString::number(static_cast<int>(filter)); break;
    }
    showMessage("Filter " + filterName, 600);
    emit viewerWidget->setScalingFilter(filter);
}

bool MW::isCropPanelActive() {
    return (activeSidePanel == SIDEPANEL_CROP);
}

void MW::onScalingFinished(std::unique_ptr<QPixmap> scaled) {
    viewerWidget->onScalingFinished(std::move(scaled));
}

void MW::saveWindowGeometry() {
    if(this->windowState() == Qt::WindowNoState)
        settings->setWindowGeometry(geometry());
    settings->setMaximizedWindow(maximized);
}

// does not apply fullscreen; window size / maximized state only
void MW::restoreWindowGeometry() {
    this->setGeometry(settings->windowGeometry());
    if(settings->maximizedWindow())
        this->setWindowState(Qt::WindowMaximized);
    updateCurrentDisplay();
}

void MW::updateCurrentDisplay() {
    auto screens = qApp->screens();
    currentDisplay = screens.indexOf(this->window()->screen());
}

void MW::onWindowGeometryChanged() {
    saveWindowGeometry();
    updateCurrentDisplay();
}

void MW::saveCurrentDisplay() {
    settings->setLastDisplay(qApp->screens().indexOf(this->window()->screen()));
}

//#############################################################
//######################### EVENTS ############################
//#############################################################

void MW::mouseMoveEvent(QMouseEvent *event) {
    event->ignore();
}

bool MW::event(QEvent *event) {
    // only save maximized state if we are already visible
    // this filter out out events while the window is still being set up
    if(event->type() == QEvent::WindowStateChange && this->isVisible() && !this->isFullScreen())
        maximized = isMaximized();
    if(event->type() == QEvent::Move || event->type() == QEvent::Resize)
        windowGeometryChangeTimer.start();
    return QWidget::event(event);
}

// hook up to actionManager
void MW::keyPressEvent(QKeyEvent *event) {
    event->accept();
    actionManager->processEvent(event);
}

void MW::wheelEvent(QWheelEvent *event) {
    event->accept();
    actionManager->processEvent(event);
}

void MW::mousePressEvent(QMouseEvent *event) {
    event->accept();
    actionManager->processEvent(event);
}

void MW::mouseReleaseEvent(QMouseEvent *event) {
    event->accept();
    actionManager->processEvent(event);
}

void MW::mouseDoubleClickEvent(QMouseEvent *event) {
    event->accept();
    QMouseEvent *fakePressEvent = new QMouseEvent(QEvent::MouseButtonPress, event->pos(), event->globalPosition(),
                                                  event->button(), event->buttons(), event->modifiers());
    actionManager->processEvent(fakePressEvent);
    actionManager->processEvent(event);
}

void MW::close() {
    saveWindowGeometry();
    saveCurrentDisplay();
    if(copyOverlay)
        copyOverlay->saveSettings();
    QWidget::close();
}

void MW::closeEvent(QCloseEvent *event) {
    // catch the close event when user presses X on the window itself
    event->accept();
    actionManager->invokeAction("exit");
}

void MW::dragEnterEvent(QDragEnterEvent *e) {
    if(e->mimeData()->hasUrls()) {
        e->acceptProposedAction();
    }
}

void MW::dropEvent(QDropEvent *event) {
    emit droppedIn(event->mimeData(), event->source());
}

void MW::resizeEvent(QResizeEvent *event) {
    if(activeSidePanel == SIDEPANEL_CROP) {
        cropOverlay->setImageScale(viewerWidget->currentScale());
        cropOverlay->setImageDrawRect(viewerWidget->imageRect());
    }
    FloatingWidgetContainer::resizeEvent(event);
}

void MW::showDefault() {
    if(!this->isVisible()) {
        if(settings->fullscreenMode())
            showFullScreen();
        else
            showWindowed();
    }
}

void MW::showSaveDialog(const QString &filePath) {
    QString newFilePath = getSaveFileName(filePath);
    if(!newFilePath.isEmpty())
        emit saveAsRequested(newFilePath);
}

QString MW::getSaveFileName(const QString &filePath) {
    docWidget->hideFloatingPanel();
    QStringList filters;
    // generate filter for writable images
    // todo: some may need to be blacklisted
    auto writerFormats = QImageWriter::supportedImageFormats();
    if(writerFormats.contains("jpg"))
        filters.append("JPEG (*.jpg *.jpeg *jpe *jfif)");
    if(writerFormats.contains("png"))
        filters.append("PNG (*.png)");
    if(writerFormats.contains("webp"))
        filters.append("WebP (*.webp)");
    // may not work..
    if(writerFormats.contains("jp2"))
        filters.append("JPEG 2000 (*.jp2 *.j2k *.jpf *.jpx *.jpm *.jpgx)");
    if(writerFormats.contains("jxl"))
        filters.append("JPEG-XL (*.jxl)");
    if(writerFormats.contains("avif"))
        filters.append("AVIF (*.avif *.avifs)");
    if(writerFormats.contains("tif"))
        filters.append("TIFF (*.tif *.tiff)");
    if(writerFormats.contains("bmp"))
        filters.append("BMP (*.bmp)");
#ifdef _WIN32
    if(writerFormats.contains("ico"))
        filters.append("Icon Files (*.ico)");
#endif
    if(writerFormats.contains("ppm"))
        filters.append("PPM (*.ppm)");
    if(writerFormats.contains("xbm"))
        filters.append("XBM (*.xbm)");
    if(writerFormats.contains("xpm"))
        filters.append("XPM (*.xpm)");
    if(writerFormats.contains("dds"))
        filters.append("DDS (*.dds)");
    if(writerFormats.contains("wbmp"))
        filters.append("WBMP (*.wbmp)");
    // add everything else from imagewriter
    for(const auto &fmt : std::as_const(writerFormats)) {
        if(filters.filter(fmt).isEmpty())
            filters.append(fmt.toUpper() + " (*." + fmt + ")");
    }
    QString filterString = filters.join(";; ");

    // find matching filter for the current image
    QString selectedFilter = "JPEG (*.jpg *.jpeg *jpe *jfif)";
    QFileInfo fi(filePath);
    for(const QString &filter : std::as_const(filters)) {
        if(filter.contains(fi.suffix().toLower())) {
            selectedFilter = filter;
            break;
        }
    }
    QString newFilePath =
        QFileDialog::getSaveFileName(this, tr("Save File as..."), filePath, filterString, &selectedFilter);
    return newFilePath;
}

void MW::showOpenDialog(const QString &path) {
    docWidget->hideFloatingPanel();

    QFileDialog dialog(this);
    QStringList imageFilter;
    imageFilter.append(settings->supportedFormatsFilter());
    imageFilter.append("All Files (*)");
    dialog.setDirectory(path);
    dialog.setNameFilters(imageFilter);
    dialog.setWindowTitle("Open image");
    dialog.setWindowModality(Qt::ApplicationModal);
    connect(&dialog, &QFileDialog::fileSelected, this, &MW::opened);
    dialog.exec();
}

void MW::showResizeDialog(QSize initialSize) {
    ResizeDialog dialog(initialSize, this);
    connect(&dialog, &ResizeDialog::sizeSelected, this, &MW::resizeRequested);
    dialog.exec();
}

DialogResult MW::fileReplaceDialog(const QString &src, const QString &dst, FileReplaceMode mode, bool multiple) {
    FileReplaceDialog dialog(this);
    dialog.setModal(true);
    dialog.setSource(src);
    dialog.setDestination(dst);
    dialog.setMode(mode);
    dialog.setMulti(multiple);

    dialog.exec();

    return dialog.getResult();
}

void MW::showSettings() {
    docWidget->hideFloatingPanel();
    SettingsDialog settingsDialog(this);
    settingsDialog.exec();
}

void MW::showScriptSettings() {
    docWidget->hideFloatingPanel();
    SettingsDialog settingsDialog(this);
    settingsDialog.switchToPage(4);
    settingsDialog.exec();
}

void MW::triggerFullScreen() {
    if(!isFullScreen()) {
        showFullScreen();
    } else {
        showWindowed();
    }
}

void MW::showFullScreen() {
    //do not save immediately on application start
    if(!isHidden())
        saveWindowGeometry();
    auto screens = qApp->screens();
    // todo: why check the screen again?
    int _currentDisplay = screens.indexOf(this->window()->screen());
    //move to target screen
    if(screens.count() > currentDisplay && currentDisplay != _currentDisplay) {
        this->move(screens.at(currentDisplay)->geometry().x(), screens.at(currentDisplay)->geometry().y());
    }
    QWidget::showFullScreen();
    // try to repaint sooner
    qApp->processEvents();
    emit fullscreenStateChanged(true);
}

void MW::showWindowed() {
    if(isFullScreen())
        QWidget::showNormal();
    restoreWindowGeometry();
    QWidget::show();
    // try to repaint sooner
    qApp->processEvents();
    emit fullscreenStateChanged(false);
}

void MW::updateCropPanelData() {
    if(cropPanel && activeSidePanel == SIDEPANEL_CROP) {
        cropPanel->setImageRealSize(viewerWidget->sourceSize());
        cropOverlay->setImageDrawRect(viewerWidget->imageRect());
        cropOverlay->setImageScale(viewerWidget->currentScale());
        cropOverlay->setImageRealSize(viewerWidget->sourceSize());
    }
}

void MW::showSaveOverlay() {
    if(!settings->showSaveOverlay())
        return;
    if(!saveOverlay)
        setupSaveOverlay();
    saveOverlay->show();
}

void MW::hideSaveOverlay() {
    if(!saveOverlay)
        return;
    saveOverlay->hide();
}

void MW::triggerCropPanel() {
    if(activeSidePanel != SIDEPANEL_CROP) {
        showCropPanel();
    } else {
        hideCropPanel();
    }
}

void MW::showCropPanel() {
    if(centralWidget->currentViewMode() == MODE_FOLDERVIEW)
        return;

    // the crop wiring is live from initActions, but the panel is built in
    // setupFullUi — which Core schedules 50ms after showGui. An early trigger
    // used to dereference the null pair (cropPanelSurvivesAnEarlyTrigger).
    if(!cropPanel)
        setupCropPanel();

    if(activeSidePanel != SIDEPANEL_CROP) {
        docWidget->hideFloatingPanel();
        sidePanel->setWidget(cropPanel);
        sidePanel->show();
        cropOverlay->show();
        activeSidePanel = SIDEPANEL_CROP;
        // reset & lock zoom so CropOverlay won't go crazy
        emit viewerWidget->fitWindow();
        setInteractionEnabled(false);
        // feed the panel current image info
        updateCropPanelData();
    }
}

void MW::setInteractionEnabled(bool mode) {
    docWidget->setInteractionEnabled(mode);
    viewerWidget->setInteractionEnabled(mode);
}

void MW::hideCropPanel() {
    sidePanel->hide();
    if(activeSidePanel == SIDEPANEL_CROP) {
        cropOverlay->hide();
        setInteractionEnabled(true);
    }
    activeSidePanel = SIDEPANEL_NONE;
}

void MW::triggerCopyOverlay() {
    if(!viewerWidget->isDisplaying())
        return;
    if(!copyOverlay)
        setupCopyOverlay();

    if(centralWidget->currentViewMode() == MODE_FOLDERVIEW)
        return;
    if(copyOverlay->operationMode() == OVERLAY_COPY) {
        copyOverlay->isHidden() ? copyOverlay->show() : copyOverlay->hide();
    } else {
        copyOverlay->setDialogMode(OVERLAY_COPY);
        copyOverlay->show();
    }
}

void MW::triggerMoveOverlay() {
    if(!viewerWidget->isDisplaying())
        return;
    if(!copyOverlay)
        setupCopyOverlay();

    if(centralWidget->currentViewMode() == MODE_FOLDERVIEW)
        return;
    if(copyOverlay->operationMode() == OVERLAY_MOVE) {
        copyOverlay->isHidden() ? copyOverlay->show() : copyOverlay->hide();
    } else {
        copyOverlay->setDialogMode(OVERLAY_MOVE);
        copyOverlay->show();
    }
}

// quit fullscreen or exit the program
void MW::closeFullScreenOrExit() {
    if(this->isFullScreen()) {
        this->showWindowed();
    } else {
        actionManager->invokeAction("exit");
    }
}

// the view is a projection: everything shown is re-derived here from the
// state Core owns and this window's own modes — nothing is stored
void MW::refreshInfoViews() {
    const CurrentInfo &info = appState->currentInfo.value();
    QString states = titleStateSuffix(info.slideshow, info.shuffle, viewerWidget->lockZoomEnabled(),
                                      viewerWidget->lockViewEnabled());

    if(renameOverlay)
        renameOverlay->setName(info.fileName);

    QString windowTitle =
        assembleWindowTitle(centralWidget->currentViewMode() == MODE_FOLDERVIEW, info, viewerWidget->lockZoomEnabled(),
                            viewerWidget->lockViewEnabled(), settings->windowTitleExtendedInfo(),
                            !settings->infoBarWindowed(), qApp->applicationName(), tr("Folder view"));

    if(centralWidget->currentViewMode() == MODE_FOLDERVIEW || info.fileName.isEmpty()) {
        infoBarFullscreen->setInfo("", tr("No file opened."), "");
        infoBarWindowed->setInfo("", tr("No file opened."), "");
    } else {
        QString pos, res, size;
        titleSegments(info, &pos, &res, &size);
        infoBarFullscreen->setInfo(pos, info.fileName + (info.edited ? "  *" : ""), res + "  " + size);
        infoBarWindowed->setInfo(pos, info.fileName + (info.edited ? "  *" : ""), res + "  " + size + states);
    }
    setWindowTitle(windowTitle);
}

// TODO!!! buffer this in mw
void MW::setExifInfo(const QMap<QString, QString> &exifInfo) {
    if(imageInfoOverlay)
        imageInfoOverlay->setExifInfo(exifInfo);
}

FolderViewProxy *MW::getFolderView() {
    return folderView;
}

ThumbnailStripProxy *MW::getThumbnailPanel() {
    return docWidget->thumbPanel();
}

// todo: this is crap
void MW::showMessageDirectory(const QString &dirName) {
    floatingMessage->showMessage(dirName, FloatingMessageIcon::ICON_DIRECTORY, 1700);
}

void MW::showMessage(const QString &text) {
    floatingMessage->showMessage(text, FloatingMessageIcon::NO_ICON, 1500);
}

void MW::showMessage(const QString &text, int duration) {
    floatingMessage->showMessage(text, FloatingMessageIcon::NO_ICON, duration);
}

void MW::showMessageSuccess(const QString &text) {
    floatingMessage->showMessage(text, FloatingMessageIcon::ICON_SUCCESS, 1500);
}

void MW::showWarning(const QString &text) {
    floatingMessage->showMessage(text, FloatingMessageIcon::ICON_WARNING, 1500);
}

void MW::showError(const QString &text) {
    floatingMessage->showMessage(text, FloatingMessageIcon::ICON_ERROR, 2800);
}

bool MW::showConfirmation(const QString &title, const QString &msg) {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(title);
    msgBox.setText(msg);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Yes);
    msgBox.addButton(QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::Yes);
    msgBox.setModal(true);
    if(msgBox.exec() == QMessageBox::Yes)
        return true;
    else
        return false;
}

void MW::readSettings() {
    showInfoBarFullscreen = settings->infoBarFullscreen();
    showInfoBarWindowed = settings->infoBarWindowed();
    adaptToWindowState();
}

// todo: remove/rename?
void MW::applyWindowedBackground() {
#ifdef USE_KDE_BLUR
    QWindow *window = this->windowHandle();
    if(window) {
        if(settings->backgroundOpacity() == 1.0)
            KWindowEffects::enableBlurBehind(window, false);
        else
            KWindowEffects::enableBlurBehind(window, settings->blurBackground());
    }
#endif
}

void MW::applyFullscreenBackground() {
#ifdef USE_KDE_BLUR
    QWindow *window = this->windowHandle();
    if(window)
        KWindowEffects::enableBlurBehind(window, false);
#endif
}

// changes ui elements according to fullscreen state
void MW::adaptToWindowState() {
    docWidget->hideFloatingPanel();
    if(isFullScreen()) { //-------------------------------------- fullscreen ---
        applyFullscreenBackground();
        infoBarWindowed->hide();

        if(showInfoBarFullscreen)
            infoBarFullscreen->showWhenReady();
        else
            infoBarFullscreen->hide();

        auto pos = settings->panelPosition();
        if(!settings->panelEnabled() || pos == PANEL_BOTTOM || pos == PANEL_LEFT)
            controlsOverlay->show();
        else
            controlsOverlay->hide();
    } else { //------------------------------------------------------ window ---
        applyWindowedBackground();
        infoBarFullscreen->hide();

        if(showInfoBarWindowed)
            infoBarWindowed->show();
        else
            infoBarWindowed->hide();

        controlsOverlay->hide();
    }
    folderView->onFullscreenModeChanged(isFullScreen());
    docWidget->onFullscreenModeChanged(isFullScreen());
    viewerWidget->onFullscreenModeChanged(isFullScreen());
}

void MW::paintEvent(QPaintEvent *event) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    FloatingWidgetContainer::paintEvent(event);
}

void MW::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    docWidget->hideFloatingPanel(true);
}

// block native tab-switching so we can use it in shortcuts
//bool MW::focusNextPrevChild(bool) {
//    return false;
//}
