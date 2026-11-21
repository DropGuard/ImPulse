/*
 * This is sort-of a main controller of application.
 * It creates and initializes all components, then sets up gui and actions.
 * Most of communication between components go through here.
 *
 */

#include "core.h"
#include <utility>

#include "utils/folderend.h"

#include "gui/windowshell.h"
#include "gui/overlayhost.h"
#include "gui/dialoghost.h"
#include "gui/crop/cropcontroller.h"

#include <QThread>
#include <QDrag>
#include <QTemporaryFile>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

#ifdef _WIN32
#include <tchar.h>
#endif

// the shared pool's size: decode, scale and thumbnails all run here, so the
// pool follows the hardware. Never below 2, so a stuck mpv frame grab cannot
// starve decoding.
static int workerPoolThreadCount() {
    return qMax(2, QThread::idealThreadCount());
}

Core::Core() : Core(Deps{}) {
}

Core::Core(const Deps &deps)
    : QObject(),
      // A borrowed pool is used as-is; otherwise Core owns one sized the way the
      // app always sized it. The unique_ptr is declared before the reference so
      // the member it points at outlives the reference's users.
      m_ownedPool(deps.pool ? nullptr : std::make_unique<WorkerPool>(workerPoolThreadCount())),
      m_pool(deps.pool ? *deps.pool : *m_ownedPool), m_bridge(deps.bridge ? *deps.bridge : EngineBridge::instance()),
      m_thumbnailer(m_pool), m_thumbPanelPresenter(m_thumbnailer), m_folderViewPresenter(m_thumbnailer) {
    loadTranslation();
    if(deps.headless) {
        // no window tree at all: the three seams are the whole surface. Their
        // completeness is guarded by the seam census at the end of this
        // constructor — deliberately not by an assert, which compiles out in
        // the Release build.
        m_ui = deps.ui;
        m_overlays = deps.overlays;
        m_view = deps.view;
        m_ownedDialogs = std::make_unique<DialogHost>(nullptr, nullptr);
        m_dialogs = m_ownedDialogs.get();
        m_crop = deps.crop; // optional: only crop-flow tests provide one
    } else {
        initGui();
        // the main window owns the dialogs and the interaction interface, so it
        // is the production provider of both; tests replace these pointers with
        // scripted fakes
        m_ui = m_mw;
        m_view = deps.view ? deps.view : m_mw;
        m_overlays = deps.overlays ? deps.overlays : m_mw->overlays();
        m_dialogs = m_mw->dialogs();
        m_crop = deps.crop ? deps.crop : m_mw->crop();
    }
    // the navigation object: both branches have set the seams it drives, so
    // it is constructed here, once — the listing arrives later, via attachModel
    m_nav = new NavigationController(&m_state, this, m_view, m_overlays, m_thumbPanelPresenter, m_folderViewPresenter,
                                     this);
    // the seam census — deliberately NOT Q_ASSERT, which compiles out of the
    // Release build this tree ships. A seam wired in one construction branch
    // only (nav_, in its day) must stay loud in every build shape, and the
    // message must say which seam set to inspect. crop_ is the documented
    // optional (crop-flow tests provide it); everything else is mandatory.
    if(!m_ui || !m_view || !m_overlays || !m_dialogs || !m_nav)
        qFatal("Core construction left a seam null (the census checks ui_, view_, overlays_, dialogs_, nav_) — "
               "every seam must be wired in both construction branches");
    // the slideshow loop is a pure function of the state: slides advance by
    // timer, normal viewing loops. The relay rides the state binding and goes
    // through the view seam — the window's notifier keeps only the info
    // projection, this one owns the player request, so a headless Core
    // reaches the seam exactly the way production does.
    m_slideshowRelay = m_state.slideshow.addNotifier([this] {
        m_view->setLoopPlayback(!m_state.slideshow.value());
    });
    initComponents();
    connectComponents();
    initActions();
    readSettings();
    connect(settings, &Settings::settingsChanged, this, &Core::readSettings);

    QVersionNumber lastVersion = settings->lastVersion();
    if(settings->firstRun())
        onFirstRun();
    // 'different', not 'greater': the version line restarted at 0.1.0 when the
    // fork took its own name, so an upgrade from the inherited qimgv numbers
    // (0.9.x, 1.0.x) compares as a DOWNGRADE — and 'greater' would never fire
    // again. The adjustment machinery is keyed on lastVer and idempotent per
    // step, which is what makes any direction safe to run it in.
    else if(appVersion != lastVersion)
        onUpdate();
}

