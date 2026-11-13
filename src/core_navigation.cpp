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

void Core::toggleShuffle() {
    if(state.shuffle) {
        overlays_->showMessage(tr("Shuffle mode: OFF"));
    } else {
        syncRandomizer();
        overlays_->showMessage(tr("Shuffle mode: ON"));
    }
    state.shuffle = !state.shuffle;
    updateInfoString();
}

void Core::toggleSlideshow() {
    if(state.slideshow) {
        stopSlideshow();
        overlays_->showMessage(tr("Slideshow: OFF"));

    } else {
        startSlideshow();
        overlays_->showMessage(tr("Slideshow: ON"));
    }
}

void Core::startSlideshow() {
    if(!state.slideshow) {
        state.slideshow = true;
        enableDocumentView();
        startSlideshowTimer();
        updateInfoString();
    }
}

void Core::stopSlideshow() {
    if(state.slideshow) {
        state.slideshow = false;
        slideshowTimer.stop();
        updateInfoString();
    }
}

void Core::onPlaybackFinished() {
    if(state.slideshow) {
        nextImageSlideshow();
    }
}

void Core::syncRandomizer() {
    if(model) {
        randomizer.setCount(model->fileCount());
        randomizer.shuffle();
        randomizer.setCurrent(model->indexOfFile(state.currentFilePath));
    }
}

void Core::onModelLoaded() {
    thumbPanelPresenter.reloadModel();
    folderViewPresenter.reloadModel();
    thumbPanelPresenter.selectAndFocus(state.currentFilePath);
    folderViewPresenter.selectAndFocus(state.currentFilePath);
    if(state.shuffle)
        syncRandomizer();
}

void Core::onDirectoryViewFileActivated(const QString &filePath) {
    // we aren`t using async load so it won't flicker with empty view
    state.viewMode = MODE_DOCUMENT;
    loadPath(filePath);
}

// the mode is Core's decision, recorded in state; the window subscribes and
// runs the view dance on change. The load-coupled surface cleanup (hiding the
// overlays for a newly loaded folder) lives at the load site, not in this
// transition.
void Core::enterMode(ViewMode mode) {
    state.viewMode = mode;
}

void Core::enableFolderView() {
    if(state.viewMode.value() == MODE_FOLDERVIEW)
        return;
    stopSlideshow();
    enterMode(MODE_FOLDERVIEW);
}

void Core::enableDocumentView() {
    if(state.viewMode.value() == MODE_DOCUMENT)
        return;
    enterMode(MODE_DOCUMENT);
    if(model && model->fileCount() && state.currentFilePath == "") {
        const QString selected = folderViewPresenter.selectedPaths().value(0);
        // if it is a directory - ignore and just open the first file
        if(model->containsFile(selected))
            loadPath(selected);
        else
            loadPath(model->firstFile());
    }
}

void Core::toggleFolderView() {
    if(state.viewMode.value() == MODE_FOLDERVIEW)
        enableDocumentView();
    else
        enableFolderView();
}

void Core::sortBy(SortingMode mode) {
    model->setSortingMode(mode);
}

void Core::setFoldersDisplay(bool mode) {
    if(folderViewPresenter.showDirs() != mode)
        folderViewPresenter.setShowDirs(mode);
}

void Core::onFileRemoved(const QString &filePath, int index) {
    // no files left
    if(model->isEmpty()) {
        view_->closeImage();
        state.hasActiveImage = false;
        state.currentFilePath = "";
    }
    // image mode && removed current file
    if(state.currentFilePath == filePath) {
        if(state.viewMode.value() == MODE_DOCUMENT) {
            if(!loadFileIndex(index, true, settings->usePreloader()))
                loadFileIndex(--index, true, settings->usePreloader());
        } else {
            state.hasActiveImage = false;
            state.currentFilePath = "";
        }
    }
    updateInfoString();
}

void Core::onFileAdded(const QString &filePath) {
    Q_UNUSED(filePath)
    // update file count
    updateInfoString();
    if(model->fileCount() == 1 && state.currentFilePath == "")
        loadFileIndex(0, false, settings->usePreloader());
}

// Following the file the viewer is showing across a rename is the reason the
// rename is one event: the Removed half of the old pair had already bounced
// currentFilePath to "" by the time the Added half arrived, and only a rename
// the app itself issued could be patched back.
void Core::onFileRenamed(const QString &filePath, const QString &newPath, int oldIndex) {
    Q_UNUSED(oldIndex)
    if(state.currentFilePath == filePath)
        loadPath(newPath);
    updateInfoString();
}

// !! fixme
void Core::onFileModified(const QString &filePath) {
    Q_UNUSED(filePath)
}

void Core::sortByName() {
    auto mode = SortingMode::SORT_NAME;
    if(model->sortingMode() == mode)
        mode = SortingMode::SORT_NAME_DESC;
    model->setSortingMode(mode);
}

