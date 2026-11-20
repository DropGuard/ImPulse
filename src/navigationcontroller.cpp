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
#include <QTimer>
#include <QDrag>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

#ifdef _WIN32
#include <tchar.h>
#endif

// How long a file must hold the view before its full-res upgrade starts. The
// dwell is the structural fix for flight-time upgrade zombies (see
// onModelItemReady): a flip inside the window means the upgrade never starts,
// rather than starting and being cancelled.
inline constexpr int UpgradeDwellMs = 400;

NavigationController::NavigationController(State *stateArg, INavigationHost *hostArg, ICoreView *viewArg,
                                           IOverlayHost *overlaysArg, DirectoryPresenter &thumbPresenterArg,
                                           DirectoryPresenter &folderPresenterArg, QObject *parent)
    : QObject(parent), m_state(stateArg), m_host(hostArg), m_view(viewArg), m_overlays(overlaysArg),
      m_thumbPanelPresenter(thumbPresenterArg), m_folderViewPresenter(folderPresenterArg),
      m_folderEndAction(FOLDER_END_NO_ACTION), m_loopSlideshow(false) {
    m_slideshowTimer.setSingleShot(true);
    // the clock's tick is this controller's own pulse — wired here, where
    // the timer lives, not in the coordinator
    connect(&m_slideshowTimer, &QTimer::timeout, this, &NavigationController::nextImageSlideshow);
    // the presenter reactions, likewise owned here: the presenters arrive as
    // constructor collaborators, so their connects live with the controller —
    // a standalone construction is fully live without coordinator-side wiring
    connect(&m_thumbPanelPresenter, &DirectoryPresenter::fileActivated, this,
            &NavigationController::onDirectoryViewFileActivated);
    connect(&m_thumbPanelPresenter, &DirectoryPresenter::dirActivated, this, &NavigationController::loadPath);
    connect(&m_folderViewPresenter, &DirectoryPresenter::fileActivated, this,
            &NavigationController::onDirectoryViewFileActivated);
    connect(&m_folderViewPresenter, &DirectoryPresenter::dirActivated, this, &NavigationController::loadPath);
}

void NavigationController::attachModel(DirectoryModel *m) {
    m_model = m;
    // the model reactions, owned by the controller that serves them: with
    // this attachment the controller is fully live — no coordinator-side
    // wiring is needed for anything navigation reacts to
    connect(m_model, &DirectoryModel::loaded, this, &NavigationController::onModelLoaded);
    connect(m_model, &DirectoryModel::imageReady, this, &NavigationController::onModelItemReady);
    connect(m_model, &DirectoryModel::imageUpdated, this, &NavigationController::onModelItemUpdated);
    connect(m_model, &DirectoryModel::sortingChanged, this, &NavigationController::onModelSortingChanged);
    connect(m_model, &DirectoryModel::loadFailed, this, &NavigationController::onLoadFailed);
    connect(m_model, &DirectoryModel::fileAdded, this, &NavigationController::onFileAdded);
    connect(m_model, &DirectoryModel::fileRemoved, this, &NavigationController::onFileRemoved);
    connect(m_model, &DirectoryModel::fileRenamed, this, &NavigationController::onFileRenamed);
    connect(m_model, &DirectoryModel::divergenceDetected, this, &NavigationController::onDivergenceDetected);
    if(m_state->shuffle)
        syncRandomizer();
}

void NavigationController::setFolderEndAction(FolderEndAction action) {
    m_folderEndAction = action;
}

void NavigationController::applySlideshowSettings(int intervalMs, bool loop) {
    m_slideshowTimer.setInterval(intervalMs);
    m_loopSlideshow = loop;
}

void NavigationController::toggleShuffle() {
    if(m_state->shuffle) {
        m_overlays->showMessage(tr("Shuffle mode: OFF"));
    } else {
        syncRandomizer();
        m_overlays->showMessage(tr("Shuffle mode: ON"));
    }
    m_state->shuffle = !m_state->shuffle;
    m_host->updateInfoString();
}

