#include "core.h"
#include "mainwindow.h"
#include "gui/windowshell.h"
#include "gui/overlayhost.h"
#include "gui/dialoghost.h"
#include "gui/cropcontroller.h"

MW::MW(State *state, QWidget *parent) : FloatingWidgetContainer(parent), appState(state), infoBarFullscreen(nullptr) {
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
    windowShell = new WindowShell(this);
    setupUi();
    // the keymap's inspect scope follows the viewer's fit mode: fit original
    // / free zoom declare "I am inspecting this image" — the arrows pan and
    // never change images until the user leaves the mode
    fitModeObserver = viewerWidget->observeFitMode([this] {
        ImageFitMode mode = viewerWidget->fitMode();
        actionManager->setInspectMode(mode == FIT_ORIGINAL || mode == FIT_FREE);
    });
    overlayHost = new OverlayHost(this, viewerWidget, docWidget);
    cropController = new CropController(this, viewerWidget, sidePanel, docWidget);
    dialogHost = new DialogHost(this, docWidget);
    connect(viewerWidget, &ViewerWidget::showScriptSettings, dialogHost, &DialogHost::showScriptSettings);

    connect(settings, &Settings::settingsChanged, this, &MW::readSettings);
    connect(windowShell, &WindowShell::fullscreenStateChanged, this, &MW::adaptToWindowState);

    readSettings();
    windowShell->restoreWindowGeometry();

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
    // infoBarFullscreen is a non-QObject proxy and ordinary C++ member of MW:
    // nothing else deletes it, and its deleteLater dtor only matters while an
    // event loop is alive — the inner overlays themselves are Qt-parented and
    // go with the tree sweep right after this. The message/info proxies live
    // in OverlayHost now.
    delete infoBarFullscreen;
}

void MW::setupUi() {
    viewerWidget = new ViewerWidget(this);
    infoBarWindowed = new InfoBarProxy(this);
    docWidget = new DocumentWidget(viewerWidget, infoBarWindowed);
    folderView = new FolderViewProxy(this);
    connect(folderView, &FolderViewProxy::sortingSelected, this, &MW::sortingSelected);
    connect(folderView, &FolderViewProxy::directorySelected, dialogHost, &DialogHost::opened);
    connect(folderView, &FolderViewProxy::copyUrlsRequested, this, &MW::copyUrlsRequested);
    connect(folderView, &FolderViewProxy::moveUrlsRequested, this, &MW::moveUrlsRequested);
    connect(folderView, &FolderViewProxy::showFoldersChanged, this, &MW::showFoldersChanged);

    centralWidget = new CentralWidget(docWidget, folderView, this);
    layout.addWidget(centralWidget);
    infoBarFullscreen = new FullscreenInfoOverlayProxy(viewerWidget);
    sidePanel = new SidePanel(this);
    layout.addWidget(sidePanel);
    connect(viewerWidget, &ViewerWidget::scalingRequested, this, &MW::scalingRequested);
    connect(viewerWidget, &ViewerWidget::draggedOut, this, qOverload<>(&MW::draggedOut));
    connect(viewerWidget, &ViewerWidget::playbackFinished, this, &MW::playbackFinished);
    connect(this, &MW::setLoopPlayback, viewerWidget, &ViewerWidget::setLoopPlayback);
}

void MW::setupFullUi() {
    cropController->setupCropPanel();
    docWidget->allowPanelInit();
    docWidget->setupMainPanel();
    infoBarWindowed->init();
    infoBarFullscreen->init();
}

void MW::enableFolderView() {
    cropController->hideCropPanel();
    overlayHost->hideTransientOverlays();
    docWidget->hideFloatingPanel();
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
        overlayHost->showMessage("Zoom temporary disabled");
    }
}

void MW::fitWidth() {
    if(viewerWidget->interactionEnabled()) {
        emit viewerWidget->fitWidth();
    } else {
        overlayHost->showMessage("Zoom temporary disabled");
    }
}