Core::~Core() {
    // the window tree is Core's to tear down — the single-owner rule puts the
    // top-level window here, and this delete is the first full destruction in
    // the app's history (until the ownership unification, MW was never freed
    // and every Core instance leaked one). Presenters hold raw views into the
    // tree but run no code after this point; their own destruction follows as
    // ordinary member teardown.
    delete m_mw;
    m_mw = nullptr;
    // the translator lives on the application's translator list, so it has to
    // come off that list before it is freed — and it had no parent and no
    // destructor, so until now nothing ever freed it
    if(m_translator) {
        qApp->removeTranslator(m_translator);
        delete m_translator;
        m_translator = nullptr;
    }
}

void Core::readSettings() {
    // What this method re-reads is five navigation settings; the only one that
    // reaches an executor is folderViewMode, whose setShowDirs() repopulates
    // the view and makes it request thumbnails again. So the wait is scoped to
    // the Thumbnailer: Loader and Scaler jobs carry their own policy inside
    // them (LoadPolicy, ScalerRequest — the rule R4 exists to enforce), so
    // none of these five settings could change what an in-flight decode or
    // scale produces, and waiting for them was pure UI-thread cost bounded by
    // the whole queue. Queued thumbnail work is not even awaited in the steady
    // path: the regenerated batch starts with Thumbnailer::clearTasks(), which
    // drains it. What remains here is the running job, so a thumbnail callback
    // cannot land mid-repopulation.
    m_pool.waitForOwner(PoolOwner::Thumbnailer);
    m_nav->applySlideshowSettings(settings->slideshowInterval(), settings->loopSlideshow());
    m_nav->setFolderEndAction(settings->folderEndAction());
    bool showDirs = (settings->folderViewMode() == FV_EXT_FOLDERS);
    if(m_folderViewPresenter.showDirs() != showDirs)
        m_folderViewPresenter.setShowDirs(showDirs);
    if(m_state.shuffle)
        m_nav->syncRandomizer();
}

// The keymap's document scope follows the view mode. Navigation writes the
// state and the window renders it; the keymap is a third consumer, and this is
// where the coordinator hands it the same fact — headless Cores included,
// which is why it cannot live in MW's notifier.
void Core::onViewModeChanged(ViewMode mode) {
    actionManager->setDocumentActive(mode != MODE_FOLDERVIEW);
}

void Core::showGui() {
    if(!m_mw)
        return; // headless Core has nothing to show
    m_view->showWindowDefault();
    // the heavy-UI trigger is the window's own first show (shownOnce, wired
    // in the MW constructor) — the processEvents + 50 ms guess this used to
    // run is gone, along with the TODO that called it unreliable
}

// create MainWindow and all widgets
void Core::initGui() {
    m_mw = new MW(&m_state);
    m_mw->hide();
}

void Core::attachModel(DirectoryModel *_model) {
    m_model.reset(_model);
    m_thumbPanelPresenter.setModel(m_model);
    m_folderViewPresenter.setModel(m_model);
    bool showDirs = (settings->folderViewMode() == FV_EXT_FOLDERS);
    m_folderViewPresenter.setShowDirs(showDirs);
    m_nav->attachModel(_model);
}

void Core::initComponents() {
    attachModel(new DirectoryModel(m_pool, m_bridge));
}