void NavigationController::toggleSlideshow() {
    if(m_state->slideshow) {
        stopSlideshow();
        m_overlays->showMessage(tr("Slideshow: OFF"));

    } else {
        startSlideshow();
        m_overlays->showMessage(tr("Slideshow: ON"));
    }
}

void NavigationController::startSlideshow() {
    if(navigationDenied())
        return;
    if(!m_state->slideshow) {
        m_state->slideshow = true;
        enableDocumentView();
        startSlideshowTimer();
        m_host->updateInfoString();
    }
}

void NavigationController::stopSlideshow() {
    if(m_state->slideshow) {
        m_state->slideshow = false;
        m_slideshowTimer.stop();
        m_host->updateInfoString();
    }
}

void NavigationController::onPlaybackFinished() {
    if(m_state->slideshow) {
        nextImageSlideshow();
    }
}

void NavigationController::syncRandomizer() {
    if(m_model) {
        m_randomizer.setCount(m_model->fileCount());
        m_randomizer.shuffle();
        m_randomizer.setCurrent(m_model->indexOfFile(m_state->currentFilePath));
    }
}

void NavigationController::onModelLoaded() {
    m_thumbPanelPresenter.reloadModel();
    m_folderViewPresenter.reloadModel();
    m_thumbPanelPresenter.selectAndFocus(m_state->currentFilePath);
    m_folderViewPresenter.selectAndFocus(m_state->currentFilePath);
    if(m_state->shuffle)
        syncRandomizer();
}

void NavigationController::onDirectoryViewFileActivated(const QString &filePath) {
    // we aren`t using async load so it won't flicker with empty view
    enterMode(MODE_DOCUMENT);
    loadPath(filePath);
}

// the mode is Core's decision, recorded in state; the window subscribes and
// runs the view dance on change. The load-coupled surface cleanup (hiding the
// overlays for a newly loaded folder) lives at the load site, not in this
// transition.
void NavigationController::enterMode(ViewMode mode) {
    m_state->viewMode = mode;
    m_host->onViewModeChanged(mode);
}

void NavigationController::enableFolderView() {
    if(navigationDenied())
        return;
    if(m_state->viewMode.value() == MODE_FOLDERVIEW)
        return;
    stopSlideshow();
    enterMode(MODE_FOLDERVIEW);
}

void NavigationController::enableDocumentView() {
    if(navigationDenied())
        return;
    if(m_state->viewMode.value() == MODE_DOCUMENT)
        return;
    enterMode(MODE_DOCUMENT);
    if(m_model && m_model->fileCount() && m_state->currentFilePath == "") {
        const QString selected = m_folderViewPresenter.selectedPaths().value(0);
        // if it is a directory - ignore and just open the first file
        if(m_model->containsFile(selected))
            loadPath(selected);
        else
            loadPath(m_model->firstFile());
    }
}

void NavigationController::toggleFolderView() {
    if(m_state->viewMode.value() == MODE_FOLDERVIEW)
        enableDocumentView();
    else
        enableFolderView();
}

void NavigationController::sortBy(SortingMode mode) {
    m_model->setSortingMode(mode);
}

void NavigationController::setFoldersDisplay(bool mode) {
    if(m_folderViewPresenter.showDirs() != mode)
        m_folderViewPresenter.setShowDirs(mode);
}

void NavigationController::onFileRemoved(const QString &filePath, int index) {
    // no files left
    if(m_model->isEmpty()) {
        m_view->closeImage();
        m_state->hasActiveImage = false;
        m_state->currentFilePath = "";
    }
    // image mode && removed current file
    if(m_state->currentFilePath == filePath) {
        if(m_state->viewMode.value() == MODE_DOCUMENT) {
            if(!loadFileIndex(index, true, settings->usePreloader()))
                loadFileIndex(--index, true, settings->usePreloader());
        } else {
            m_state->hasActiveImage = false;
            m_state->currentFilePath = "";
        }
    }
    m_host->updateInfoString();
}

