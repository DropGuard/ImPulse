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

#ifdef __WIN32
#include <tchar.h>
#endif

// the shared pool's size: the old thumbnailerThreads knob now means "the
// pipeline's threads". Never below 2, so a stuck mpv frame grab cannot starve
// decoding; never above the hardware
static int workerPoolThreadCount() {
    const int hw = qMax(2, QThread::idealThreadCount());
    return qBound(2, settings->thumbnailerThreadCount(), hw);
}

Core::Core() : Core(Deps{}) {
}

Core::Core(const Deps &deps)
    : QObject(),
      // A borrowed pool is used as-is; otherwise Core owns one sized the way the
      // app always sized it. The unique_ptr is declared before the reference so
      // the member it points at outlives the reference's users.
      ownedPool(deps.pool ? nullptr : std::make_unique<WorkerPool>(workerPoolThreadCount())),
      pool(deps.pool ? *deps.pool : *ownedPool), mBridge(deps.bridge ? *deps.bridge : EngineBridge::instance()),
      thumbnailer(pool, mBridge), thumbPanelPresenter(thumbnailer), folderViewPresenter(thumbnailer) {
    loadTranslation();
    if(deps.headless) {
        // no window tree at all: the three seams are the whole surface. Their
        // completeness is guarded by the seam census at the end of this
        // constructor — deliberately not by an assert, which compiles out in
        // the Release build.
        ui_ = deps.ui;
        overlays_ = deps.overlays;
        view_ = deps.view;
        ownedDialogs = std::make_unique<DialogHost>(nullptr, nullptr);
        dialogs_ = ownedDialogs.get();
        crop_ = deps.crop; // optional: only crop-flow tests provide one
    } else {
        initGui();
        // the main window owns the dialogs and the interaction interface, so it
        // is the production provider of both; tests replace these pointers with
        // scripted fakes
        ui_ = mw;
        view_ = deps.view ? deps.view : mw;
        overlays_ = deps.overlays ? deps.overlays : mw->overlays();
        dialogs_ = mw->dialogs();
        crop_ = deps.crop ? deps.crop : mw->crop();
    }
    // the navigation object: both branches have set the seams it drives, so
    // it is constructed here, once — the listing arrives later, via attachModel
    nav_ = new NavigationController(&state, this, view_, overlays_, thumbPanelPresenter, folderViewPresenter, this);
    // the seam census — deliberately NOT Q_ASSERT, which compiles out of the
    // Release build this tree ships. A seam wired in one construction branch
    // only (nav_, in its day) must stay loud in every build shape, and the
    // message must say which seam set to inspect. crop_ is the documented
    // optional (crop-flow tests provide it); everything else is mandatory.
    if(!ui_ || !view_ || !overlays_ || !dialogs_ || !nav_)
        qFatal("Core construction left a seam null (the census checks ui_, view_, overlays_, dialogs_, nav_) — "
               "every seam must be wired in both construction branches");
    // the slideshow loop is a pure function of the state: slides advance by
    // timer, normal viewing loops. The relay rides the state binding and goes
    // through the view seam — the window's notifier keeps only the info
    // projection, this one owns the player request, so a headless Core
    // reaches the seam exactly the way production does.
    slideshowRelay = state.slideshow.addNotifier([this] {
        view_->setLoopPlayback(!state.slideshow.value());
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
    delete mw;
    mw = nullptr;
    // the translator lives on the application's translator list, so it has to
    // come off that list before it is freed — and it had no parent and no
    // destructor, so until now nothing ever freed it
    if(translator) {
        qApp->removeTranslator(translator);
        delete translator;
        translator = nullptr;
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
    pool.waitForOwner(PoolOwner::Thumbnailer);
    // The engine decides WHICH thumbnails run; the pool executes them. Both
    // halves need the same concurrency or the knob only moves one of them:
    // the pool had `thumbnailerThreads` threads while the engine admitted a
    // hardcoded 2, so a slider at 16 left twelve threads permanently idle.
    // thumbConfig existed for this and had no caller — the engine's in-flight
    // cap is now the pool's own size, so one number sizes both.
    mBridge.thumbConfig(pool.threadCount());
    nav_->applySlideshowSettings(settings->slideshowInterval(), settings->loopSlideshow());
    nav_->setFolderEndAction(settings->folderEndAction());
    bool showDirs = (settings->folderViewMode() == FV_EXT_FOLDERS);
    if(folderViewPresenter.showDirs() != showDirs)
        folderViewPresenter.setShowDirs(showDirs);
    if(state.shuffle)
        nav_->syncRandomizer();
}

// The keymap's document scope follows the view mode. Navigation writes the
// state and the window renders it; the keymap is a third consumer, and this is
// where the coordinator hands it the same fact — headless Cores included,
// which is why it cannot live in MW's notifier.
void Core::onViewModeChanged(ViewMode mode) {
    actionManager->setDocumentActive(mode != MODE_FOLDERVIEW);
}

void Core::showGui() {
    if(!mw)
        return; // headless Core has nothing to show
    view_->showWindowDefault();
    // the heavy-UI trigger is the window's own first show (shownOnce, wired
    // in the MW constructor) — the processEvents + 50 ms guess this used to
    // run is gone, along with the TODO that called it unreliable
}

// create MainWindow and all widgets
void Core::initGui() {
    mw = new MW(&state);
    mw->hide();
}

void Core::attachModel(DirectoryModel *_model) {
    model.reset(_model);
    thumbPanelPresenter.setModel(model);
    folderViewPresenter.setModel(model);
    bool showDirs = (settings->folderViewMode() == FV_EXT_FOLDERS);
    folderViewPresenter.setShowDirs(showDirs);
    nav_->attachModel(_model);
}

void Core::initComponents() {
    attachModel(new DirectoryModel(pool, mBridge));
}

void Core::connectComponents() {

    connect(&folderViewPresenter, &DirectoryPresenter::draggedOut, this,
            qOverload<const QList<QString> &>(&Core::onDraggedOut));

    connect(&folderViewPresenter, &DirectoryPresenter::droppedInto, this,
            qOverload<const QList<QString> &, const QString &>(&Core::movePathsTo));

    connect(scriptManager, &ScriptManager::error, overlays_, &IOverlayHost::showError);

    connect(overlays_, &IOverlayHost::copyRequested, this, &Core::copyCurrentFile);
    connect(overlays_, &IOverlayHost::moveRequested, this, &Core::moveCurrentFile);
    connect(overlays_, &IOverlayHost::saveAsClicked, this, &Core::requestSavePath);
    connect(overlays_, &IOverlayHost::saveRequested, this, &Core::saveCurrentFile);
    connect(overlays_, &IOverlayHost::renameRequested, this, &Core::renameCurrentSelection);
    connect(overlays_, &IOverlayHost::discardEditsRequested, this, &Core::discardEdits);

    connect(model->scaler, &Scaler::scalingFinished, this, &Core::onScalingFinished);

    if(mw)
        connectWindowComponents();
}

// The window-connected bindings and relays: view binding, dialog host signals, and the
// window->Core legs. Headless Core skips the whole method — a windowless
// process has no drops, no drags, no folder-view clicks, no keys.
void Core::connectWindowComponents() {
    connect(mw, &MW::droppedIn, this, &Core::onDropIn);
    connect(mw, &MW::copyUrlsRequested, this, qOverload<const QList<QString> &, const QString &>(&Core::copyPathsTo));
    connect(mw, &MW::moveUrlsRequested, this, &Core::movePathsTo);
    connect(mw, &MW::sortingSelected, nav_, &NavigationController::sortBy);
    connect(mw, &MW::showFoldersChanged, nav_, &NavigationController::setFoldersDisplay);
    connect(mw, &MW::draggedOut, this, qOverload<>(&Core::onDraggedOut));
    connect(mw, &MW::playbackFinished, nav_, &NavigationController::onPlaybackFinished);
    connect(mw, &MW::scalingRequested, this, &Core::scalingRequest);
    thumbPanelPresenter.setView(mw->getThumbnailPanel());
    folderViewPresenter.setView(mw->getFolderView());
    connect(mw->dialogs(), &DialogHost::opened, nav_, &NavigationController::loadPath);
    connect(mw->crop(), &CropController::cropRequested, this, &Core::crop);
    connect(mw->crop(), &CropController::cropAndSaveRequested, this, &Core::cropAndSave);
    connect(mw->dialogs(), &DialogHost::saveAsRequested, this, &Core::saveCurrentFileAs);
    connect(mw->dialogs(), &DialogHost::resizeRequested, this, &Core::resize);
}

void Core::initActions() {
    actionManager->connectAction("nextImage", nav_, &NavigationController::nextImage);
    actionManager->connectAction("prevImage", nav_, &NavigationController::prevImage);
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
    actionManager->connectAction("copyFile", overlays_, &IOverlayHost::triggerCopyOverlay);
    actionManager->connectAction("quickCopy", this, &Core::quickCopyToFixedDirectory);
    actionManager->connectAction("moveFile", overlays_, &IOverlayHost::triggerMoveOverlay);
    actionManager->connectAction("jumpToFirst", nav_, &NavigationController::jumpToFirst);
    actionManager->connectAction("jumpToLast", nav_, &NavigationController::jumpToLast);
    // script actions are dynamic — no registered QAction exists for an
    // "s:<name>" binding, so the runScript carrier signal stays a plain
    // connect (R12's documented exception)
    connect(actionManager, &ActionManager::runScript, this, &Core::runScript);
    actionManager->connectAction("folderView", nav_, &NavigationController::enableFolderView);
    actionManager->connectAction("documentView", nav_, &NavigationController::enableDocumentView);
    actionManager->connectAction("toggleFolderView", nav_, &NavigationController::toggleFolderView);
    actionManager->connectAction("reloadImage", this, qOverload<>(&Core::reloadImage));
    actionManager->connectAction("copyFileClipboard", this, &Core::copyFileClipboard);
    actionManager->connectAction("copyPathClipboard", this, &Core::copyPathClipboard);
    actionManager->connectAction("renameFile", this, &Core::showRenameDialog);
    actionManager->connectAction("sortByName", nav_, &NavigationController::sortByName);
    actionManager->connectAction("sortByTime", nav_, &NavigationController::sortByTime);
    actionManager->connectAction("sortBySize", nav_, &NavigationController::sortBySize);
    actionManager->connectAction("toggleImageInfo", overlays_, &IOverlayHost::toggleImageInfoOverlay);
    actionManager->connectAction("toggleShuffle", nav_, &NavigationController::toggleShuffle);
    actionManager->connectAction("showInDirectory", this, &Core::showInDirectory);
    actionManager->connectAction("toggleSlideshow", nav_, &NavigationController::toggleSlideshow);
    actionManager->connectAction("goUp", nav_, &NavigationController::loadParentDir);
    actionManager->connectAction("discardEdits", this, &Core::discardEdits);
    actionManager->connectAction("nextDirectory", nav_, &NavigationController::nextDirectory);
    actionManager->connectAction("prevDirectory", nav_, qOverload<>(&NavigationController::prevDirectory));
    actionManager->connectAction("print", this, &Core::print);
    actionManager->connectAction("toggleFullscreenInfoBar", this, &Core::toggleFullscreenInfoBar);
    actionManager->connectAction("pasteFile", this, &Core::openFromClipboard);
    // the folder grid's scope actions, landed on the proxy the same way the
    // viewer commands land on ViewerWidget
    if(mw)
        connectWindowActions();
}

// The window-connected actions, kept as one block: headless Core has no keys and skips the whole
// method — its tests call the slots directly. The connectAction calls stay
// in core.cpp because R12 reads this file's text for the connected list.
void Core::connectWindowActions() {
    // the projection's registration surface: a QAction's shortcuts are inert
    // until added to a widget (dispatch_probe_tests), so the whole registry
    // goes live on the main window here
    actionManager->attachWindowSurface(mw);
    actionManager->connectAction("fitWindow", mw->viewer(), &ViewerWidget::fitWindow);
    actionManager->connectAction("fitWidth", mw->viewer(), &ViewerWidget::fitWidth);
    actionManager->connectAction("fitNormal", mw->viewer(), &ViewerWidget::fitOriginal);
    actionManager->connectAction("nextFitMode", mw, &MW::cycleFitMode);
    actionManager->connectAction("toggleFitMode", mw, &MW::switchFitMode);
    actionManager->connectAction("toggleFullscreen", mw->shell(), &WindowShell::triggerFullScreen);
    actionManager->connectAction("lockZoom", mw->viewer(), &ViewerWidget::toggleLockZoom);
    actionManager->connectAction("lockView", mw->viewer(), &ViewerWidget::toggleLockView);
    actionManager->connectAction("zoomIn", mw->viewer(), &ViewerWidget::zoomIn);
    actionManager->connectAction("zoomOut", mw->viewer(), &ViewerWidget::zoomOut);
    actionManager->connectAction("zoomInCursor", mw->viewer(), &ViewerWidget::zoomInCursor);
    actionManager->connectAction("zoomOutCursor", mw->viewer(), &ViewerWidget::zoomOutCursor);
    actionManager->connectAction("scrollUp", mw->viewer(), &ViewerWidget::scrollUp);
    actionManager->connectAction("scrollDown", mw->viewer(), &ViewerWidget::scrollDown);
    actionManager->connectAction("scrollLeft", mw->viewer(), &ViewerWidget::scrollLeft);
    actionManager->connectAction("scrollRight", mw->viewer(), &ViewerWidget::scrollRight);
    actionManager->connectAction("openSettings", mw->dialogs(), &DialogHost::showSettings);
    actionManager->connectAction("closeFullScreenOrExit", mw->shell(), &WindowShell::closeFullScreenOrExit);
    actionManager->connectAction("pauseVideo", mw->viewer(), &ViewerWidget::pauseResumePlayback);
    actionManager->connectAction("seekVideoForward", mw->viewer(), &ViewerWidget::seekForward);
    actionManager->connectAction("seekVideoBackward", mw->viewer(), &ViewerWidget::seekBackward);
    actionManager->connectAction("frameStep", mw->viewer(), &ViewerWidget::frameStep);
    actionManager->connectAction("frameStepBack", mw->viewer(), &ViewerWidget::frameStepBack);
    actionManager->connectAction("contextMenu", mw, &MW::showContextMenu);
    actionManager->connectAction("toggleTransparencyGrid", mw->viewer(), &ViewerWidget::toggleTransparencyGrid);
    actionManager->connectAction("toggleScalingFilter", mw, &MW::toggleScalingFilter);
    actionManager->connectAction("toggleMute", mw->viewer(), &ViewerWidget::toggleMute);
    actionManager->connectAction("volumeUp", mw->viewer(), &ViewerWidget::volumeUp);
    actionManager->connectAction("volumeDown", mw->viewer(), &ViewerWidget::volumeDown);
    actionManager->connectAction("gridSelectNext", mw->getFolderView(), &FolderViewProxy::gridSelectNext);
    actionManager->connectAction("gridSelectPrev", mw->getFolderView(), &FolderViewProxy::gridSelectPrev);
    actionManager->connectAction("gridSelectAbove", mw->getFolderView(), &FolderViewProxy::gridSelectAbove);
    actionManager->connectAction("gridSelectBelow", mw->getFolderView(), &FolderViewProxy::gridSelectBelow);
    actionManager->connectAction("gridPageUp", mw->getFolderView(), &FolderViewProxy::gridPageUp);
    actionManager->connectAction("gridPageDown", mw->getFolderView(), &FolderViewProxy::gridPageDown);
    actionManager->connectAction("gridSelectFirst", mw->getFolderView(), &FolderViewProxy::gridSelectFirst);
    actionManager->connectAction("gridSelectLast", mw->getFolderView(), &FolderViewProxy::gridSelectLast);
    actionManager->connectAction("gridSelectAll", mw->getFolderView(), &FolderViewProxy::gridSelectAll);
    actionManager->connectAction("gridOpen", mw->getFolderView(), &FolderViewProxy::activateCurrent);
    // the folder view's Ctrl+V. Landed on Core, not the grid: the destination
    // is the model's directory and the work is interactiveCopy, both of which
    // the grid surface has no business owning.
    actionManager->connectAction("pasteFilesHere", this, &Core::pasteFilesToCurrentDirectory);
}

void Core::loadTranslation() {
    if(!translator)
        translator = new QTranslator;
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
        QApplication::removeTranslator(translator);
        return;
    }
    QString trFile = trPath + "/" + localeName;
    QString trFileFallback = trPathFallback + "/" + localeName;
    if(!translator->load(trFile)) {
        qDebug() << "Could not load translation file: " << trFile;
        if(!translator->load(trFileFallback)) {
            qDebug() << "Could not load translation file: " << trFileFallback;
            return;
        }
    }
    QApplication::installTranslator(translator);
}

void Core::onUpdate() {

    qDebug() << "Updated: " << settings->lastVersion().toString() << ">" << appVersion.toString();
    // TODO: finish changelogs
    //if(settings->showChangelogs())
    //    mw->showChangelogWindow();
    overlays_->showMessage(tr("Updated: ") + settings->lastVersion().toString() + " > " + appVersion.toString(), 4000);
    settings->setLastVersion(appVersion);
}

void Core::onFirstRun() {
    //mw->showSomeSortOfWelcomeScreen();
    overlays_->showMessage(tr("Welcome to ") + qApp->applicationName() + tr(" version ") + appVersion.toString() + "!",
                           4000);
    settings->setFirstRun(false);
    settings->setLastVersion(appVersion);
}

void Core::close() {
    if(!mw)
        return; // headless: nothing to close
    mw->close();
}
// -----------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------

void Core::toggleFullscreenInfoBar() {
    view_->toggleFullscreenInfoBar();
}

// ---------------------------------------------------------------- image operations

// ---------------------------------------------------------------- image operations ^

// todo: remove?
QString Core::selectedPath() {
    if(!model)
        return "";
    else if(state.viewMode.value() == MODE_FOLDERVIEW)
        return folderViewPresenter.selectedPaths().constLast();
    else
        return state.currentFilePath;
}

QList<QString> Core::currentSelection() {
    if(!model)
        return QList<QString>();
    else if(state.viewMode.value() == MODE_FOLDERVIEW)
        return folderViewPresenter.selectedPaths();
    else
        return QList<QString>() << state.currentFilePath;
}

//------------------------

void Core::runScript(const QString &scriptName) {
    if(model->isEmpty())
        return;
    scriptManager->runScript(scriptName, model->getImage(selectedPath()));
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
    if(model->isEmpty() || selectedPath().isEmpty())
        return;
    auto img = model->getImage(selectedPath());
    if(img->type() != DocumentType::STATIC) {
        overlays_->showMessage("Set wallpaper: file not supported");
        return;
    }
#ifdef __WIN32
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
        overlays_->showMessage("Action is not supported in your desktop session (\"" + session + "\")", 3000);
        return;
    }
    QProcess::startDetached(program, args);
#endif
}

void Core::print() {
    if(!mw)
        return; // the print dialog parents to the window; headless has none
    if(model->isEmpty())
        return;
    PrintDialog p(mw);
    auto img = model->getImage(selectedPath());
    if(!img) {
        overlays_->showError(tr("Could not open image"));
        return;
    }
    if(img->type() != DocumentType::STATIC) {
        overlays_->showError(tr("Can only print static images"));
        return;
    }
    QString pdfPath = model->directoryPath() + "/" + img->baseName() + ".pdf";
    p.setImage(img->getImage());
    p.setOutputPath(pdfPath);
    p.exec();
}

void Core::scalingRequest(QSize size, ScalingFilter filter) {
    // visibility is the emitter's own timing now (requestScaling suppresses
    // while hidden and re-requests on show); what remains here is the state check
    if(state.hasActiveImage) {
        std::shared_ptr<Image> forScale = model->getImage(state.currentFilePath);
        if(forScale) {
            model->scaler->requestScaled(
                ScalerRequest(forScale, size, state.currentFilePath, filter, settings->smoothUpscaling()));
        }
    }
}

// TODO: don't use connect? otherwise there is no point using unique_ptr
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
    if(state.hasActiveImage && req.path == state.currentFilePath) {
        view_->onScalingFinished(std::make_unique<const QImage>(*scaled));
    }
}