void Core::connectComponents() {

    connect(&m_folderViewPresenter, &DirectoryPresenter::draggedOut, this,
            qOverload<const QList<QString> &>(&Core::onDraggedOut));

    connect(&m_folderViewPresenter, &DirectoryPresenter::droppedInto, this,
            qOverload<const QList<QString> &, const QString &>(&Core::movePathsTo));

    connect(scriptManager, &ScriptManager::error, m_overlays, &IOverlayHost::showError);

    connect(m_overlays, &IOverlayHost::copyRequested, this, &Core::copyCurrentFile);
    connect(m_overlays, &IOverlayHost::moveRequested, this, &Core::moveCurrentFile);
    connect(m_overlays, &IOverlayHost::saveAsClicked, this, &Core::requestSavePath);
    connect(m_overlays, &IOverlayHost::saveRequested, this, &Core::saveCurrentFile);
    connect(m_overlays, &IOverlayHost::renameRequested, this, &Core::renameCurrentSelection);
    connect(m_overlays, &IOverlayHost::discardEditsRequested, this, &Core::discardEdits);

    connect(m_model->scaler.get(), &Scaler::scalingFinished, this, &Core::onScalingFinished);

    if(m_mw)
        connectWindowComponents();
}

// The window-connected bindings and relays: view binding, dialog host signals, and the
// window->Core legs. Headless Core skips the whole method — a windowless
// process has no drops, no drags, no folder-view clicks, no keys.
void Core::connectWindowComponents() {
    connect(m_mw, &MW::droppedIn, this, &Core::onDropIn);
    connect(m_mw, &MW::copyUrlsRequested, this, qOverload<const QList<QString> &, const QString &>(&Core::copyPathsTo));
    connect(m_mw, &MW::moveUrlsRequested, this, &Core::movePathsTo);
    connect(m_mw, &MW::sortingSelected, m_nav, &NavigationController::sortBy);
    connect(m_mw, &MW::showFoldersChanged, m_nav, &NavigationController::setFoldersDisplay);
    connect(m_mw, &MW::draggedOut, this, qOverload<>(&Core::onDraggedOut));
    connect(m_mw, &MW::playbackFinished, m_nav, &NavigationController::onPlaybackFinished);
    connect(m_mw, &MW::scalingRequested, this, &Core::scalingRequest);
    m_thumbPanelPresenter.setView(m_mw->getThumbnailPanel());
    m_folderViewPresenter.setView(m_mw->getFolderView());
    connect(m_mw->dialogs(), &DialogHost::opened, m_nav, &NavigationController::loadPath);
    connect(m_mw->crop(), &CropController::cropRequested, this, &Core::crop);
    connect(m_mw->crop(), &CropController::cropAndSaveRequested, this, &Core::cropAndSave);
    connect(m_mw->dialogs(), &DialogHost::saveAsRequested, this, &Core::saveCurrentFileAs);
    connect(m_mw->dialogs(), &DialogHost::resizeRequested, this, &Core::resize);
}