void NavigationController::onFileAdded(const QString &filePath) {
    Q_UNUSED(filePath)
    // update file count
    m_host->updateInfoString();
    if(m_model->fileCount() == 1 && m_state->currentFilePath == "")
        loadFileIndex(0, false, settings->usePreloader());
}

// Following the file the viewer is showing across a rename is the reason the
// rename is one event: the Removed half of the old pair had already bounced
// currentFilePath to "" by the time the Added half arrived, and only a rename
// the app itself issued could be patched back.
void NavigationController::onFileRenamed(const QString &filePath, const QString &newPath, int oldIndex) {
    Q_UNUSED(oldIndex)
    if(m_state->currentFilePath == filePath)
        loadPathInternal(newPath);
    m_host->updateInfoString();
}

// The model edge-detects — its slot owns the mtime comparison and the
// per-(path, disk-mtime) dedup, and hands Core one divergenceDetected per
// real divergence. Core only carries the human-facing half, for the file on
// screen; a self-save never reaches this (Image::save refreshes the recorded
// mtime, so the model sees no edge at all).
// the pending-work lock: any registered surface holding unfinished work
// denies user-facing navigation with feedback. Internal convergence paths
// (the rename follow, the external-change reload) bypass the gate — they are
// self-healing, not user intent.
bool NavigationController::navigationDenied() {
    // pending work has two homes: interactive surfaces (the registry), and
    // the current image's uncommitted edits — the save prompt may be
    // settings-off, but the work exists regardless of its rendering
    const bool pending = m_overlays->hasPendingSurface() || [&] {
        // no file under attention yet (the first load): nothing can be edited,
        // and getImage() would decode whatever path it was handed — an empty
        // one, which logs "cannot open" on every startup
        if(!m_model || m_state->currentFilePath.value().isEmpty())
            return false;
        auto img = m_model->getImage(m_state->currentFilePath);
        return img && img->isEdited();
    }();
    if(pending) {
        m_overlays->showMessage(tr("Commit or discard your changes first."));
        return true;
    }
    return false;
}

void NavigationController::onDivergenceDetected(const QString &filePath) {
    if(filePath == m_state->currentFilePath)
        m_overlays->showMessage(tr("File changed on disk — your edits are kept in memory."));
}

NavigationController::InteractionState NavigationController::interactionState() const {
    // slideshow first: it implies document view (startSlideshow enters it), so
    // the two can only be told apart by asking the clock
    if(m_state->slideshow.value())
        return InteractionState::Slideshow;
    return m_state->viewMode.value() == MODE_FOLDERVIEW ? InteractionState::FolderView : InteractionState::Document;
}

void NavigationController::refuseEdit() const {
    m_overlays->showMessage(tr("Stop the slideshow before editing."));
}

void NavigationController::sortByName() {
    auto mode = SortingMode::SORT_NAME;
    if(m_model->sortingMode() == mode)
        mode = SortingMode::SORT_NAME_DESC;
    m_model->setSortingMode(mode);
}

void NavigationController::sortByTime() {
    auto mode = SortingMode::SORT_TIME;
    if(m_model->sortingMode() == mode)
        mode = SortingMode::SORT_TIME_DESC;
    m_model->setSortingMode(mode);
}

void NavigationController::sortBySize() {
    auto mode = SortingMode::SORT_SIZE;
    if(m_model->sortingMode() == mode)
        mode = SortingMode::SORT_SIZE_DESC;
    m_model->setSortingMode(mode);
}

bool NavigationController::loadPath(const QString &path) {
    if(navigationDenied())
        return false;
    return loadPathInternal(path);
}

