#include "core.h"
#include "mainwindow.h"
#include "gui/windowshell.h"
#include "gui/overlayhost.h"
#include "gui/dialoghost.h"
#include "gui/crop/cropcontroller.h"

MW::MW(State *state, QWidget *parent)
    : FloatingWidgetContainer(parent), m_appState(state), m_infoBarFullscreen(nullptr) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    m_layout.setContentsMargins(0, 0, 0, 0);
    m_layout.setSpacing(0);

    setMinimumSize(10, 10);

    // do not steal focus when clicked
    // this is just a container. accept key events only
    // via passthrough from child widgets
    setFocusPolicy(Qt::NoFocus);

    this->setLayout(&m_layout);

    setWindowTitle(QGuiApplication::applicationDisplayName() + " " + QCoreApplication::applicationVersion());

    this->setMouseTracking(true);
    this->setAcceptDrops(true);
    this->setAccessibleName("mainwindow");
    m_windowShell = new WindowShell(this);
    Ui ui = createUi();
    adoptUi(ui);
    // the keymap's inspect scope follows the viewer's fit mode: fit original
    // / free zoom declare "I am inspecting this image" — the arrows pan and
    // never change images until the user leaves the mode
    m_fitModeObserver = m_viewerWidget->observeFitMode([this] {
        ImageFitMode mode = m_viewerWidget->fitMode();
        actionManager->setInspectMode(mode == FIT_ORIGINAL || mode == FIT_FREE);
    });
    m_overlayHost = new OverlayHost(this, m_viewerWidget, m_docWidget);
    m_cropController = new CropController(this, state, m_viewerWidget, m_sidePanel, m_docWidget);
    connect(m_viewerWidget, &ViewerWidget::showScriptSettings, m_dialogHost, &DialogHost::showScriptSettings);

    connect(settings, &Settings::settingsChanged, this, &MW::readSettings);
    connect(m_windowShell, &WindowShell::fullscreenStateChanged, this, &MW::adaptToWindowState);

    readSettings();
    m_windowShell->restoreWindowGeometry();

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
    m_infoObserver = state->currentInfo.addNotifier([this] {
        refreshInfoViews();
    });
    m_slideshowObserver = state->slideshow.addNotifier([this] {
        // the player request left for the Core seam (view_->setLoopPlayback);
        // this binding keeps only the window's own projection
        refreshInfoViews();
    });
    m_shuffleObserver = state->shuffle.addNotifier([this] {
        refreshInfoViews();
    });
    m_directoryObserver = state->directoryPath.addNotifier([this] {
        m_folderView->setDirectoryPath(m_appState->directoryPath.value());
        refreshInfoViews();
    });
    // the mode dance: the view switch and its surface cleanup, run by the
    // window on the state's change. The slideshow loop relay rides its own
    // binding above.
    m_viewModeObserver = state->viewMode.addNotifier([this, state = m_appState] {
        const bool folder = state->viewMode.value() == MODE_FOLDERVIEW;
        // the keymap's document scope follows the same state the view does:
        // one truth, two consumers
        actionManager->setDocumentActive(!folder);
        folder ? enableFolderView() : enableDocumentView();
    });
    // The seam census, the same shape Core's constructor carries and for the
    // same reason: this constructor is a forty-line hand-written dependency
    // sequence, and nothing but the census checks that every step produced
    // what the next one assumes. It exists because one did not — DialogHost
    // was created after the connect that handed it out, which bound a null
    // receiver and left the folder view's home and root buttons inert for the
    // life of the feature (01e704f3's era; named from a startup warning nobody
    // read). qFatal, not Q_ASSERT: the Release build this tree ships must also
    // fail loudly.
    //
    // This catches the "forgot to create it" half only. Sequencing and
    // wiring-the-wrong-object are still the author's judgment — see the
    // create/wire split this constructor is a step toward.
    if(!m_windowShell || !m_viewerWidget || !m_infoBarWindowed || !m_docWidget || !m_dialogHost || !m_folderView ||
       !m_centralWidget || !m_infoBarFullscreen || !m_sidePanel || !m_overlayHost || !m_cropController)
        qFatal("MW construction left a child null — the census checks windowShell, viewerWidget, "
               "infoBarWindowed, docWidget, dialogHost, folderView, centralWidget, infoBarFullscreen, "
               "sidePanel, overlayHost and cropController");
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
    delete m_infoBarFullscreen;
}

