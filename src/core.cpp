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
#include "gui/cropcontroller.h"

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
      folderEndAction(FOLDER_END_NO_ACTION), loopSlideshow(false), thumbnailer(pool, mBridge),
      thumbPanelPresenter(thumbnailer), folderViewPresenter(thumbnailer) {
    loadTranslation();
    if(deps.headless) {
        // no window tree at all: the three seams are the whole surface, and a
        // missing one would surface as a null deref on first use — assert it
        Q_ASSERT(deps.ui && deps.overlays && deps.view);
        ui_ = deps.ui;
        overlays_ = deps.overlays;
        view_ = deps.view;
        ownedDialogs = std::make_unique<DialogHost>(nullptr, nullptr);
        dialogs_ = ownedDialogs.get();
    } else {
        initGui();
        // the main window owns the dialogs and the interaction interface, so it
        // is the production provider of both; tests replace these pointers with
        // scripted fakes
        ui_ = mw;
        view_ = deps.view ? deps.view : mw;
        overlays_ = deps.overlays ? deps.overlays : mw->overlays();
        dialogs_ = mw->dialogs();
    }
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
    slideshowTimer.setSingleShot(true);
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
    loopSlideshow = settings->loopSlideshow();
    folderEndAction = settings->folderEndAction();
    slideshowTimer.setInterval(settings->slideshowInterval());
    bool showDirs = (settings->folderViewMode() == FV_EXT_FOLDERS);
    if(folderViewPresenter.showDirs() != showDirs)
        folderViewPresenter.setShowDirs(showDirs);
    if(state.shuffle)
        syncRandomizer();
}

void Core::showGui() {
    if(!mw)
        return; // headless Core has nothing to show
    if(!view_->isWindowVisible())
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
    if(state.shuffle)
        syncRandomizer();
}

void Core::initComponents() {
    attachModel(new DirectoryModel(pool, mBridge));
}

void Core::connectComponents() {
    connect(&thumbPanelPresenter, &DirectoryPresenter::fileActivated, this, &Core::onDirectoryViewFileActivated);
    connect(&thumbPanelPresenter, &DirectoryPresenter::dirActivated, this, &Core::loadPath);

    connect(&folderViewPresenter, &DirectoryPresenter::fileActivated, this, &Core::onDirectoryViewFileActivated);
    connect(&folderViewPresenter, &DirectoryPresenter::dirActivated, this, &Core::loadPath);

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

    connect(model.get(), &DirectoryModel::fileAdded, this, &Core::onFileAdded);
    connect(model.get(), &DirectoryModel::fileRemoved, this, &Core::onFileRemoved);
    connect(model.get(), &DirectoryModel::divergenceDetected, this, &Core::onDivergenceDetected);
    connect(model.get(), &DirectoryModel::fileRenamed, this, &Core::onFileRenamed);
    connect(model.get(), &DirectoryModel::loaded, this, &Core::onModelLoaded);
    connect(model.get(), &DirectoryModel::imageReady, this, &Core::onModelItemReady);
    connect(model.get(), &DirectoryModel::imageUpdated, this, &Core::onModelItemUpdated);
    connect(model.get(), &DirectoryModel::sortingChanged, this, &Core::onModelSortingChanged);
    connect(model.get(), &DirectoryModel::loadFailed, this, &Core::onLoadFailed);

    connect(&slideshowTimer, &QTimer::timeout, this, &Core::nextImageSlideshow);
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
    connect(mw, &MW::sortingSelected, this, &Core::sortBy);
    connect(mw, &MW::showFoldersChanged, this, &Core::setFoldersDisplay);
    connect(mw, &MW::draggedOut, this, qOverload<>(&Core::onDraggedOut));
    connect(mw, &MW::playbackFinished, this, &Core::onPlaybackFinished);
    connect(mw, &MW::scalingRequested, this, &Core::scalingRequest);
    thumbPanelPresenter.setView(mw->getThumbnailPanel());
    folderViewPresenter.setView(mw->getFolderView());
    connect(mw->dialogs(), &DialogHost::opened, this, &Core::loadPath);
    connect(mw->crop(), &CropController::cropRequested, this, &Core::crop);
    connect(mw->crop(), &CropController::cropAndSaveRequested, this, &Core::cropAndSave);
    connect(mw->dialogs(), &DialogHost::saveAsRequested, this, &Core::saveCurrentFileAs);
    connect(mw->dialogs(), &DialogHost::resizeRequested, this, &Core::resize);
}