void Core::initActions() {
    actionManager->connectAction("nextImage", m_nav, &NavigationController::nextImage);
    actionManager->connectAction("prevImage", m_nav, &NavigationController::prevImage);
    actionManager->connectAction("resize", this, &Core::showResizeDialog);
    actionManager->connectAction("flipH", this, &Core::flipH);
    actionManager->connectAction("flipV", this, &Core::flipV);
    actionManager->connectAction("rotateLeft", this, &Core::rotateLeft);
    actionManager->connectAction("rotateRight", this, &Core::rotateRight);
    actionManager->connectAction("crop", this, &Core::toggleCropPanel);
    actionManager->connectAction("setWallpaper", this, &Core::setWallpaper);
    actionManager->connectAction("open", this, &Core::showOpenDialog);
    actionManager->connectAction("save", this, &Core::saveCurrentFile);
    actionManager->connectAction("saveAs", this, &Core::requestSavePath);
    actionManager->connectAction("exit", this, &Core::close);
    actionManager->connectAction("removeFile", this, &Core::removePermanent);
    actionManager->connectAction("moveToTrash", this, &Core::moveToTrash);
    actionManager->connectAction("copyFile", m_overlays, &IOverlayHost::triggerCopyOverlay);
    actionManager->connectAction("quickCopy", this, &Core::quickCopyToFixedDirectory);
    actionManager->connectAction("moveFile", m_overlays, &IOverlayHost::triggerMoveOverlay);
    actionManager->connectAction("jumpToFirst", m_nav, &NavigationController::jumpToFirst);
    actionManager->connectAction("jumpToLast", m_nav, &NavigationController::jumpToLast);
    // script actions are dynamic — no registered QAction exists for an
    // "s:<name>" binding, so the runScript carrier signal stays a plain
    // connect (R12's documented exception)
    connect(actionManager, &ActionManager::runScript, this, &Core::runScript);
    actionManager->connectAction("folderView", m_nav, &NavigationController::enableFolderView);
    actionManager->connectAction("documentView", m_nav, &NavigationController::enableDocumentView);
    actionManager->connectAction("toggleFolderView", m_nav, &NavigationController::toggleFolderView);
    actionManager->connectAction("reloadImage", this, qOverload<>(&Core::reloadImage));
    actionManager->connectAction("copyFileClipboard", this, &Core::copyFileClipboard);
    actionManager->connectAction("copyPathClipboard", this, &Core::copyPathClipboard);
    actionManager->connectAction("renameFile", this, &Core::showRenameDialog);
    actionManager->connectAction("sortByName", m_nav, &NavigationController::sortByName);
    actionManager->connectAction("sortByTime", m_nav, &NavigationController::sortByTime);
    actionManager->connectAction("sortBySize", m_nav, &NavigationController::sortBySize);
    actionManager->connectAction("toggleImageInfo", m_overlays, &IOverlayHost::toggleImageInfoOverlay);
    actionManager->connectAction("toggleShuffle", m_nav, &NavigationController::toggleShuffle);
    actionManager->connectAction("showInDirectory", this, &Core::showInDirectory);
    actionManager->connectAction("toggleSlideshow", m_nav, &NavigationController::toggleSlideshow);
    actionManager->connectAction("goUp", m_nav, &NavigationController::loadParentDir);
    actionManager->connectAction("discardEdits", this, &Core::discardEdits);
    actionManager->connectAction("nextDirectory", m_nav, &NavigationController::nextDirectory);
    actionManager->connectAction("prevDirectory", m_nav, qOverload<>(&NavigationController::prevDirectory));
    actionManager->connectAction("print", this, &Core::print);
    actionManager->connectAction("toggleFullscreenInfoBar", this, &Core::toggleFullscreenInfoBar);
    actionManager->connectAction("pasteFile", this, &Core::openFromClipboard);
    // the folder grid's scope actions, landed on the proxy the same way the
    // viewer commands land on ViewerWidget
    if(m_mw)
        connectWindowActions();
}

