/*
 * Core's navigation, as an object: the path and index walk, the directory
 * boundary, the slideshow clock, the mode transitions, and the model
 * reactions that keep the current image pinned to the listing. Owns the
 * slideshow timer, the shuffle randomizer, the folder-end policy and the
 * loop flag; writes the navigation slice of State (currentFilePath,
 * directoryPath, viewMode, slideshow, shuffle, pendingListedToken,
 * delayModel, two hasActiveImage clears). The coordinator's kernel — the
 * display sink, the info projection, the window shower — is NOT owned:
 * the controller calls it back through INavigationHost. The listing
 * materializes on first load, so the model arrives via attachModel.
 */

#include "navigationcontroller.h"

/*
 * Core's navigation half: path and index loading, the directory walk, the
 * slideshow clock, the mode transitions, and the model reactions that keep
 * the current image pinned to the listing. Same class, partial
 * implementation — core.cpp keeps composition, wiring and the display sink.
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
#include <QFileInfo>
#include <QTemporaryFile>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

#ifdef __WIN32
#include <tchar.h>
#endif

NavigationController::NavigationController(State *stateArg, INavigationHost *hostArg, ICoreView *viewArg,
                                           IOverlayHost *overlaysArg, DirectoryPresenter &thumbPresenterArg,
                                           DirectoryPresenter &folderPresenterArg, QObject *parent)
    : QObject(parent), state(stateArg), host_(hostArg), view_(viewArg), overlays_(overlaysArg),
      thumbPanelPresenter(thumbPresenterArg), folderViewPresenter(folderPresenterArg),
      folderEndAction(FOLDER_END_NO_ACTION), loopSlideshow(false) {
    slideshowTimer.setSingleShot(true);
    // the clock's tick is this controller's own pulse — wired here, where
    // the timer lives, not in the coordinator
    connect(&slideshowTimer, &QTimer::timeout, this, &NavigationController::nextImageSlideshow);
    // the presenter reactions, likewise owned here: the presenters arrive as
    // constructor collaborators, so their connects live with the controller —
    // a standalone construction is fully live without coordinator-side wiring
    connect(&thumbPanelPresenter, &DirectoryPresenter::fileActivated, this,
            &NavigationController::onDirectoryViewFileActivated);
    connect(&thumbPanelPresenter, &DirectoryPresenter::dirActivated, this, &NavigationController::loadPath);
    connect(&folderViewPresenter, &DirectoryPresenter::fileActivated, this,
            &NavigationController::onDirectoryViewFileActivated);
    connect(&folderViewPresenter, &DirectoryPresenter::dirActivated, this, &NavigationController::loadPath);
}

void NavigationController::attachModel(DirectoryModel *m) {
    model = m;
    // the model reactions, owned by the controller that serves them: with
    // this attachment the controller is fully live — no coordinator-side
    // wiring is needed for anything navigation reacts to
    connect(model, &DirectoryModel::loaded, this, &NavigationController::onModelLoaded);
    connect(model, &DirectoryModel::imageReady, this, &NavigationController::onModelItemReady);
    connect(model, &DirectoryModel::imageUpdated, this, &NavigationController::onModelItemUpdated);
    connect(model, &DirectoryModel::sortingChanged, this, &NavigationController::onModelSortingChanged);
    connect(model, &DirectoryModel::loadFailed, this, &NavigationController::onLoadFailed);
    connect(model, &DirectoryModel::fileAdded, this, &NavigationController::onFileAdded);
    connect(model, &DirectoryModel::fileRemoved, this, &NavigationController::onFileRemoved);
    connect(model, &DirectoryModel::fileRenamed, this, &NavigationController::onFileRenamed);
    connect(model, &DirectoryModel::divergenceDetected, this, &NavigationController::onDivergenceDetected);
    if(state->shuffle)
        syncRandomizer();
}

void NavigationController::setFolderEndAction(FolderEndAction action) {
    folderEndAction = action;
}

void NavigationController::applySlideshowSettings(int intervalMs, bool loop) {
    slideshowTimer.setInterval(intervalMs);
    loopSlideshow = loop;
}

void NavigationController::toggleShuffle() {
    if(state->shuffle) {
        overlays_->showMessage(tr("Shuffle mode: OFF"));
    } else {
        syncRandomizer();
        overlays_->showMessage(tr("Shuffle mode: ON"));
    }
    state->shuffle = !state->shuffle;
    host_->updateInfoString();
}

void NavigationController::toggleSlideshow() {
    if(state->slideshow) {
        stopSlideshow();
        overlays_->showMessage(tr("Slideshow: OFF"));

    } else {
        startSlideshow();
        overlays_->showMessage(tr("Slideshow: ON"));
    }
}

void NavigationController::startSlideshow() {
    if(!state->slideshow) {
        state->slideshow = true;
        enableDocumentView();
        startSlideshowTimer();
        host_->updateInfoString();
    }
}

void NavigationController::stopSlideshow() {
    if(state->slideshow) {
        state->slideshow = false;
        slideshowTimer.stop();
        host_->updateInfoString();
    }
}

void NavigationController::onPlaybackFinished() {
    if(state->slideshow) {
        nextImageSlideshow();
    }
}

void NavigationController::syncRandomizer() {
    if(model) {
        randomizer.setCount(model->fileCount());
        randomizer.shuffle();
        randomizer.setCurrent(model->indexOfFile(state->currentFilePath));
    }
}

void NavigationController::onModelLoaded() {
    thumbPanelPresenter.reloadModel();
    folderViewPresenter.reloadModel();
    thumbPanelPresenter.selectAndFocus(state->currentFilePath);
    folderViewPresenter.selectAndFocus(state->currentFilePath);
    if(state->shuffle)
        syncRandomizer();
}

void NavigationController::onDirectoryViewFileActivated(const QString &filePath) {
    // we aren`t using async load so it won't flicker with empty view
    state->viewMode = MODE_DOCUMENT;
    loadPath(filePath);
}

// the mode is Core's decision, recorded in state; the window subscribes and
// runs the view dance on change. The load-coupled surface cleanup (hiding the
// overlays for a newly loaded folder) lives at the load site, not in this
// transition.
void NavigationController::enterMode(ViewMode mode) {
    state->viewMode = mode;
}

void NavigationController::enableFolderView() {
    if(state->viewMode.value() == MODE_FOLDERVIEW)
        return;
    stopSlideshow();
    enterMode(MODE_FOLDERVIEW);
}

void NavigationController::enableDocumentView() {
    if(state->viewMode.value() == MODE_DOCUMENT)
        return;
    enterMode(MODE_DOCUMENT);
    if(model && model->fileCount() && state->currentFilePath == "") {
        const QString selected = folderViewPresenter.selectedPaths().value(0);
        // if it is a directory - ignore and just open the first file
        if(model->containsFile(selected))
            loadPath(selected);
        else
            loadPath(model->firstFile());
    }
}

void NavigationController::toggleFolderView() {
    if(state->viewMode.value() == MODE_FOLDERVIEW)
        enableDocumentView();
    else
        enableFolderView();
}

void NavigationController::sortBy(SortingMode mode) {
    model->setSortingMode(mode);
}

void NavigationController::setFoldersDisplay(bool mode) {
    if(folderViewPresenter.showDirs() != mode)
        folderViewPresenter.setShowDirs(mode);
}

void NavigationController::onFileRemoved(const QString &filePath, int index) {
    // no files left
    if(model->isEmpty()) {
        view_->closeImage();
        state->hasActiveImage = false;
        state->currentFilePath = "";
    }
    // image mode && removed current file
    if(state->currentFilePath == filePath) {
        if(state->viewMode.value() == MODE_DOCUMENT) {
            if(!loadFileIndex(index, true, settings->usePreloader()))
                loadFileIndex(--index, true, settings->usePreloader());
        } else {
            state->hasActiveImage = false;
            state->currentFilePath = "";
        }
    }
    host_->updateInfoString();
}

void NavigationController::onFileAdded(const QString &filePath) {
    Q_UNUSED(filePath)
    // update file count
    host_->updateInfoString();
    if(model->fileCount() == 1 && state->currentFilePath == "")
        loadFileIndex(0, false, settings->usePreloader());
}

// Following the file the viewer is showing across a rename is the reason the
// rename is one event: the Removed half of the old pair had already bounced
// currentFilePath to "" by the time the Added half arrived, and only a rename
// the app itself issued could be patched back.
void NavigationController::onFileRenamed(const QString &filePath, const QString &newPath, int oldIndex) {
    Q_UNUSED(oldIndex)
    if(state->currentFilePath == filePath)
        loadPath(newPath);
    host_->updateInfoString();
}

// The model edge-detects — its slot owns the mtime comparison and the
// per-(path, disk-mtime) dedup, and hands Core one divergenceDetected per
// real divergence. Core only carries the human-facing half, for the file on
// screen; a self-save never reaches this (Image::save refreshes the recorded
// mtime, so the model sees no edge at all).
void NavigationController::onDivergenceDetected(const QString &filePath) {
    if(filePath == state->currentFilePath)
        overlays_->showMessage(tr("File changed on disk — your edits are kept in memory."));
}

void NavigationController::sortByName() {
    auto mode = SortingMode::SORT_NAME;
    if(model->sortingMode() == mode)
        mode = SortingMode::SORT_NAME_DESC;
    model->setSortingMode(mode);
}

void NavigationController::sortByTime() {
    auto mode = SortingMode::SORT_TIME;
    if(model->sortingMode() == mode)
        mode = SortingMode::SORT_TIME_DESC;
    model->setSortingMode(mode);
}

void NavigationController::sortBySize() {
    auto mode = SortingMode::SORT_SIZE;
    if(model->sortingMode() == mode)
        mode = SortingMode::SORT_SIZE_DESC;
    model->setSortingMode(mode);
}

bool NavigationController::loadPath(const QString &path) {
    if(path.isEmpty())
        return false;
    QString target = path;
    if(target.startsWith("file://", Qt::CaseInsensitive))
        target.remove(0, 7);

    stopSlideshow();
    state->delayModel = false;
    QFileInfo fileInfo(target);
    // the target lives in a local until the model has accepted it — the
    // state->directoryPath write is the commit point inside setDirectory, and
    // the folder view re-derives from that write's notify. Writing the
    // property here would move the view before the load could be refused.
    QString dir;
    if(fileInfo.isDir()) {
        dir = QDir(target).absolutePath();
    } else if(fileInfo.isFile()) {
        dir = fileInfo.absolutePath();
        // delayModel now means exactly one thing: the GUI is still hidden and
        // the first ready item must raise it. It no longer defers the listing
        // — the open target below arms to the landing instead of racing it
        state->delayModel = (model->directoryPath() != dir);
    } else {
        overlays_->showError(tr("Could not open target: ") + target);
        qDebug() << "Could not open target: " << target;
        return false;
    }
    if(!setDirectory(dir))
        return false;

    // load file / folderview
    if(fileInfo.isFile()) {
        // membership is the listing's decision alone: a file the engine's
        // filter does not admit is not openable, no mime-sniff rescue, no
        // local insert (forceInsert retired 2026-11 — the mirror has no
        // writers besides the apply target)
        enterMode(MODE_DOCUMENT);
        int index = model->indexOfFile(fileInfo.absoluteFilePath());
        if(index != -1)
            return loadFileIndex(index, false, settings->usePreloader());
        // cold open: this directory's listing has not landed yet (or has not
        // started). Start the scan and arm the open to its landing — the same
        // whenListed primitive the folder-boundary loads ride on. The old
        // code bootstrapped this moment through forceInsert's local write;
        // that exception is what hid the fact that the open target had no
        // landing-driven form at all.
        const QString filePath = fileInfo.absoluteFilePath();
        state->pendingListedToken = model->whenListed(dir, [this, filePath] {
            state->pendingListedToken = 0;
            int idx = model->indexOfFile(filePath);
            if(idx != -1)
                loadFileIndex(idx, false, settings->usePreloader());
            else
                overlays_->showError(tr("Not in this folder's listing: ") + filePath);
        });
        return true;
    } else {
        state->viewMode = MODE_FOLDERVIEW;
        return true;
    }
}

bool NavigationController::setDirectory(const QString &path) {
    if(model->directoryPath() != path) {
        this->reset();
        if(!model->setDirectory(path)) {
            overlays_->showError(tr("Could not load folder: ") + path);
            return false;
        }
        // the state write is the commit point: the folder view re-derives
        // from this notify, so it must not fire before the model has accepted
        // the directory — a refused load leaves the view where it was. This
        // replaces the direct mw->setDirectoryPath push, and also writes
        // state on the next/prev directory paths, which never touched
        // state->directoryPath and left it stale.
        state->directoryPath = QDir(path).absolutePath();
    }
    return true;
}

bool NavigationController::loadFileIndex(int index, bool async, bool preload) {
    if(!model)
        return false;
    // an explicit choice of file outranks the deferred folder-boundary load
    if(state->pendingListedToken) {
        model->cancelListed(state->pendingListedToken);
        state->pendingListedToken = 0;
    }
    auto entry = model->fileEntryAt(index);
    if(entry.path.isEmpty())
        return false;
    // direction-aware preload: decode only the side of travel
    int oldIndex = model->indexOfFile(state->currentFilePath);
    bool movingForward = (oldIndex == -1) ? true : (index >= oldIndex);
    state->currentFilePath = entry.path;
    model->unloadExcept(entry.path, preload);
    model->load(entry.path, async);
    if(preload) {
        if(movingForward)
            model->preload(model->nextOf(entry.path));
        else
            model->preload(model->prevOf(entry.path));
    }
    thumbPanelPresenter.selectAndFocus(entry.path);
    folderViewPresenter.selectAndFocus(entry.path);
    host_->updateInfoString();
    return true;
}

void NavigationController::loadParentDir() {
    if(model->directoryPath().isEmpty() || state->viewMode.value() != MODE_FOLDERVIEW)
        return;
    stopSlideshow();
    QFileInfo currentDir(model->directoryPath());
    QFileInfo parentDir(currentDir.absolutePath());
    if(parentDir.exists() && parentDir.isReadable())
        loadPath(parentDir.absoluteFilePath());
    folderViewPresenter.selectAndFocus(currentDir.absoluteFilePath());
}

void NavigationController::nextDirectory() {
    if(model->directoryPath().isEmpty() || state->viewMode.value() != MODE_DOCUMENT)
        return;
    stopSlideshow();
    QString next = model->adjacentDirectory(model->directoryPath(), true);
    if(next.isEmpty()) {
        return;
    }
    if(!setDirectory(next))
        return;
    overlays_->showMessageDirectory(QFileInfo(next).baseName());
    // the new folder's listing is in flight; loading index 0 here would show a
    // file of the folder we just left — defer until it lands
    state->pendingListedToken = model->whenListed(next, [this] {
        if(model->fileCount())
            loadFileIndex(0, false, true);
    });
}

void NavigationController::prevDirectory(bool selectLast) {
    if(model->directoryPath().isEmpty() || state->viewMode.value() != MODE_DOCUMENT)
        return;
    QString prev = model->adjacentDirectory(model->directoryPath(), false);
    if(prev.isEmpty()) {
        return;
    }
    if(!setDirectory(prev))
        return;
    overlays_->showMessageDirectory(QFileInfo(prev).baseName());
    state->pendingListedToken = model->whenListed(prev, [this, selectLast] {
        if(model->fileCount())
            loadFileIndex(selectLast ? static_cast<int>(model->fileCount()) - 1 : 0, false, true);
    });
}

void NavigationController::prevDirectory() {
    prevDirectory(false);
}

void NavigationController::nextImage() {
    if(state->viewMode.value() == MODE_FOLDERVIEW || (model->isEmpty() && folderEndAction != FOLDER_END_GOTO_ADJACENT))
        return;
    stopSlideshow();
    if(state->shuffle) {
        loadFileIndex(randomizer.next(), true, false);
        return;
    }
    int newIndex = model->indexOfFile(state->currentFilePath) + 1;
    auto decision = FolderEnd::decide(folderEndAction, newIndex, model->fileCount());
    switch(decision.outcome) {
    case FolderEnd::Outcome::LoadIndex: loadFileIndex(decision.index, true, settings->usePreloader()); return;
    case FolderEnd::Outcome::EnterAdjacentFolder: nextDirectory(); return;
    case FolderEnd::Outcome::ReportBoundary:
        if(!model->loaderBusy())
            return;
    }
}

void NavigationController::prevImage() {
    if(state->viewMode.value() == MODE_FOLDERVIEW || (model->isEmpty() && folderEndAction != FOLDER_END_GOTO_ADJACENT))
        return;
    stopSlideshow();
    if(state->shuffle) {
        loadFileIndex(randomizer.prev(), true, false);
        return;
    }

    int newIndex = model->indexOfFile(state->currentFilePath) - 1;
    auto decision = FolderEnd::decide(folderEndAction, newIndex, model->fileCount());
    switch(decision.outcome) {
    case FolderEnd::Outcome::LoadIndex: loadFileIndex(decision.index, true, settings->usePreloader()); return;
    case FolderEnd::Outcome::EnterAdjacentFolder: prevDirectory(true); return;
    case FolderEnd::Outcome::ReportBoundary:
        if(!model->loaderBusy())
            return;
    }
}

void NavigationController::nextImageSlideshow() {
    if(model->isEmpty() || state->viewMode.value() == MODE_FOLDERVIEW)
        return;
    if(state->shuffle) {
        loadFileIndex(randomizer.next(), false, false);
    } else {
        int newIndex = model->indexOfFile(state->currentFilePath) + 1;
        if(newIndex >= model->fileCount()) {
            if(loopSlideshow) {
                newIndex = 0;
            } else {
                stopSlideshow();
                overlays_->showMessage(tr("End of directory."));
                return;
            }
        }
        loadFileIndex(newIndex, false, true);
    }
    startSlideshowTimer();
}

void NavigationController::startSlideshowTimer() {
    // start timer only for static images or single frame gifs
    // for proper gifs and video we get a playbackFinished() signal
    auto img = model->getImage(state->currentFilePath);
    if(img->type() == STATIC) {
        slideshowTimer.start();
    } else if(img->type() == ANIMATED) {
        auto anim = dynamic_cast<ImageAnimated *>(img.get());
        if(anim && anim->frameCount() <= 1)
            slideshowTimer.start();
    }
}

void NavigationController::jumpToFirst() {
    if(model->isEmpty())
        return;
    stopSlideshow();
    loadFileIndex(0, true, settings->usePreloader());
}

void NavigationController::jumpToLast() {
    if(model->isEmpty())
        return;
    stopSlideshow();
    loadFileIndex(model->fileCount() - 1, true, settings->usePreloader());
}

void NavigationController::onLoadFailed(const QString &path) {
    // upstream issue #505: a preload failing for a file the user is not
    // viewing used to surface "Load failed" over the image on screen —
    // noise from a file they never opened. Only the file the view actually
    // needs closed may announce its own failure; everything else is a debug
    // note (navigating to it retries the load and reports then).
    if(path != state->currentFilePath) {
        qDebug() << "Load failed (not on screen):" << path;
        return;
    }
    overlays_->showMessage(tr("Load failed: ") + path);
    view_->closeImage(); // the seam, not the window: headless Core lands here too
}

void NavigationController::onModelItemReady(std::shared_ptr<Image> img, const QString &path, bool scaled) {
    if(path == state->currentFilePath) {
        host_->guiSetImage(img);
        host_->updateInfoString();
        if(state->delayModel) {
            host_->showGui();
            state->delayModel = false;
        }
        model->unloadExcept(state->currentFilePath, settings->usePreloader());
        // two-tier: a scaled image on screen schedules the full-res upgrade.
        // The next flip's clearPool kills it if the user moved on.
        if(scaled)
            model->upgradeToFull(path);
    }
}

void NavigationController::onModelItemUpdated(const QString &filePath) {
    if(filePath == state->currentFilePath) {
        host_->guiSetImage(model->getImage(filePath));
        host_->updateInfoString();
    }
}

void NavigationController::onModelSortingChanged(SortingMode mode) {
    view_->onSortingChanged(mode);
    thumbPanelPresenter.reloadModel();
    thumbPanelPresenter.selectAndFocus(state->currentFilePath);
    folderViewPresenter.reloadModel();
    folderViewPresenter.selectAndFocus(state->currentFilePath);
}

void NavigationController::reset() {
    state->hasActiveImage = false;
    state->currentFilePath = "";
    if(state->pendingListedToken) {
        model->cancelListed(state->pendingListedToken);
        state->pendingListedToken = 0;
    }
    model->setDirectory("");
}