void MW::fitOriginal() {
    if(viewerWidget->interactionEnabled()) {
        emit viewerWidget->fitOriginal();
    } else {
        overlayHost->showMessage("Zoom temporary disabled");
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
void MW::showImage(std::unique_ptr<QPixmap> pixmap) {
    if(settings->autoResizeWindow())
        windowShell->preShowResize(pixmap->size());
    viewerWidget->showImage(std::move(pixmap));
    cropController->updateCropPanelData();
}

void MW::showAnimation(const std::shared_ptr<QMovie> &movie) {
    if(settings->autoResizeWindow())
        windowShell->preShowResize(movie->frameRect().size());
    viewerWidget->showAnimation(movie);
    cropController->updateCropPanelData();
}

void MW::showVideo(const QString &file) {
    if(settings->autoResizeWindow())
        windowShell->preShowResize(QSize()); // tmp. find a way to get this though mpv BEFORE playback
    viewerWidget->showVideo(file);
}

// viewer command one-liners: ActionManager's connects land here and each
// re-fires the same-named (or renamed) ViewerWidget signal directly.
void MW::zoomIn() {
    emit viewerWidget->zoomIn();
}

void MW::zoomOut() {
    emit viewerWidget->zoomOut();
}

void MW::zoomInCursor() {
    emit viewerWidget->zoomInCursor();
}

void MW::zoomOutCursor() {
    emit viewerWidget->zoomOutCursor();
}

void MW::scrollUp() {
    emit viewerWidget->scrollUp();
}

void MW::scrollDown() {
    emit viewerWidget->scrollDown();
}

void MW::scrollLeft() {
    emit viewerWidget->scrollLeft();
}

void MW::scrollRight() {
    emit viewerWidget->scrollRight();
}

void MW::pauseVideo() {
    viewerWidget->pauseResumePlayback();
}

void MW::seekVideoForward() {
    viewerWidget->seekForward();
}

void MW::seekVideoBackward() {
    viewerWidget->seekBackward();
}

void MW::frameStep() {
    viewerWidget->frameStep();
}

void MW::frameStepBack() {
    viewerWidget->frameStepBack();
}

void MW::toggleMute() {
    viewerWidget->toggleMute();
}

void MW::volumeUp() {
    viewerWidget->volumeUp();
}

void MW::volumeDown() {
    viewerWidget->volumeDown();
}

void MW::toggleTransparencyGrid() {
    emit viewerWidget->toggleTransparencyGrid();
}

void MW::showContextMenu() {
    viewerWidget->showContextMenu();
}

void MW::onSortingChanged(SortingMode mode) {
    folderView->onSortingChanged(mode);
    if(centralWidget->currentViewMode() == ViewMode::MODE_DOCUMENT) {
        switch(mode) {
        case SortingMode::SORT_NAME: overlayHost->showMessage("Sorting: By Name"); break;
        case SortingMode::SORT_NAME_DESC: overlayHost->showMessage("Sorting: By Name (desc.)"); break;
        case SortingMode::SORT_TIME: overlayHost->showMessage("Sorting: By Time"); break;
        case SortingMode::SORT_TIME_DESC: overlayHost->showMessage("Sorting: By Time (desc.)"); break;
        case SortingMode::SORT_SIZE: overlayHost->showMessage("Sorting: By File Size"); break;
        case SortingMode::SORT_SIZE_DESC: overlayHost->showMessage("Sorting: By File Size (desc.)"); break;
        }
    }
}

void MW::toggleLockZoom() {
    emit viewerWidget->toggleLockZoom();
    if(viewerWidget->lockZoomEnabled())
        overlayHost->showMessage("Zoom lock: ON");
    else
        overlayHost->showMessage("Zoom lock: OFF");
    refreshInfoViews();
}

void MW::toggleLockView() {
    emit viewerWidget->toggleLockView();
    if(viewerWidget->lockViewEnabled())
        overlayHost->showMessage("View lock: ON");
    else
        overlayHost->showMessage("View lock: OFF");
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

void MW::toggleScalingFilter() {
    ScalingFilter configuredFilter = settings->scalingFilter();
    if(viewerWidget->scalingFilter() == configuredFilter) {
        setFilterNearest();
    } else {
        setFilter(configuredFilter);
    }
}

void MW::setFilterNearest() {
    overlayHost->showMessage("Filter: nearest", 600);
    emit viewerWidget->setFilterNearest();
}

void MW::setFilterBilinear() {
    overlayHost->showMessage("Filter: bilinear", 600);
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
    overlayHost->showMessage("Filter " + filterName, 600);
    emit viewerWidget->setScalingFilter(filter);
}

void MW::onScalingFinished(std::unique_ptr<QPixmap> scaled) {
    viewerWidget->onScalingFinished(std::move(scaled));
}

//#############################################################
//######################### EVENTS ############################
//#############################################################

void MW::mouseMoveEvent(QMouseEvent *event) {
    event->ignore();
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
    windowShell->saveWindowGeometry();
    windowShell->saveCurrentDisplay();
    overlayHost->saveCopySettings();
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
    cropController->onWindowResized();
    FloatingWidgetContainer::resizeEvent(event);
}

DialogResult MW::fileReplaceDialog(const QString &src, const QString &dst, FileReplaceMode mode, bool multiple) {
    return dialogHost->fileReplaceDialog(src, dst, mode, multiple);
}

void MW::setInteractionEnabled(bool mode) {
    docWidget->setInteractionEnabled(mode);
    viewerWidget->setInteractionEnabled(mode);
}

// the view is a projection: everything shown is re-derived here from the
// state Core owns and this window's own modes — nothing is stored
void MW::refreshInfoViews() {
    const CurrentInfo &info = appState->currentInfo.value();
    QString states = titleStateSuffix(info.slideshow, info.shuffle, viewerWidget->lockZoomEnabled(),
                                      viewerWidget->lockViewEnabled());

    overlayHost->setRenameOverlayName(info.fileName);

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
CropController *MW::crop() {
    return cropController;
}

DialogHost *MW::dialogs() {
    return dialogHost;
}

OverlayHost *MW::overlays() {
    return overlayHost;
}

WindowShell *MW::shell() {
    return windowShell;
}

FolderViewProxy *MW::getFolderView() {
    return folderView;
}

ThumbnailStripProxy *MW::getThumbnailPanel() {
    return docWidget->thumbPanel();
}

// todo: this is crap
void MW::showMessage(const QString &text) {
    overlayHost->showMessage(text);
}

bool MW::showConfirmation(const QString &title, const QString &msg) {
    return dialogHost->showConfirmation(title, msg);
}

void MW::readSettings() {
    showInfoBarFullscreen = settings->infoBarFullscreen();
    showInfoBarWindowed = settings->infoBarWindowed();
    adaptToWindowState();
}

// changes ui elements according to fullscreen state
void MW::adaptToWindowState() {
    docWidget->hideFloatingPanel();
    if(isFullScreen()) { //-------------------------------------- fullscreen ---
        windowShell->applyFullscreenBackground();
        infoBarWindowed->hide();

        if(showInfoBarFullscreen)
            infoBarFullscreen->showWhenReady();
        else
            infoBarFullscreen->hide();

        overlayHost->updateControlsOverlay();
    } else { //------------------------------------------------------ window ---
        windowShell->applyWindowedBackground();
        infoBarFullscreen->hide();

        if(showInfoBarWindowed)
            infoBarWindowed->show();
        else
            infoBarWindowed->hide();

        overlayHost->hideControlsOverlay();
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