void Core::sortByTime() {
    auto mode = SortingMode::SORT_TIME;
    if(model->sortingMode() == mode)
        mode = SortingMode::SORT_TIME_DESC;
    model->setSortingMode(mode);
}

void Core::sortBySize() {
    auto mode = SortingMode::SORT_SIZE;
    if(model->sortingMode() == mode)
        mode = SortingMode::SORT_SIZE_DESC;
    model->setSortingMode(mode);
}

bool Core::loadPath(QString path) {
    if(path.isEmpty())
        return false;
    if(path.startsWith("file://", Qt::CaseInsensitive))
        path.remove(0, 7);

    stopSlideshow();
    state.delayModel = false;
    QFileInfo fileInfo(path);
    // the target lives in a local until the model has accepted it — the
    // state.directoryPath write is the commit point inside setDirectory, and
    // the folder view re-derives from that write's notify. Writing the
    // property here would move the view before the load could be refused.
    QString dir;
    if(fileInfo.isDir()) {
        dir = QDir(path).absolutePath();
    } else if(fileInfo.isFile()) {
        dir = fileInfo.absolutePath();
        // delayModel now means exactly one thing: the GUI is still hidden and
        // the first ready item must raise it. It no longer defers the listing
        // — the open path below arms to the landing instead of racing it
        state.delayModel = (model->directoryPath() != dir);
    } else {
        overlays_->showError(tr("Could not open path: ") + path);
        qDebug() << "Could not open path: " << path;
        return false;
    }
    if(!setDirectory(dir))
        return false;

    // load file / folderview
    if(fileInfo.isFile()) {
        // membership is the listing's decision alone: a file the engine's
        // filter does not admit is not openable, no mime-sniff rescue, no
        // local insert (forceInsert retired 2026-11 — the mirror has no
        // writers besides the apply path)
        enterMode(MODE_DOCUMENT);
        int index = model->indexOfFile(fileInfo.absoluteFilePath());
        if(index != -1)
            return loadFileIndex(index, false, settings->usePreloader());
        // cold open: this directory's listing has not landed yet (or has not
        // started). Start the scan and arm the open to its landing — the same
        // whenListed primitive the folder-boundary loads ride on. The old
        // code bootstrapped this moment through forceInsert's local write;
        // that exception is what hid the fact that the open path had no
        // landing-driven form at all.
        const QString filePath = fileInfo.absoluteFilePath();
        state.pendingListedToken = model->whenListed(dir, [this, filePath] {
            state.pendingListedToken = 0;
            int idx = model->indexOfFile(filePath);
            if(idx != -1)
                loadFileIndex(idx, false, settings->usePreloader());
            else
                overlays_->showError(tr("Not in this folder's listing: ") + filePath);
        });
        return true;
    } else {
        state.viewMode = MODE_FOLDERVIEW;
        return true;
    }
}

bool Core::setDirectory(const QString &path) {
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
        // state.directoryPath and left it stale.
        state.directoryPath = QDir(path).absolutePath();
    }
    return true;
}

bool Core::loadFileIndex(int index, bool async, bool preload) {
    if(!model)
        return false;
    // an explicit choice of file outranks the deferred folder-boundary load
    if(state.pendingListedToken) {
        model->cancelListed(state.pendingListedToken);
        state.pendingListedToken = 0;
    }
    auto entry = model->fileEntryAt(index);
    if(entry.path.isEmpty())
        return false;
    // direction-aware preload: decode only the side of travel
    int oldIndex = model->indexOfFile(state.currentFilePath);
    bool movingForward = (oldIndex == -1) ? true : (index >= oldIndex);
    state.currentFilePath = entry.path;
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
    updateInfoString();
    return true;
}

void Core::loadParentDir() {
    if(model->directoryPath().isEmpty() || state.viewMode.value() != MODE_FOLDERVIEW)
        return;
    stopSlideshow();
    QFileInfo currentDir(model->directoryPath());
    QFileInfo parentDir(currentDir.absolutePath());
    if(parentDir.exists() && parentDir.isReadable())
        loadPath(parentDir.absoluteFilePath());
    folderViewPresenter.selectAndFocus(currentDir.absoluteFilePath());
}

void Core::nextDirectory() {
    if(model->directoryPath().isEmpty() || state.viewMode.value() != MODE_DOCUMENT)
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
    state.pendingListedToken = model->whenListed(next, [this] {
        if(model->fileCount())
            loadFileIndex(0, false, true);
    });
}

void Core::prevDirectory(bool selectLast) {
    if(model->directoryPath().isEmpty() || state.viewMode.value() != MODE_DOCUMENT)
        return;
    QString prev = model->adjacentDirectory(model->directoryPath(), false);
    if(prev.isEmpty()) {
        return;
    }
    if(!setDirectory(prev))
        return;
    overlays_->showMessageDirectory(QFileInfo(prev).baseName());
    state.pendingListedToken = model->whenListed(prev, [this, selectLast] {
        if(model->fileCount())
            loadFileIndex(selectLast ? static_cast<int>(model->fileCount()) - 1 : 0, false, true);
    });
}