MW::Ui MW::createUi() {
    // Everything this window owns, in the one order the dependencies allow:
    // the creation order used to be interleaved with the wiring across forty
    // lines of the constructor, and that is where DialogHost came to be created
    // after the connect that handed it out.
    //
    // What the return value buys is correctness under this tree's contract,
    // not a hard error: an omitted field is -Wmissing-field-initializers (in
    // -Wextra, and the build holds zero warnings), and a dependency written in
    // the wrong order names a local that does not exist yet, which IS a hard
    // error. Passing an explicit nullptr to one of them still compiles, and
    // that is what the constructor's runtime census remains for.
    auto *uiViewer = new ViewerWidget(this);
    auto *uiInfoBarWindowed = new InfoBarProxy(this);
    auto *uiDoc = new DocumentWidget(uiViewer, uiInfoBarWindowed);
    auto *uiDialog = new DialogHost(this, uiDoc);
    auto *uiFolderView = new FolderViewProxy(this);
    auto *uiCentral = new CentralWidget(uiDoc, uiFolderView, this);
    auto *uiInfoBarFullscreen = new FullscreenInfoOverlayProxy(uiViewer);
    auto *uiSidePanel = new SidePanel(this);

    // the wires that only need what this function built. The ones that
    // involve `this` as a signal's receiver and `uiDialog`/`uiViewer` as senders
    // live here too — they are part of the same graph.
    connect(uiFolderView, &FolderViewProxy::sortingSelected, this, &MW::sortingSelected);
    connect(uiFolderView, &FolderViewProxy::directorySelected, uiDialog, &DialogHost::opened);
    connect(uiFolderView, &FolderViewProxy::copyUrlsRequested, this, &MW::copyUrlsRequested);
    connect(uiFolderView, &FolderViewProxy::moveUrlsRequested, this, &MW::moveUrlsRequested);
    connect(uiFolderView, &FolderViewProxy::showFoldersChanged, this, &MW::showFoldersChanged);
    connect(uiViewer, &ViewerWidget::scalingRequested, this, &MW::scalingRequested);
    connect(uiViewer, &ViewerWidget::draggedOut, this, qOverload<>(&MW::draggedOut));
    connect(uiViewer, &ViewerWidget::playbackFinished, this, &MW::playbackFinished);
    connect(this, &MW::setLoopPlayback, uiViewer, &ViewerWidget::setLoopPlayback);
    // the heavy panels ride the window's own first show; queued, so the setup
    // runs after the show and layout pass have completed
    connect(this, &MW::shownOnce, this, &MW::setupFullUi, Qt::QueuedConnection);
    // every field named at one site, in dependency order
    return Ui{uiViewer, uiInfoBarWindowed, uiDoc, uiDialog, uiFolderView, uiCentral, uiInfoBarFullscreen, uiSidePanel};
}

void MW::adoptUi(const Ui &ui) {
    // the single place the members are filled. The parent tree owns every
    // widget; these observe (see the rule comment in the header).
    m_viewerWidget = ui.viewer;
    m_infoBarWindowed = ui.infoBarWindowed;
    m_docWidget = ui.doc;
    m_dialogHost = ui.dialog;
    m_folderView = ui.folderView;
    m_centralWidget = ui.central;
    m_infoBarFullscreen = ui.infoBarFullscreen;
    m_sidePanel = ui.sidePanel;

    m_layout.addWidget(m_centralWidget);
    m_layout.addWidget(m_sidePanel);
    // the document view's keys are its own, not the global table's: they ride
    // QShortcuts parented here, active exactly while this subtree is visible
    // and focused (CentralWidget shows page 0 and focuses the focusProxy, so
    // "visible" and "focusable" are the same condition).
    actionManager->attachDocumentSurface(m_docWidget);
    // the notifier below only fires on CHANGE; the initial mode is a state
    // too, and the resolver needs to agree with it from the first keystroke
    actionManager->setDocumentActive(m_appState->viewMode.value() != MODE_FOLDERVIEW);
}

