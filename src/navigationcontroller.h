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
        return loopSlideshow;
    }
    const QTimer &slideshowClock() const {
        return slideshowTimer;
    }
    void reset();

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

  private:
    // the navigation slice of State this controller writes: currentFilePath,
    // directoryPath, viewMode, slideshow, shuffle, pendingListedToken,
    // delayModel, and the two hasActiveImage clears (reset, onFileRemoved).
    // The rest of State is the coordinator's.
    State *state;
    INavigationHost *host_;
    ICoreView *view_;
    IOverlayHost *overlays_;
    DirectoryPresenter &thumbPanelPresenter;
    DirectoryPresenter &folderViewPresenter;
    DirectoryModel *model = nullptr; // arrives via attachModel

    // private state
    QTimer slideshowTimer;
    Randomizer randomizer;
    FolderEndAction folderEndAction;
    bool loopSlideshow;
};