// The window-connected actions, kept as one block: headless Core has no keys and skips the whole
// method — its tests call the slots directly. The connectAction calls stay
// in core.cpp because R12 reads this file's text for the connected list.
void Core::connectWindowActions() {
    // the projection's registration surface: a QAction's shortcuts are inert
    // until added to a widget (dispatch_probe_tests), so the whole registry
    // goes live on the main window here
    actionManager->attachWindowSurface(m_mw);
    actionManager->connectAction("fitWindow", m_mw->viewer(), &ViewerWidget::fitWindow);
    actionManager->connectAction("fitWidth", m_mw->viewer(), &ViewerWidget::fitWidth);
    actionManager->connectAction("fitNormal", m_mw->viewer(), &ViewerWidget::fitOriginal);
    actionManager->connectAction("nextFitMode", m_mw, &MW::cycleFitMode);
    actionManager->connectAction("toggleFitMode", m_mw, &MW::switchFitMode);
    actionManager->connectAction("toggleFullscreen", m_mw->shell(), &WindowShell::triggerFullScreen);
    actionManager->connectAction("lockZoom", m_mw->viewer(), &ViewerWidget::toggleLockZoom);
    actionManager->connectAction("lockView", m_mw->viewer(), &ViewerWidget::toggleLockView);
    actionManager->connectAction("zoomIn", m_mw->viewer(), &ViewerWidget::zoomIn);
    actionManager->connectAction("zoomOut", m_mw->viewer(), &ViewerWidget::zoomOut);
    actionManager->connectAction("zoomInCursor", m_mw->viewer(), &ViewerWidget::zoomInCursor);
    actionManager->connectAction("zoomOutCursor", m_mw->viewer(), &ViewerWidget::zoomOutCursor);
    actionManager->connectAction("scrollUp", m_mw->viewer(), &ViewerWidget::scrollUp);
    actionManager->connectAction("scrollDown", m_mw->viewer(), &ViewerWidget::scrollDown);
    actionManager->connectAction("scrollLeft", m_mw->viewer(), &ViewerWidget::scrollLeft);
    actionManager->connectAction("scrollRight", m_mw->viewer(), &ViewerWidget::scrollRight);
    actionManager->connectAction("openSettings", m_mw->dialogs(), &DialogHost::showSettings);
    actionManager->connectAction("closeFullScreenOrExit", m_mw->shell(), &WindowShell::closeFullScreenOrExit);
    actionManager->connectAction("pauseVideo", m_mw->viewer(), &ViewerWidget::pauseResumePlayback);
    actionManager->connectAction("seekVideoForward", m_mw->viewer(), &ViewerWidget::seekForward);
    actionManager->connectAction("seekVideoBackward", m_mw->viewer(), &ViewerWidget::seekBackward);
    actionManager->connectAction("frameStep", m_mw->viewer(), &ViewerWidget::frameStep);
    actionManager->connectAction("frameStepBack", m_mw->viewer(), &ViewerWidget::frameStepBack);
    actionManager->connectAction("contextMenu", m_mw, &MW::showContextMenu);
    actionManager->connectAction("toggleTransparencyGrid", m_mw->viewer(), &ViewerWidget::toggleTransparencyGrid);
    actionManager->connectAction("toggleScalingFilter", m_mw, &MW::toggleScalingFilter);
    actionManager->connectAction("toggleMute", m_mw->viewer(), &ViewerWidget::toggleMute);
    actionManager->connectAction("volumeUp", m_mw->viewer(), &ViewerWidget::volumeUp);
    actionManager->connectAction("volumeDown", m_mw->viewer(), &ViewerWidget::volumeDown);
    actionManager->connectAction("gridSelectNext", m_mw->getFolderView(), &FolderViewProxy::gridSelectNext);
    actionManager->connectAction("gridSelectPrev", m_mw->getFolderView(), &FolderViewProxy::gridSelectPrev);
    actionManager->connectAction("gridSelectAbove", m_mw->getFolderView(), &FolderViewProxy::gridSelectAbove);
    actionManager->connectAction("gridSelectBelow", m_mw->getFolderView(), &FolderViewProxy::gridSelectBelow);
    actionManager->connectAction("gridPageUp", m_mw->getFolderView(), &FolderViewProxy::gridPageUp);
    actionManager->connectAction("gridPageDown", m_mw->getFolderView(), &FolderViewProxy::gridPageDown);
    actionManager->connectAction("gridSelectFirst", m_mw->getFolderView(), &FolderViewProxy::gridSelectFirst);
    actionManager->connectAction("gridSelectLast", m_mw->getFolderView(), &FolderViewProxy::gridSelectLast);
    actionManager->connectAction("gridSelectAll", m_mw->getFolderView(), &FolderViewProxy::gridSelectAll);
    actionManager->connectAction("gridOpen", m_mw->getFolderView(), &FolderViewProxy::activateCurrent);
    // the folder view's Ctrl+V. Landed on Core, not the grid: the destination
    // is the model's directory and the work is interactiveCopy, both of which
    // the grid surface has no business owning.
    actionManager->connectAction("pasteFilesHere", this, &Core::pasteFilesToCurrentDirectory);
}

void Core::loadTranslation() {
    if(!m_translator)
        m_translator = new QTranslator;
    QString trPathFallback = QCoreApplication::applicationDirPath() + "/translations";
#ifdef TRANSLATIONS_PATH
    QString trPath = QString(TRANSLATIONS_PATH);
#else
    QString trPath = trPathFallback;
#endif
    QString localeName = settings->language();
    if(localeName == "system")
        localeName = QLocale::system().name();
    if(localeName.isEmpty() || localeName == "en_US") {
        QApplication::removeTranslator(m_translator);
        return;
    }
    QString trFile = trPath + "/" + localeName;
    QString trFileFallback = trPathFallback + "/" + localeName;
    if(!m_translator->load(trFile)) {
        qDebug() << "Could not load translation file: " << trFile;
        if(!m_translator->load(trFileFallback)) {
            qDebug() << "Could not load translation file: " << trFileFallback;
            return;
        }
    }
    QApplication::installTranslator(m_translator);
}