// The heavy-UI trigger: fired exactly once, from the first show. The flag
// keeps it at one fire even though setupFullUi is idempotent by its guards.
void MW::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if(!m_shownOnceFired) {
        m_shownOnceFired = true;
        emit shownOnce();
    }
}

void MW::setupFullUi() {
    m_cropController->setupCropPanel();
    m_docWidget->allowPanelInit();
    m_docWidget->setupMainPanel();
    m_infoBarWindowed->init();
    m_infoBarFullscreen->init();
}

void MW::enableFolderView() {
    m_cropController->hideCropPanel();
    m_overlayHost->hideTransientOverlays();
    m_docWidget->hideFloatingPanel();
    m_centralWidget->showFolderView();
    refreshInfoViews();
}

void MW::enableDocumentView() {
    m_centralWidget->showDocumentView();
    refreshInfoViews();
}

ViewMode MW::currentViewMode() {
    return m_centralWidget->currentViewMode();
}

void MW::fitWindow() {
    if(m_viewerWidget->interactionEnabled()) {
        viewer()->fitWindow();
    } else {
        m_overlayHost->showMessage("Zoom temporary disabled");
    }
}

void MW::fitWidth() {
    if(m_viewerWidget->interactionEnabled()) {
        viewer()->fitWidth();
    } else {
        m_overlayHost->showMessage("Zoom temporary disabled");
    }
}

void MW::fitOriginal() {
    if(m_viewerWidget->interactionEnabled()) {
        viewer()->fitOriginal();
    } else {
        m_overlayHost->showMessage("Zoom temporary disabled");
    }
}

// switch between 1:1 and Fit All
// TODO: move to viewerWidget?
void MW::switchFitMode() {
    // Space in video mode is play/pause — the context check used to live in
    // ViewerWidget::keyPressEvent, an input side door; the action handler
    // owns the context now
    if(m_viewerWidget->currentWidgetType() == VIDEOPLAYER) {
        m_viewerWidget->toggleVideoPlayback();
        return;
    }
    if(m_viewerWidget->fitMode() == FIT_WINDOW)
        m_viewerWidget->setFitMode(FIT_ORIGINAL);
    else
        m_viewerWidget->setFitMode(FIT_WINDOW);
}

// Shift+Space: cycle Window -> Width -> Original -> Window
void MW::cycleFitMode() {
    if(m_viewerWidget->fitMode() == FIT_WINDOW)
        m_viewerWidget->setFitMode(FIT_WIDTH);
    else if(m_viewerWidget->fitMode() == FIT_WIDTH)
        m_viewerWidget->setFitMode(FIT_ORIGINAL);
    else
        m_viewerWidget->setFitMode(FIT_WINDOW);
}

void MW::closeImage() {
    m_viewerWidget->closeImage();
}

// todo: fix flicker somehow
// ideally it should change img & resize in one go
void MW::showImage(std::shared_ptr<const QImage> image) {
    if(settings->autoResizeWindow())
        m_windowShell->preShowResize(image->size());
    auto pixmap = std::make_unique<QPixmap>(QPixmap::fromImage(*image));
    m_viewerWidget->showImage(std::move(pixmap));
    m_cropController->updateCropPanelData();
}

void MW::showAnimation(const std::shared_ptr<QMovie> &movie) {
    if(settings->autoResizeWindow())
        m_windowShell->preShowResize(movie->frameRect().size());
    m_viewerWidget->showAnimation(movie);
    m_cropController->updateCropPanelData();
}