bool NavigationController::loadPathInternal(const QString &path) {
    if(path.isEmpty())
        return false;
    QString target = path;
    if(target.startsWith("file://", Qt::CaseInsensitive))
        target.remove(0, 7);

    stopSlideshow();
    m_state->delayModel = false;
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
        m_state->delayModel = (m_model->directoryPath() != dir);
    } else {
        m_overlays->showError(tr("Could not open target: ") + target);
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
        int index = m_model->indexOfFile(fileInfo.absoluteFilePath());
        if(index != -1)
            return loadFileIndex(index, false, settings->usePreloader());
        // cold open: this directory's listing has not landed yet (or has not
        // started). Start the scan and arm the open to its landing — the same
        // whenListed primitive the folder-boundary loads ride on. The old
        // code bootstrapped this moment through forceInsert's local write;
        // that exception is what hid the fact that the open target had no
        // landing-driven form at all.
        const QString filePath = fileInfo.absoluteFilePath();
        m_state->pendingListedToken = m_model->whenListed(dir, [this, filePath] {
            m_state->pendingListedToken = 0;
            int idx = m_model->indexOfFile(filePath);
            if(idx != -1)
                loadFileIndex(idx, false, settings->usePreloader());
            else
                m_overlays->showError(tr("Not in this folder's listing: ") + filePath);
        });
        return true;
    } else {
        enterMode(MODE_FOLDERVIEW);
        return true;
    }
}

bool NavigationController::setDirectory(const QString &path) {
    if(m_model->directoryPath() != path) {
        this->reset();
        if(!m_model->setDirectory(path)) {
            m_overlays->showError(tr("Could not load folder: ") + path);
            return false;
        }
        // the state write is the commit point: the folder view re-derives
        // from this notify, so it must not fire before the model has accepted
        // the directory — a refused load leaves the view where it was. This
        // replaces the direct mw->setDirectoryPath push, and also writes
        // state on the next/prev directory paths, which never touched
        // state->directoryPath and left it stale.
        m_state->directoryPath = QDir(path).absolutePath();
    }
    return true;
}

bool NavigationController::loadFileIndex(int index, bool async, bool preload) {
    if(!m_model)
        return false;
    // an explicit choice of file outranks the deferred folder-boundary load
    if(m_state->pendingListedToken) {
        m_model->cancelListed(m_state->pendingListedToken);
        m_state->pendingListedToken = 0;
    }
    auto entry = m_model->fileEntryAt(index);
    if(entry.path.isEmpty())
        return false;
    // direction-aware preload: decode only the side of travel
    int oldIndex = m_model->indexOfFile(m_state->currentFilePath);
    bool movingForward = (oldIndex == -1) ? true : (index >= oldIndex);
    m_state->currentFilePath = entry.path;
    m_model->unloadExcept(entry.path, preload);
    if(async) {
        // the flip window: the stop point promotes over everything the flight
        // left queued, the direction neighbor survives, the rest dies — a
        // queued job has not started, so dropping it costs nothing decoded
        m_model->loadFocused(entry.path, movingForward, preload);
    } else {
        m_model->load(entry.path, false);
        if(preload) {
            if(movingForward)
                m_model->preload(m_model->nextOf(entry.path));
            else
                m_model->preload(m_model->prevOf(entry.path));
        }
    }
    m_thumbPanelPresenter.selectAndFocus(entry.path);
    m_folderViewPresenter.selectAndFocus(entry.path);
    m_host->updateInfoString();
    return true;
}

void NavigationController::loadParentDir() {
    if(navigationDenied())
        return;
    if(m_model->directoryPath().isEmpty() || m_state->viewMode.value() != MODE_FOLDERVIEW)
        return;
    stopSlideshow();
    QFileInfo currentDir(m_model->directoryPath());
    QFileInfo parentDir(currentDir.absolutePath());
    if(parentDir.exists() && parentDir.isReadable())
        loadPath(parentDir.absoluteFilePath());
    m_folderViewPresenter.selectAndFocus(currentDir.absoluteFilePath());
}