void Core::initActions() {
    connect(actionManager, &ActionManager::nextImage, this, &Core::nextImage);
    connect(actionManager, &ActionManager::prevImage, this, &Core::prevImage);
    connect(actionManager, &ActionManager::resize, this, &Core::showResizeDialog);
    connect(actionManager, &ActionManager::flipH, this, &Core::flipH);
    connect(actionManager, &ActionManager::flipV, this, &Core::flipV);
    connect(actionManager, &ActionManager::rotateLeft, this, &Core::rotateLeft);
    connect(actionManager, &ActionManager::rotateRight, this, &Core::rotateRight);
    connect(actionManager, &ActionManager::crop, this, &Core::toggleCropPanel);
    connect(actionManager, &ActionManager::setWallpaper, this, &Core::setWallpaper);
    connect(actionManager, &ActionManager::open, this, &Core::showOpenDialog);
    connect(actionManager, &ActionManager::save, this, &Core::saveCurrentFile);
    connect(actionManager, &ActionManager::saveAs, this, &Core::requestSavePath);
    connect(actionManager, &ActionManager::exit, this, &Core::close);
    connect(actionManager, &ActionManager::removeFile, this, &Core::removePermanent);
    connect(actionManager, &ActionManager::moveToTrash, this, &Core::moveToTrash);
    connect(actionManager, &ActionManager::copyFile, overlays_, &IOverlayHost::triggerCopyOverlay);
    connect(actionManager, &ActionManager::quickCopy, this, &Core::quickCopyToFixedDirectory);
    connect(actionManager, &ActionManager::moveFile, overlays_, &IOverlayHost::triggerMoveOverlay);
    connect(actionManager, &ActionManager::jumpToFirst, this, &Core::jumpToFirst);
    connect(actionManager, &ActionManager::jumpToLast, this, &Core::jumpToLast);
    connect(actionManager, &ActionManager::runScript, this, &Core::runScript);
    connect(actionManager, &ActionManager::folderView, this, &Core::enableFolderView);
    connect(actionManager, &ActionManager::documentView, this, &Core::enableDocumentView);
    connect(actionManager, &ActionManager::toggleFolderView, this, &Core::toggleFolderView);
    connect(actionManager, &ActionManager::reloadImage, this, qOverload<>(&Core::reloadImage));
    connect(actionManager, &ActionManager::copyFileClipboard, this, &Core::copyFileClipboard);
    connect(actionManager, &ActionManager::copyPathClipboard, this, &Core::copyPathClipboard);
    connect(actionManager, &ActionManager::renameFile, this, &Core::showRenameDialog);
    connect(actionManager, &ActionManager::sortByName, this, &Core::sortByName);
    connect(actionManager, &ActionManager::sortByTime, this, &Core::sortByTime);
    connect(actionManager, &ActionManager::sortBySize, this, &Core::sortBySize);
    connect(actionManager, &ActionManager::toggleImageInfo, overlays_, &IOverlayHost::toggleImageInfoOverlay);
    connect(actionManager, &ActionManager::toggleShuffle, this, &Core::toggleShuffle);
    connect(actionManager, &ActionManager::showInDirectory, this, &Core::showInDirectory);
    connect(actionManager, &ActionManager::toggleSlideshow, this, &Core::toggleSlideshow);
    connect(actionManager, &ActionManager::goUp, this, &Core::loadParentDir);
    connect(actionManager, &ActionManager::discardEdits, this, &Core::discardEdits);
    connect(actionManager, &ActionManager::nextDirectory, this, &Core::nextDirectory);
    connect(actionManager, &ActionManager::prevDirectory, this, qOverload<>(&Core::prevDirectory));
    connect(actionManager, &ActionManager::print, this, &Core::print);
    connect(actionManager, &ActionManager::toggleFullscreenInfoBar, this, &Core::toggleFullscreenInfoBar);
    connect(actionManager, &ActionManager::pasteFile, this, &Core::openFromClipboard);
    // the folder grid's scope actions, landed on the proxy the same way the
    // viewer commands land on ViewerWidget
    if(mw)
        connectWindowActions();
}