// reset state; clear cache; etc

QString Core::currentFilePath() const {
    return state.currentFilePath;
}

bool Core::firstFramePending() const {
    return state.delayModel;
}

bool Core::isSettled() const {
    return model && model->listingSettled() && !model->loaderBusy();
}

void Core::guiSetImage(std::shared_ptr<Image> img) {
    state.hasActiveImage = true;
    if(!img) {
        overlays_->showMessage(tr("Error: could not load image."));
        return;
    }
    DocumentType type = img->type();
    if(type == STATIC) {
        view_->showImage(img->getImage());
    } else if(type == ANIMATED) {
        auto animated = dynamic_cast<ImageAnimated *>(img.get());
        view_->showAnimation(animated->getMovie());
    } else if(type == VIDEO) {
        auto video = dynamic_cast<Video *>(img.get());
        // workaround for mpv. If we play video while mainwindow is hidden we get black screen.
        // affects only initial startup (e.g. we open webm from file manager)
        showGui();
        view_->showVideo(video->filePath());
    }
    img->isEdited() ? overlays_->showSaveOverlay() : overlays_->hideSaveOverlay();
    overlays_->setExifInfo(img->getExifTags());
}

void Core::updateInfoString() {
    CurrentInfo ci;
    ci.index = model->indexOfFile(state.currentFilePath);
    ci.fileCount = model->fileCount();
    ci.fileName = model->fileNameAt(ci.index);
    ci.filePath = model->filePathAt(ci.index);
    ci.directoryPath = model->directoryPath();
    ci.directoryName = ci.directoryPath.split("/").last();
    if(model->isLoaded(state.currentFilePath)) {
        auto img = model->getImage(state.currentFilePath);
        ci.imageSize = img->originalSize();
        ci.fileSize = img->fileSize();
        ci.edited = img->isEdited();
    }
    ci.slideshow = state.slideshow;
    ci.shuffle = state.shuffle;
    // the assignment is the notification: MW re-derives its views from the
    // projection on change (issue #505 semantics live in onLoadFailed)
    state.currentInfo = ci;
}