void Core::prevDirectory() {
    prevDirectory(false);
}

void Core::nextImage() {
    if(state.viewMode.value() == MODE_FOLDERVIEW || (model->isEmpty() && folderEndAction != FOLDER_END_GOTO_ADJACENT))
        return;
    stopSlideshow();
    if(state.shuffle) {
        loadFileIndex(randomizer.next(), true, false);
        return;
    }
    int newIndex = model->indexOfFile(state.currentFilePath) + 1;
    auto decision = FolderEnd::decide(folderEndAction, newIndex, model->fileCount());
    switch(decision.outcome) {
    case FolderEnd::Outcome::LoadIndex: loadFileIndex(decision.index, true, settings->usePreloader()); return;
    case FolderEnd::Outcome::EnterAdjacentFolder: nextDirectory(); return;
    case FolderEnd::Outcome::ReportBoundary:
        if(!model->loaderBusy())
            return;
    }
}

void Core::prevImage() {
    if(state.viewMode.value() == MODE_FOLDERVIEW || (model->isEmpty() && folderEndAction != FOLDER_END_GOTO_ADJACENT))
        return;
    stopSlideshow();
    if(state.shuffle) {
        loadFileIndex(randomizer.prev(), true, false);
        return;
    }

    int newIndex = model->indexOfFile(state.currentFilePath) - 1;
    auto decision = FolderEnd::decide(folderEndAction, newIndex, model->fileCount());
    switch(decision.outcome) {
    case FolderEnd::Outcome::LoadIndex: loadFileIndex(decision.index, true, settings->usePreloader()); return;
    case FolderEnd::Outcome::EnterAdjacentFolder: prevDirectory(true); return;
    case FolderEnd::Outcome::ReportBoundary:
        if(!model->loaderBusy())
            return;
    }
}

void Core::nextImageSlideshow() {
    if(model->isEmpty() || state.viewMode.value() == MODE_FOLDERVIEW)
        return;
    if(state.shuffle) {
        loadFileIndex(randomizer.next(), false, false);
    } else {
        int newIndex = model->indexOfFile(state.currentFilePath) + 1;
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

void Core::startSlideshowTimer() {
    // start timer only for static images or single frame gifs
    // for proper gifs and video we get a playbackFinished() signal
    auto img = model->getImage(state.currentFilePath);
    if(img->type() == STATIC) {
        slideshowTimer.start();
    } else if(img->type() == ANIMATED) {
        auto anim = dynamic_cast<ImageAnimated *>(img.get());
        if(anim && anim->frameCount() <= 1)
            slideshowTimer.start();
    }
}

void Core::jumpToFirst() {
    if(model->isEmpty())
        return;
    stopSlideshow();
    loadFileIndex(0, true, settings->usePreloader());
}

void Core::jumpToLast() {
    if(model->isEmpty())
        return;
    stopSlideshow();
    loadFileIndex(model->fileCount() - 1, true, settings->usePreloader());
}

void Core::onLoadFailed(const QString &path) {
    // upstream issue #505: a preload failing for a file the user is not
    // viewing used to surface "Load failed" over the image on screen —
    // noise from a file they never opened. Only the file the view actually
    // needs closed may announce its own failure; everything else is a debug
    // note (navigating to it retries the load and reports then).
    if(path != state.currentFilePath) {
        qDebug() << "Load failed (not on screen):" << path;
        return;
    }
    overlays_->showMessage(tr("Load failed: ") + path);
    mw->closeImage();
}

void Core::onModelItemReady(std::shared_ptr<Image> img, const QString &path, bool scaled) {
    if(path == state.currentFilePath) {
        guiSetImage(img);
        updateInfoString();
        if(state.delayModel) {
            this->showGui();
            state.delayModel = false;
        }
        model->unloadExcept(state.currentFilePath, settings->usePreloader());
        // two-tier: a scaled image on screen schedules the full-res upgrade.
        // The next flip's clearPool kills it if the user moved on.
        if(scaled)
            model->upgradeToFull(path);
    }
}

void Core::onModelItemUpdated(const QString &filePath) {
    if(filePath == state.currentFilePath) {
        guiSetImage(model->getImage(filePath));
        updateInfoString();
    }
}

void Core::onModelSortingChanged(SortingMode mode) {
    view_->onSortingChanged(mode);
    thumbPanelPresenter.reloadModel();
    thumbPanelPresenter.selectAndFocus(state.currentFilePath);
    folderViewPresenter.reloadModel();
    folderViewPresenter.selectAndFocus(state.currentFilePath);
}