void NavigationController::nextDirectory() {
    if(navigationDenied())
        return;
    if(m_model->directoryPath().isEmpty() || m_state->viewMode.value() != MODE_DOCUMENT)
        return;
    stopSlideshow();
    QString next = m_model->adjacentDirectory(m_model->directoryPath(), true);
    if(next.isEmpty()) {
        return;
    }
    if(!setDirectory(next))
        return;
    m_overlays->showMessageDirectory(QFileInfo(next).baseName());
    // the new folder's listing is in flight; loading index 0 here would show a
    // file of the folder we just left — defer until it lands
    m_state->pendingListedToken = m_model->whenListed(next, [this] {
        if(m_model->fileCount())
            loadFileIndex(0, false, true);
    });
}

void NavigationController::prevDirectory(bool selectLast) {
    if(navigationDenied())
        return;
    if(m_model->directoryPath().isEmpty() || m_state->viewMode.value() != MODE_DOCUMENT)
        return;
    QString prev = m_model->adjacentDirectory(m_model->directoryPath(), false);
    if(prev.isEmpty()) {
        return;
    }
    if(!setDirectory(prev))
        return;
    m_overlays->showMessageDirectory(QFileInfo(prev).baseName());
    m_state->pendingListedToken = m_model->whenListed(prev, [this, selectLast] {
        if(m_model->fileCount())
            loadFileIndex(selectLast ? static_cast<int>(m_model->fileCount()) - 1 : 0, false, true);
    });
}

void NavigationController::prevDirectory() {
    if(navigationDenied())
        return;
    prevDirectory(false);
}

void NavigationController::nextImage() {
    if(navigationDenied())
        return;
    if(m_state->viewMode.value() == MODE_FOLDERVIEW ||
       (m_model->isEmpty() && m_folderEndAction != FOLDER_END_GOTO_ADJACENT))
        return;
    stopSlideshow();
    if(m_state->shuffle) {
        loadFileIndex(m_randomizer.next(), true, false);
        return;
    }
    int newIndex = m_model->indexOfFile(m_state->currentFilePath) + 1;
    auto decision = FolderEnd::decide(m_folderEndAction, newIndex, m_model->fileCount());
    switch(decision.outcome) {
    case FolderEnd::Outcome::LoadIndex: loadFileIndex(decision.index, true, settings->usePreloader()); return;
    case FolderEnd::Outcome::EnterAdjacentFolder: nextDirectory(); return;
    case FolderEnd::Outcome::ReportBoundary:
        if(!m_model->loaderBusy())
            return;
    }
}

void NavigationController::prevImage() {
    if(navigationDenied())
        return;
    if(m_state->viewMode.value() == MODE_FOLDERVIEW ||
       (m_model->isEmpty() && m_folderEndAction != FOLDER_END_GOTO_ADJACENT))
        return;
    stopSlideshow();
    if(m_state->shuffle) {
        loadFileIndex(m_randomizer.prev(), true, false);
        return;
    }

    int newIndex = m_model->indexOfFile(m_state->currentFilePath) - 1;
    auto decision = FolderEnd::decide(m_folderEndAction, newIndex, m_model->fileCount());
    switch(decision.outcome) {
    case FolderEnd::Outcome::LoadIndex: loadFileIndex(decision.index, true, settings->usePreloader()); return;
    case FolderEnd::Outcome::EnterAdjacentFolder: prevDirectory(true); return;
    case FolderEnd::Outcome::ReportBoundary:
        if(!m_model->loaderBusy())
            return;
    }
}