void Core::onUpdate() {

    qDebug() << "Updated: " << settings->lastVersion().toString() << ">" << appVersion.toString();
    // TODO: finish changelogs
    //if(settings->showChangelogs())
    //    mw->showChangelogWindow();
    m_overlays->showMessage(tr("Updated: ") + settings->lastVersion().toString() + " > " + appVersion.toString(), 4000);
    settings->setLastVersion(appVersion);
}

void Core::onFirstRun() {
    //mw->showSomeSortOfWelcomeScreen();
    m_overlays->showMessage(tr("Welcome to ") + qApp->applicationName() + tr(" version ") + appVersion.toString() + "!",
                            4000);
    settings->setFirstRun(false);
    settings->setLastVersion(appVersion);
}

void Core::close() {
    if(!m_mw)
        return; // headless: nothing to close
    m_mw->close();
}
// -----------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------

void Core::toggleFullscreenInfoBar() {
    m_view->toggleFullscreenInfoBar();
}

// ---------------------------------------------------------------- image operations

// ---------------------------------------------------------------- image operations ^

// todo: remove?
QString Core::selectedPath() {
    if(!m_model)
        return "";
    else if(m_state.viewMode.value() == MODE_FOLDERVIEW)
        return m_folderViewPresenter.selectedPaths().constLast();
    else
        return m_state.currentFilePath;
}

QList<QString> Core::currentSelection() {
    if(!m_model)
        return QList<QString>();
    else if(m_state.viewMode.value() == MODE_FOLDERVIEW)
        return m_folderViewPresenter.selectedPaths();
    else
        return QList<QString>() << m_state.currentFilePath;
}

//------------------------

void Core::runScript(const QString &scriptName) {
    if(m_model->listing().isEmpty())
        return;
    scriptManager->runScript(scriptName, m_model->getImage(selectedPath()));
}

// Linux implementation is KDE Plasma only (plasma-apply-wallpaperimage /
// qdbus) — deliberate fork scope, see the context menu entry.
// Pure dispatch: session -> program + arguments. An empty program means the
// session is unsupported. Arguments pass verbatim — QProcess takes them
// without a shell, so filenames with spaces or quotes survive intact.
QPair<QString, QStringList> Core::wallpaperCommand(const QString &session, const QString &path) {
    if(session.contains("plasma"))
        return {"plasma-apply-wallpaperimage", {path}};
    if(session.contains("gnome"))
        return {"gsettings", {"set", "org.gnome.desktop.background", "picture-uri", path}};
    return {};
}

void Core::setWallpaper() {
    if(m_model->listing().isEmpty() || selectedPath().isEmpty())
        return;
    auto img = m_model->getImage(selectedPath());
    if(img->type() != DocumentType::STATIC) {
        m_overlays->showMessage("Set wallpaper: file not supported");
        return;
    }
#ifdef _WIN32
    // set fit mode (registry)
    LONG status;
    HKEY hKey;
    status = RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Control Panel\\Desktop"), 0, KEY_WRITE, &hKey);
    if((status == ERROR_SUCCESS) && (hKey != NULL)) {
        LPCTSTR value = TEXT("WallpaperStyle");
        LPCTSTR data = TEXT("10");
        status = RegSetValueEx(hKey, value, 0, REG_SZ, (LPBYTE)data, _tcslen(data) + 1);
        RegCloseKey(hKey);
    }
    // set wallpaper path
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (char *)(selectedPath().toStdWString().c_str()),
                          SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
#else
    // detached, not ScriptManager::runCommand: the blocking variant froze the
    // UI thread for up to two seconds on a value nobody reads
    const QString session = qgetenv("DESKTOP_SESSION").toLower();
    auto [program, args] = wallpaperCommand(session, selectedPath());
    if(program.isEmpty()) {
        m_overlays->showMessage("Action is not supported in your desktop session (\"" + session + "\")", 3000);
        return;
    }
    QProcess::startDetached(program, args);
#endif
}