void MW::showVideo(const QString &file) {
    if(settings->autoResizeWindow())
        m_windowShell->preShowResize(QSize()); // tmp. find a way to get this though mpv BEFORE playback
    m_viewerWidget->showVideo(file);
}

// viewer command one-liners: ActionManager's connects land here and each
// re-fires the same-named (or renamed) ViewerWidget signal directly.
void MW::showContextMenu() {
    m_viewerWidget->showContextMenu();
}

void MW::onSortingChanged(SortingMode mode) {
    m_folderView->onSortingChanged(mode);
    if(m_centralWidget->currentViewMode() == ViewMode::MODE_DOCUMENT) {
        switch(mode) {
        case SortingMode::SORT_NAME: m_overlayHost->showMessage("Sorting: By Name"); break;
        case SortingMode::SORT_NAME_DESC: m_overlayHost->showMessage("Sorting: By Name (desc.)"); break;
        case SortingMode::SORT_TIME: m_overlayHost->showMessage("Sorting: By Time"); break;
        case SortingMode::SORT_TIME_DESC: m_overlayHost->showMessage("Sorting: By Time (desc.)"); break;
        case SortingMode::SORT_SIZE: m_overlayHost->showMessage("Sorting: By File Size"); break;
        case SortingMode::SORT_SIZE_DESC: m_overlayHost->showMessage("Sorting: By File Size (desc.)"); break;
        }
    }
}

void MW::toggleLockZoom() {
    viewer()->toggleLockZoom();
    if(m_viewerWidget->lockZoomEnabled())
        m_overlayHost->showMessage("Zoom lock: ON");
    else
        m_overlayHost->showMessage("Zoom lock: OFF");
    refreshInfoViews();
}

void MW::toggleLockView() {
    viewer()->toggleLockView();
    if(m_viewerWidget->lockViewEnabled())
        m_overlayHost->showMessage("View lock: ON");
    else
        m_overlayHost->showMessage("View lock: OFF");
    refreshInfoViews();
}

void MW::toggleFullscreenInfoBar() {
    if(!this->isFullScreen())
        return;
    m_showInfoBarFullscreen = !m_showInfoBarFullscreen;
    if(m_showInfoBarFullscreen)
        m_infoBarFullscreen->showWhenReady();
    else
        m_infoBarFullscreen->hide();
}
// ICoreView adaptations of the QWidget bits the interface covers
void MW::repaintView() {
    repaint();
}

void MW::setUpdatesEnabled(bool enabled) {
    QWidget::setUpdatesEnabled(enabled);
}

void MW::showWindowDefault() {
    shell()->showDefault();
}

void MW::toggleScalingFilter() {
    ScalingFilter configuredFilter = settings->scalingFilter();
    if(m_viewerWidget->scalingFilter() == configuredFilter) {
        setFilterNearest();
    } else {
        setFilter(configuredFilter);
    }
}

void MW::setFilterNearest() {
    m_overlayHost->showMessage("Filter: nearest", 600);
    viewer()->setFilterNearest();
}

void MW::setFilterBilinear() {
    m_overlayHost->showMessage("Filter: bilinear", 600);
    viewer()->setFilterBilinear();
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
    m_overlayHost->showMessage("Filter " + filterName, 600);
    viewer()->setScalingFilter(filter);
}

void MW::onScalingFinished(std::unique_ptr<const QImage> scaled) {
    // the paint-device conversion happens here, on the GUI thread
    m_viewerWidget->onScalingFinished(std::make_unique<QPixmap>(QPixmap::fromImage(*scaled)));
}

//#############################################################
//######################### EVENTS ############################
//#############################################################

void MW::mouseMoveEvent(QMouseEvent *event) {
    event->ignore();
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
    m_windowShell->saveWindowGeometry();
    m_windowShell->saveCurrentDisplay();
    m_overlayHost->saveCopySettings();
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
    FloatingWidgetContainer::resizeEvent(event);
}