void NavigationController::nextImageSlideshow() {
    if(m_overlays->hasPendingSurface())
        return; // the clock tick is not user intent — denied silently
    if(m_model->isEmpty() || m_state->viewMode.value() == MODE_FOLDERVIEW)
        return;
    if(m_state->shuffle) {
        loadFileIndex(m_randomizer.next(), false, false);
    } else {
        int newIndex = m_model->indexOfFile(m_state->currentFilePath) + 1;
        if(newIndex >= m_model->fileCount()) {
            if(m_loopSlideshow) {
                newIndex = 0;
            } else {
                stopSlideshow();
                m_overlays->showMessage(tr("End of directory."));
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
    auto img = m_model->getImage(m_state->currentFilePath);
    if(img->type() == STATIC) {
        m_slideshowTimer.start();
    } else if(img->type() == ANIMATED) {
        auto anim = dynamic_cast<ImageAnimated *>(img.get());
        if(anim && anim->frameCount() <= 1)
            m_slideshowTimer.start();
    }
}

void NavigationController::jumpToFirst() {
    if(navigationDenied())
        return;
    if(m_model->isEmpty())
        return;
    stopSlideshow();
    loadFileIndex(0, true, settings->usePreloader());
}

void NavigationController::jumpToLast() {
    if(navigationDenied())
        return;
    if(m_model->isEmpty())
        return;
    stopSlideshow();
    loadFileIndex(m_model->fileCount() - 1, true, settings->usePreloader());
}

void NavigationController::onLoadFailed(const QString &path) {
    // upstream issue #505: a preload failing for a file the user is not
    // viewing used to surface "Load failed" over the image on screen —
    // noise from a file they never opened. Only the file the view actually
    // needs closed may announce its own failure; everything else is a debug
    // note (navigating to it retries the load and reports then).
    if(path != m_state->currentFilePath) {
        qDebug() << "Load failed (not on screen):" << path;
        return;
    }
    m_overlays->showMessage(tr("Load failed: ") + path);
    m_view->closeImage(); // the seam, not the window: headless Core lands here too
}

void NavigationController::onModelItemReady(std::shared_ptr<Image> img, const QString &path, bool scaled) {
    if(path == m_state->currentFilePath) {
        m_host->guiSetImage(img);
        m_host->updateInfoString();
        if(m_state->delayModel) {
            m_host->showGui();
            m_state->delayModel = false;
        }
        m_model->unloadExcept(m_state->currentFilePath, settings->usePreloader());
        // two-tier: a scaled image on screen schedules the full-res upgrade —
        // but only after the file has held the view for the dwell. Starting it
        // immediately made every intermediate file of a flight queue an
        // upgrade the next flip renders stale: measured at 5-19 zombies per
        // flight, each burning 150-250ms of decode on exactly the cores the
        // flight's own decodes need (flipbench on taskset-constrained cores,
        // 2026-11-19 — per-flip display latency ran to ~1s at 2 cores). The
        // timer's currency check makes a superseded file drop out without
        // ever starting a decode; at rest the dwell is the only cost, and it
        // is invisible behind the tier-1 image already on screen.
        if(scaled)
            QTimer::singleShot(UpgradeDwellMs, this, [this, path] {
                if(m_state->currentFilePath == path)
                    m_model->upgradeToFull(path);
            });
    }
}

void NavigationController::onModelItemUpdated(const QString &filePath) {
    if(filePath == m_state->currentFilePath) {
        m_host->guiSetImage(m_model->getImage(filePath));
        m_host->updateInfoString();
    }
}

void NavigationController::onModelSortingChanged(SortingMode mode) {
    m_view->onSortingChanged(mode);
    m_thumbPanelPresenter.reloadModel();
    m_thumbPanelPresenter.selectAndFocus(m_state->currentFilePath);
    m_folderViewPresenter.reloadModel();
    m_folderViewPresenter.selectAndFocus(m_state->currentFilePath);
}

void NavigationController::reset() {
    m_state->hasActiveImage = false;
    m_state->currentFilePath = "";
    if(m_state->pendingListedToken) {
        m_model->cancelListed(m_state->pendingListedToken);
        m_state->pendingListedToken = 0;
    }
    m_model->setDirectory("");
}