void Core::print() {
    if(!m_mw)
        return; // the print dialog parents to the window; headless has none
    if(m_model->listing().isEmpty())
        return;
    PrintDialog p(m_mw);
    auto img = m_model->getImage(selectedPath());
    if(!img) {
        m_overlays->showError(tr("Could not open image"));
        return;
    }
    if(img->type() != DocumentType::STATIC) {
        m_overlays->showError(tr("Can only print static images"));
        return;
    }
    QString pdfPath = m_model->listing().directoryPath() + "/" + img->baseName() + ".pdf";
    p.setImage(img->getImage());
    p.setOutputPath(pdfPath);
    p.exec();
}

void Core::scalingRequest(QSize size, ScalingFilter filter) {
    // visibility is the emitter's own timing now (requestScaling suppresses
    // while hidden and re-requests on show); what remains here is the state check
    if(m_state.hasActiveImage) {
        std::shared_ptr<Image> forScale = m_model->getImage(m_state.currentFilePath);
        if(forScale) {
            m_model->scaler->requestScaled(
                ScalerRequest(forScale, size, m_state.currentFilePath, filter, settings->smoothUpscaling()));
        }
    }
}

void Core::onScalingFinished(std::shared_ptr<const QImage> scaled, const ScalerRequest &req) {
    // Staleness is owned layer by layer, and each predicate here names its
    // own: existence — nothing is handed to a closed viewer; identity — a
    // result for any path but the one on screen is void (a switch cancels
    // the flight). The rest is deliberately NOT re-checked here: the scaler
    // serializes (one flight, newest-wins buffer), so ordering is never the
    // problem; zoom supersession belongs to the viewer, whose setScaledPixmap
    // drops any frame whose size no longer matches live zoom state — the only
    // guard able to compare against it. The old TODO asked for "a better fix";
    // the better fix turned out to already exist one layer down.
    //
    // The viewer takes a unique_ptr, so hand it one from a value instead of
    // passing a naked QPixmap* and hoping somebody deletes it.
    if(m_state.hasActiveImage && req.path == m_state.currentFilePath) {
        m_view->onScalingFinished(std::make_unique<const QImage>(*scaled));
    }
}

// reset state; clear cache; etc

QString Core::currentFilePath() const {
    return m_state.currentFilePath;
}

bool Core::firstFramePending() const {
    return m_state.delayModel;
}

bool Core::isSettled() const {
    return m_model && m_model->listing().isSettled() && !m_model->loaderBusy();
}

void Core::guiSetImage(std::shared_ptr<Image> img) {
    m_state.hasActiveImage = true;
    if(!img) {
        m_overlays->showMessage(tr("Error: could not load image."));
        return;
    }
    DocumentType type = img->type();
    if(type == STATIC) {
        m_view->showImage(img->getImage());
    } else if(type == ANIMATED) {
        auto animated = dynamic_cast<ImageAnimated *>(img.get());
        m_view->showAnimation(animated->getMovie());
    } else if(type == VIDEO) {
        auto video = dynamic_cast<Video *>(img.get());
        // workaround for mpv. If we play video while mainwindow is hidden we get black screen.
        // affects only initial startup (e.g. we open webm from file manager)
        showGui();
        m_view->showVideo(video->filePath());
    }
    img->isEdited() ? m_overlays->showSaveOverlay() : m_overlays->hideSaveOverlay();
    m_overlays->setExifInfo(img->getExifTags());
}

void Core::updateInfoString() {
    CurrentInfo ci;
    ci.index = m_model->listing().indexOfFile(m_state.currentFilePath);
    ci.fileCount = m_model->listing().fileCount();
    ci.fileName = m_model->listing().fileNameAt(ci.index);
    ci.filePath = m_model->listing().filePathAt(ci.index);
    ci.directoryPath = m_model->listing().directoryPath();
    ci.directoryName = ci.directoryPath.split("/").last();
    if(m_model->isLoaded(m_state.currentFilePath)) {
        auto img = m_model->getImage(m_state.currentFilePath);
        ci.imageSize = img->originalSize();
        ci.fileSize = img->fileSize();
        ci.edited = img->isEdited();
    }
    ci.slideshow = m_state.slideshow;
    ci.shuffle = m_state.shuffle;
    // the assignment is the notification: MW re-derives its views from the
    // projection on change (issue #505 semantics live in onLoadFailed)
    m_state.currentInfo = ci;
}