DialogResult MW::fileReplaceDialog(const QString &src, const QString &dst, FileReplaceMode mode, bool multiple) {
    return m_dialogHost->fileReplaceDialog(src, dst, mode, multiple);
}

// the view is a projection: everything shown is re-derived here from the
// state Core owns and this window's own modes — nothing is stored
void MW::refreshInfoViews() {
    const CurrentInfo &info = m_appState->currentInfo.value();
    QString states = titleStateSuffix(info.slideshow, info.shuffle, m_viewerWidget->lockZoomEnabled(),
                                      m_viewerWidget->lockViewEnabled());

    m_overlayHost->setRenameOverlayName(info.fileName);

    QString windowTitle = assembleWindowTitle(m_centralWidget->currentViewMode() == MODE_FOLDERVIEW, info,
                                              m_viewerWidget->lockZoomEnabled(), m_viewerWidget->lockViewEnabled(),
                                              settings->windowTitleExtendedInfo(), !settings->infoBarWindowed(),
                                              qApp->applicationName(), tr("Folder view"));

    if(m_centralWidget->currentViewMode() == MODE_FOLDERVIEW || info.fileName.isEmpty()) {
        m_infoBarFullscreen->setInfo("", tr("No file opened."), "");
        m_infoBarWindowed->setInfo("", tr("No file opened."), "");
    } else {
        QString pos, res, size;
        titleSegments(info, &pos, &res, &size);
        m_infoBarFullscreen->setInfo(pos, info.fileName + (info.edited ? "  *" : ""), res + "  " + size);
        m_infoBarWindowed->setInfo(pos, info.fileName + (info.edited ? "  *" : ""), res + "  " + size + states);
    }
    setWindowTitle(windowTitle);
}

// TODO!!! buffer this in mw
ViewerWidget *MW::viewer() {
    return m_viewerWidget;
}

CropController *MW::crop() {
    return m_cropController;
}

DialogHost *MW::dialogs() {
    return m_dialogHost;
}

OverlayHost *MW::overlays() {
    return m_overlayHost;
}

WindowShell *MW::shell() {
    return m_windowShell;
}

FolderViewProxy *MW::getFolderView() {
    return m_folderView;
}

ThumbnailStripProxy *MW::getThumbnailPanel() {
    return m_docWidget->thumbPanel();
}

// todo: this is crap

bool MW::showConfirmation(const QString &title, const QString &msg) {
    return m_dialogHost->showConfirmation(title, msg);
}

void MW::readSettings() {
    m_showInfoBarFullscreen = settings->infoBarFullscreen();
    m_showInfoBarWindowed = settings->infoBarWindowed();
    adaptToWindowState();
}

// changes ui elements according to fullscreen state
void MW::adaptToWindowState() {
    m_docWidget->hideFloatingPanel();
    if(isFullScreen()) { //-------------------------------------- fullscreen ---
        m_windowShell->applyFullscreenBackground();
        m_infoBarWindowed->hide();

        if(m_showInfoBarFullscreen)
            m_infoBarFullscreen->showWhenReady();
        else
            m_infoBarFullscreen->hide();

        m_overlayHost->updateControlsOverlay();
    } else { //------------------------------------------------------ window ---
        m_windowShell->applyWindowedBackground();
        m_infoBarFullscreen->hide();

        if(m_showInfoBarWindowed)
            m_infoBarWindowed->show();
        else
            m_infoBarWindowed->hide();

        m_overlayHost->hideControlsOverlay();
    }
    m_folderView->onFullscreenModeChanged(isFullScreen());
    m_docWidget->onFullscreenModeChanged(isFullScreen());
    m_viewerWidget->onFullscreenModeChanged(isFullScreen());
}

void MW::paintEvent(QPaintEvent *event) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    FloatingWidgetContainer::paintEvent(event);
}

void MW::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    m_docWidget->hideFloatingPanel(true);
}

// block native tab-switching so we can use it in shortcuts
//bool MW::focusNextPrevChild(bool) {
//    return false;
//}