// The window-connected actions, kept as one block: headless Core has no keys and skips the whole
// method — its tests call the slots directly. The connects stay in core.cpp
// because R12 reads this file's text for the registry.
void Core::connectWindowActions() {
    connect(actionManager, &ActionManager::fitWindow, mw->viewer(), &ViewerWidget::fitWindow);
    connect(actionManager, &ActionManager::fitWidth, mw->viewer(), &ViewerWidget::fitWidth);
    connect(actionManager, &ActionManager::fitNormal, mw->viewer(), &ViewerWidget::fitOriginal);
    connect(actionManager, &ActionManager::nextFitMode, mw, &MW::cycleFitMode);
    connect(actionManager, &ActionManager::toggleFitMode, mw, &MW::switchFitMode);
    connect(actionManager, &ActionManager::toggleFullscreen, mw->shell(), &WindowShell::triggerFullScreen);
    connect(actionManager, &ActionManager::lockZoom, mw->viewer(), &ViewerWidget::toggleLockZoom);
    connect(actionManager, &ActionManager::lockView, mw->viewer(), &ViewerWidget::toggleLockView);
    connect(actionManager, &ActionManager::zoomIn, mw->viewer(), &ViewerWidget::zoomIn);
    connect(actionManager, &ActionManager::zoomOut, mw->viewer(), &ViewerWidget::zoomOut);
    connect(actionManager, &ActionManager::zoomInCursor, mw->viewer(), &ViewerWidget::zoomInCursor);
    connect(actionManager, &ActionManager::zoomOutCursor, mw->viewer(), &ViewerWidget::zoomOutCursor);
    connect(actionManager, &ActionManager::scrollUp, mw->viewer(), &ViewerWidget::scrollUp);
    connect(actionManager, &ActionManager::scrollDown, mw->viewer(), &ViewerWidget::scrollDown);
    connect(actionManager, &ActionManager::scrollLeft, mw->viewer(), &ViewerWidget::scrollLeft);
    connect(actionManager, &ActionManager::scrollRight, mw->viewer(), &ViewerWidget::scrollRight);
    connect(actionManager, &ActionManager::openSettings, mw->dialogs(), &DialogHost::showSettings);
    connect(actionManager, &ActionManager::closeFullScreenOrExit, mw->shell(), &WindowShell::closeFullScreenOrExit);
    connect(actionManager, &ActionManager::pauseVideo, mw->viewer(), &ViewerWidget::pauseResumePlayback);
    connect(actionManager, &ActionManager::seekVideoForward, mw->viewer(), &ViewerWidget::seekForward);
    connect(actionManager, &ActionManager::seekVideoBackward, mw->viewer(), &ViewerWidget::seekBackward);
    connect(actionManager, &ActionManager::frameStep, mw->viewer(), &ViewerWidget::frameStep);
    connect(actionManager, &ActionManager::frameStepBack, mw->viewer(), &ViewerWidget::frameStepBack);
    connect(actionManager, &ActionManager::contextMenu, mw, &MW::showContextMenu);
    connect(actionManager, &ActionManager::toggleTransparencyGrid, mw->viewer(), &ViewerWidget::toggleTransparencyGrid);
    connect(actionManager, &ActionManager::toggleScalingFilter, mw, &MW::toggleScalingFilter);
    connect(actionManager, &ActionManager::toggleMute, mw->viewer(), &ViewerWidget::toggleMute);
    connect(actionManager, &ActionManager::volumeUp, mw->viewer(), &ViewerWidget::volumeUp);
    connect(actionManager, &ActionManager::volumeDown, mw->viewer(), &ViewerWidget::volumeDown);
    connect(actionManager, &ActionManager::gridSelectNext, mw->getFolderView(), &FolderViewProxy::gridSelectNext);
    connect(actionManager, &ActionManager::gridSelectPrev, mw->getFolderView(), &FolderViewProxy::gridSelectPrev);
    connect(actionManager, &ActionManager::gridSelectAbove, mw->getFolderView(), &FolderViewProxy::gridSelectAbove);
    connect(actionManager, &ActionManager::gridSelectBelow, mw->getFolderView(), &FolderViewProxy::gridSelectBelow);
    connect(actionManager, &ActionManager::gridPageUp, mw->getFolderView(), &FolderViewProxy::gridPageUp);
    connect(actionManager, &ActionManager::gridPageDown, mw->getFolderView(), &FolderViewProxy::gridPageDown);
    connect(actionManager, &ActionManager::gridSelectFirst, mw->getFolderView(), &FolderViewProxy::gridSelectFirst);
    connect(actionManager, &ActionManager::gridSelectLast, mw->getFolderView(), &FolderViewProxy::gridSelectLast);
    connect(actionManager, &ActionManager::gridSelectAll, mw->getFolderView(), &FolderViewProxy::gridSelectAll);
    connect(actionManager, &ActionManager::gridOpen, mw->getFolderView(), &FolderViewProxy::activateCurrent);
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
    // filter out an unnecessary scale request at statup
    if(view_->isWindowVisible() && state.hasActiveImage) {
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
void Core::reset() {
    state.hasActiveImage = false;
    state.currentFilePath = "";
    if(state.pendingListedToken) {
        model->cancelListed(state.pendingListedToken);
        state.pendingListedToken = 0;
    }
    model->setDirectory("");
}

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
