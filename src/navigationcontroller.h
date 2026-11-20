#pragma once

// Core's navigation, as an object — see the .cpp header for the ownership
// map. The controller writes the navigation slice of State and calls the
// coordinator's kernel back through INavigationHost; it owns nothing about
// the display sink or the info projection.

#include <QObject>
#include <QTimer>
#include <memory>

#include "settings.h" // ViewMode, SortingMode
#include "utils/folderend.h"
#include "utils/randomizer.h"

class State;
class Image;
class DirectoryModel;
class DirectoryPresenter;
class ICoreView;
class IOverlayHost;

// what the controller needs back from the coordinator: the kernel functions
// that are Core's identity — the display sink, the info projection, the
// window shower. Core implements this; the controller never reaches into
// Core itself.
class INavigationHost {
  public:
    virtual ~INavigationHost() = default;
    virtual void guiSetImage(std::shared_ptr<Image> img) = 0;
    virtual void updateInfoString() = 0;
    virtual void showGui() = 0;
    // the view mode changed: the coordinator owns what follows from it (the
    // window dance, the keymap scope). Navigation writes the state; it does
    // not own its consumers.
    virtual void onViewModeChanged(ViewMode mode) = 0;
};

class NavigationController : public QObject {
    Q_OBJECT
  public:
    explicit NavigationController(State *state, INavigationHost *host, ICoreView *view, IOverlayHost *overlays,
                                  DirectoryPresenter &thumbPanelPresenter, DirectoryPresenter &folderViewPresenter,
                                  QObject *parent = nullptr);
    // the listing materializes on first load — the model arrives then
    void attachModel(DirectoryModel *model);
    // the coordinator pushes settings-derived policy on settingsChanged
    void setFolderEndAction(FolderEndAction action);
    void applySlideshowSettings(int intervalMs, bool loop);
    void syncRandomizer();
    // Whether the slideshow loops. Read-only: the behavior under test is the
    // slideshow reacting to the setting, not a caller flipping it directly.
    bool slideshowLoops() const {
        return m_loopSlideshow;
    }
    const QTimer &slideshowClock() const {
        return m_slideshowTimer;
    }
    void reset();

    // The state the window is in, for the decisions that depend on it. Today
    // it is a fold of two stored properties (viewMode, slideshow), and the
    // fold is total because the transitions keep folderView+slideshow
    // unreachable: startSlideshow enters document view, enableFolderView and
    // the folder load path stop the clock first. Making the fold a name is
    // what lets a caller branch on the STATE instead of probing a timer.
    //
    // It answers only "what is on screen", never "what may this command do":
    // each command owns its own contract (crop needs one image on screen,
    // resize needs the result to be VISIBLE, flip writes either way).
    enum class InteractionState {
        FolderView,
        Document,
        Slideshow
    };
    InteractionState interactionState() const;

    // Says why an edit cannot proceed, for the states that refuse one. The
    // decision is the caller's (`interactionState() == Slideshow`); this only
    // speaks, so the reason lives in one place. The clock's owner is the party
    // that would destroy the work (the next tick's unloadExcept), so it is the
    // party that explains itself.
    void refuseEdit() const;

  public slots:
    bool loadPath(const QString &path);
    bool setDirectory(const QString &path);
    void nextImage();
    void prevImage();
    void nextImageSlideshow();
    void jumpToFirst();
    void jumpToLast();
    void toggleSlideshow();
    void startSlideshow();
    void stopSlideshow();
    void toggleShuffle();
    void enableFolderView();
    void enableDocumentView();
    void toggleFolderView();
    void sortBy(SortingMode mode);
    void sortByName();
    void sortByTime();
    void sortBySize();
    void setFoldersDisplay(bool mode);
    void loadParentDir();
    void nextDirectory();
    // the two adjacent-folder commands; the bool is "land on the last file"
    void prevDirectory(bool selectLast);
    void prevDirectory();
    void onDirectoryViewFileActivated(const QString &filePath);
    void onPlaybackFinished();
    void onModelLoaded();
    void onModelItemReady(std::shared_ptr<Image> img, const QString &path, bool scaled);
    void onModelItemUpdated(const QString &filePath);
    void onModelSortingChanged(SortingMode mode);
    void onLoadFailed(const QString &path);
    void onFileRemoved(const QString &filePath, int index);
    void onFileAdded(const QString &filePath);
    void onFileRenamed(const QString &filePath, const QString &newPath, int oldIndex);
    void onDivergenceDetected(const QString &filePath);

  private slots:
    void enterMode(ViewMode mode);
    bool loadFileIndex(int index, bool async, bool preload);
    void startSlideshowTimer();
    bool navigationDenied();
    bool loadPathInternal(const QString &path);

  private:
    // the navigation slice of State this controller writes: currentFilePath,
    // directoryPath, viewMode, slideshow, shuffle, pendingListedToken,
    // delayModel, and the two hasActiveImage clears (reset, onFileRemoved).
    // The rest of State is the coordinator's.
    State *m_state;
    INavigationHost *m_host;
    ICoreView *m_view;
    IOverlayHost *m_overlays;
    DirectoryPresenter &m_thumbPanelPresenter;
    DirectoryPresenter &m_folderViewPresenter;
    DirectoryModel *m_model = nullptr; // arrives via attachModel

    // private state
    QTimer m_slideshowTimer;
    Randomizer m_randomizer;
    FolderEndAction m_folderEndAction;
    bool m_loopSlideshow;
};
