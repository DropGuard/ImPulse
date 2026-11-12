#pragma once

#include <QObject>

#include <memory>
#include <QDebug>
#include <QMutex>
#include <QClipboard>
#include <QMimeData>
#include <QFileSystemModel>
#include <QDesktopServices>
#include <QTranslator>
#include <QProperty>
#include <QLocale>
#include "appversion.h"
#include "settings.h"
#include "components/workerpool/workerpool.h"
#include "gui/iuserinteractions.h"
#include "gui/ioverlayhost.h"
#include "components/directorymodel.h"
#include "components/directorymanager/enginebridge.h"
#include "components/thumbnailer/thumbnailer.h"
#include "components/directorypresenter.h"
#include "components/scriptmanager/scriptmanager.h"
#include "gui/mainwindow.h"
#include "utils/randomizer.h"
#include "gui/dialogs/printdialog.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

// the projection Core maintains for the views: everything the title bar,
// info bars and overlays render is derived from this — views never store
// copies (upstream kept a mirror struct in MW that drifted from Core's
// state; the projection-in-state shape is what retires it)
struct CurrentInfo {
    int index = 0;
    int fileCount = 0;
    QString fileName;
    QString filePath;
    QString directoryName;
    QString directoryPath;
    QSize imageSize;
    qint64 fileSize = 0;
    bool slideshow = false;
    bool shuffle = false;
    bool edited = false;
};

// the title bar's string assembly, pure so tests can drive it without
// instantiating the window
inline QString titleSegments(const CurrentInfo &info, QString *posOut, QString *resOut, QString *sizeOut) {
    QString pos;
    if(info.fileCount)
        pos = "[ " + QString::number(info.index + 1) + "/" + QString::number(info.fileCount) + " ]";
    QString res;
    if(info.imageSize.width())
        res = QString::number(info.imageSize.width()) + " x " + QString::number(info.imageSize.height());
    QString size;
    if(info.fileSize)
        size = QLocale().formattedDataSize(info.fileSize, 1);
    if(posOut)
        *posOut = pos;
    if(resOut)
        *resOut = res;
    if(sizeOut)
        *sizeOut = size;
    return pos;
}

inline QString titleStateSuffix(bool slideshow, bool shuffle, bool zoomLock, bool viewLock) {
    QString states;
    if(slideshow)
        states.append(" [slideshow]");
    if(shuffle)
        states.append(" [shuffle]");
    if(zoomLock)
        states.append(" [zoom lock]");
    if(viewLock)
        states.append(" [view lock]");
    return states;
}

inline QString assembleWindowTitle(bool folderView, const CurrentInfo &info, bool zoomLock, bool viewLock,
                                   bool extendedInfo, bool appendStates, const QString &appName,
                                   const QString &folderViewTitle) {
    if(folderView)
        return folderViewTitle;
    if(info.fileName.isEmpty())
        return appName;
    QString pos, res, size;
    titleSegments(info, &pos, &res, &size);
    QString title = info.fileName;
    if(extendedInfo) {
        if(!pos.isEmpty())
            title.prepend(pos + "  ");
        if(!res.isEmpty())
            title.append("  -  " + res);
        if(!size.isEmpty())
            title.append("  -  " + size);
    }
    QString states = titleStateSuffix(info.slideshow, info.shuffle, zoomLock, viewLock);
    if(appendStates && !states.isEmpty())
        title.append(" -" + states);
    if(info.edited)
        title.prepend("* ");
    return title;
}

struct State {
    // scalar state lives in QProperties: bindable storage for the pull-style
    // views (P2 of the unidirectional-flow refactor), plain operator= writes
    QProperty<bool> hasActiveImage{false};
    // the view mode is Core's decision, recorded here; the window follows it.
    // Every mode read in Core's decision points is a state read — the window
    // is a subscriber, not the source of truth
    ViewMode viewMode = MODE_DOCUMENT;
    // the GUI is waiting for the first ready frame: main defers showGui while
    // set, and onModelItemReady raises the window with the image already in it
    QProperty<bool> delayModel{false};
    // armed via DirectoryManager::whenListed: which image to show on a
    // folder-boundary transition is only knowable when the listing lands
    QProperty<qint64> pendingListedToken{0};
    QProperty<QString> currentFilePath{QString()};
    QProperty<QString> directoryPath{QString()};
    // the projection the views bind to — written by updateInfoString after
    // every mutation, read by the views on invalidation
    QProperty<CurrentInfo> currentInfo;
    QProperty<bool> slideshow{false};
    QProperty<bool> shuffle{false};
};

enum MimeDataTarget {
    TARGET_CLIPBOARD,
    TARGET_DROP
};

class Core : public QObject {
    Q_OBJECT
  public:
    // What Core is built from. Every field defaults to the production choice, so
    // `Core()` still assembles a full app; a test names the ones it wants to
    // control and leaves the rest.
    //
    // This exists because the constructor used to hard-code the whole graph --
    // the engine bridge was a process-wide singleton reached for in the member
    // init list, the pool size came from the global settings, and the dialog
    // provider was whatever MainWindow happened to be. A test that wanted any
    // one of those had to build all of them, which is why the Core suites were
    // thousands of lines: every test paid for an application.
    struct Deps {
        // null -> a pool sized from the settings, as production does
        WorkerPool *pool = nullptr;
        // null -> EngineBridge::instance(), the process-wide one. A test that
        // passes its own gets an isolated engine, watch state included.
        EngineBridge *bridge = nullptr;
        // null -> the one MainWindow builds. Supplying one detaches Core from
        // the widget tree for every path that only talks to the overlays.
        IOverlayHost *overlays = nullptr;
    };

    // The no-argument form is the production assembly. Kept as a separate
    // overload rather than a defaulted parameter: a defaulted `Deps` needs a
    // default constructor, and spelling it out twice is how the two drift.
    Core();
    explicit Core(const Deps &deps);
    ~Core() override;
    void showGui();
    // the file on screen; the state Core owns and every navigation decision reads
    QString currentFilePath() const;
    bool firstFramePending() const;
    // Nothing asked of the model is outstanding and no deferred hop is armed.
    // Exists so a caller can wait instead of sleeping; it answers no question
    // about what the listing contains, so it cannot substitute for an assert.
    bool isSettled() const;

    // ---------------------------------------------------------------------
    // Seams the tests and the scripted dialogs use. These were three
    // `friend class` declarations, which meant a test could reach private state
    // only by being named in the production header -- the ownership rule in
    // reverse. Each is the narrowest thing that lets a caller answer a
    // question, and each says why it exists.
    //
    // The dialog provider: production points this at MainWindow, a test at a
    // scripted fake. Set after construction because the provider is the window
    // in production, and the window does not exist until initGui ran.
    void setUserInteractions(IUserInteractions *ui) {
        ui_ = ui;
    }
    IUserInteractions *userInteractions() const {
        return ui_;
    }
    // The overlay surface, for the same reason and with the same lifetime rule.
    void setOverlayHost(IOverlayHost *host) {
        overlays_ = host;
    }
    IOverlayHost *overlayHost() const {
        return overlays_;
    }
    // The executor Core runs on: borrowed from Deps, or the one Core owns. A
    // test that supplied a pool reads it back here to submit against the same
    // one, rather than reaching into the member.
    WorkerPool &executor() {
        return pool;
    }

    // The window tree. Public because the widget-driven tests need it, and
    // because hiding it behind a friend declaration was the only thing making
    // it private in the first place -- MainWindow is part of Core's observable
    // surface, not an implementation detail.
    MW *mainWindow() const {
        return mw;
    }

    // The directory model Core drives. Exposed read-only: a caller may inspect
    // it, not replace it (attachModel is the one writer, and it is private).
    const std::shared_ptr<DirectoryModel> &directoryModel() const {
        return model;
    }
    // The projection Core maintains -- what the title bar and overlays render.
    // Read-only for the same reason.
    const State &viewState() const {
        return state;
    }
    // Whether the slideshow loops. Read-only: the behavior under test is the
    // slideshow reacting to the setting, not a caller flipping it directly.
    bool slideshowLoops() const {
        return loopSlideshow;
    }
    // The paths currently selected, in view order. Public because every command
    // that acts on "the selection" is public and this is how a caller confirms
    // what one acted on.
    QList<QString> currentSelection();

    // The folder view's presenter, for the tests that drive grid commands and
    // assert on what the view was told. Read-only for the same reason as the
    // model.
    DirectoryPresenter &folderPresenter() {
        return folderViewPresenter;
    }

    // The slideshow's clock. Tests assert on isActive because "the timer is
    // armed" is the observable half of "a slideshow is running"; stopping it is
    // the behavior under test, not something a caller may do directly.
    const QTimer &slideshowClock() const {
        return slideshowTimer;
    }

  public slots:
    void updateInfoString();
    bool loadPath(QString);

  private:
    QElapsedTimer t;

    void initGui();
    void initComponents();
    void connectComponents();
    void initActions();
    void loadTranslation();
    void onUpdate();
    void onFirstRun();

    // ui stuff
    MW *mw;

    // The one executor Loader/Scaler/Thumbnailer run on, injected into every
    // component that needs it.
    //
    // A Deps pool is borrowed, otherwise Core owns one: `ownedPool` is empty in
    // the borrowed case, and `pool` is the reference every component was built
    // with. Declared before the components so that in the owned case the pool's
    // destruction -- the only place the workers are joined -- happens after each
    // of them has drained and waited.
    std::unique_ptr<WorkerPool> ownedPool;
    WorkerPool &pool;
    // the engine Core drives everything through: borrowed from Deps, or the
    // process-wide instance. Stored so the model is built on the same one the
    // thumbnailer got -- the two must not disagree about which engine they
    // answer to.
    EngineBridge &mBridge;

    // the dialog answers, through the interface MW implements; tests inject a
    // scripted fake here to drive the interactive command machines. Non-owning:
    // it points at mw in production and is set in initGui
    IUserInteractions *ui_ = nullptr;

    // where Core's messages and overlay commands go: MainWindow's OverlayHost in
    // production, a scripted host in a test that does not want a window. Not
    // owned -- the Deps-provided host outlives Core, and the production one is
    // a child of the window tree Core deletes.
    IOverlayHost *overlays_ = nullptr;

    State state;
    FolderEndAction folderEndAction;
    bool loopSlideshow;

    // components
    std::shared_ptr<DirectoryModel> model;

    // White-box seam for the navigation test: the direction-aware preload
    // decision lives in loadFileIndex() and is only observable through the
    // model's cache, which is deliberately not part of Core's public API.

    // the one Thumbnailer in the process, shared by both presenters (rule R9).
    // Declared after pool_ and before the presenters, so destruction runs
    // presenters → thumbnailer → pool_ and the pool outlives its drain
    Thumbnailer thumbnailer;

    DirectoryPresenter thumbPanelPresenter, folderViewPresenter;

    void rotateByDegrees(int degrees);
    void enterMode(ViewMode mode);
    void reset();
    bool setDirectory(const QString &path);

    QTranslator *translator = nullptr;

    Randomizer randomizer;
    void syncRandomizer();

    void attachModel(DirectoryModel *_model);
    QString selectedPath();
    void guiSetImage(std::shared_ptr<Image> img);
    QTimer slideshowTimer;

    void startSlideshowTimer();

    bool saveFile(const QString &filePath, const QString &newPath);
    bool saveFile(const QString &filePath);

    std::shared_ptr<ImageStatic> getEditableImage(const QString &filePath);

    template <typename... Args>
    void edit_template(bool save, const QString &actionName,
                       const std::function<std::unique_ptr<const QImage>(std::shared_ptr<const QImage>, Args...)> &func,
                       Args &&...as);

    void doInteractiveCopy(const QString &path, const QString &destDirectory, DialogResult &overwriteAllFiles);
    void doInteractiveMove(const QString &path, const QString &destDirectory, DialogResult &overwriteAllFiles);

  public slots:
    // ---------------------------------------------------------------------
    // Core's commands. These are what a keystroke ends up invoking -- the
    // ActionManager connects land here -- so they are part of Core's surface,
    // not its internals.
    //
    // They used to be private, reachable only through `friend class TestCoreNav`
    // and its successors. That made the tests the only callers who were not
    // allowed to be callers: every one of them drives a command exactly the way
    // a key press does. A friend declaration was the cost of not saying so.
    void saveCurrentFileAs(const QString &destPath);
    void crop(QRect rect);
    void cropAndSave(QRect rect);
    void copyCurrentFile(const QString &destDirectory);
    void moveCurrentFile(const QString &destDirectory);
    void interactiveCopy(const QList<QString> &paths, const QString &destDirectory);
    void interactiveMove(const QList<QString> &paths, const QString &destDirectory);
    // the two adjacent-folder commands; the bool is "land on the last file"
    void prevDirectory(bool selectLast);
    void prevDirectory();
    void startSlideshow();
    void stopSlideshow();
    void nextImage();
    void prevImage();
    void nextImageSlideshow();
    void jumpToFirst();
    void jumpToLast();
    void rotateLeft();
    void rotateRight();
    void flipH();
    void flipV();
    void discardEdits();
    void toggleCropPanel();
    void requestSavePath();
    void saveCurrentFile();
    void setWallpaper();
    void removePermanent();
    void moveToTrash();
    void copyFileClipboard();
    void openFromClipboard();
    void toggleShuffle();
    void enableFolderView();
    void toggleFolderView();
    void toggleSlideshow();
    void nextDirectory();
    // The drag-and-drop entry points a real gesture uses: a drop from outside
    // the app, what a drag out of it produces, and the paths for it.
    void onDropIn(const QMimeData *mimeData, QObject *source);
    void onDraggedOut(const QList<QString> &paths);
    QMimeData *mimeDataForPaths(const QList<QString> &paths);
    // what a drag or clipboard entry carries for one image
    QMimeData *getMimeDataForImage(std::shared_ptr<Image> img, MimeDataTarget target);
    void resize(QSize size);
    void movePathsTo(const QList<QString> &paths, const QString &destDirectory);
    void renameCurrentSelection(const QString &newName);
    // The wallpaper command dispatch, as a pure function of (session, path):
    // what the audit test pins. Public because it is the seam for that test and
    // hiding it behind a friend declaration was the only thing making it
    // private.
    static QPair<QString, QStringList> wallpaperCommand(const QString &session, const QString &path);

  private slots:
    void readSettings();
    void onModelItemReady(std::shared_ptr<Image>, const QString &, bool scaled);
    void onModelItemUpdated(const QString &fileName);
    void onModelSortingChanged(SortingMode mode);
    void onLoadFailed(const QString &path);
    void close();
    void scalingRequest(QSize, ScalingFilter);
    void onScalingFinished(QPixmap scaled, const ScalerRequest &req);
    void copyPathsTo(const QList<QString> &paths, const QString &destDirectory);
    FileOpResult removeFile(const QString &fileName, bool trash);
    void onFileRemoved(const QString &filePath, int index);
    void onFileAdded(const QString &filePath);
    void onFileModified(const QString &filePath);
    void onFileRenamed(const QString &filePath, const QString &newPath, int oldIndex);
    void showResizeDialog();
    void toggleFullscreenInfoBar();
    void runScript(const QString &);
    void reloadImage();
    void reloadImage(const QString &fileName);
    void copyPathClipboard();
    void sortBy(SortingMode mode);
    void sortByName();
    void sortByTime();
    void sortBySize();
    void showRenameDialog();
    void onDraggedOut(); // the action's no-argument form; the paths form is public
    void onModelLoaded();
    void outputError(const FileOpResult &error) const;
    void reportBatchRemoval(const QString &doneVerb, int total, int successCount) const;
    void showOpenDialog();
    void showInDirectory();
    void quickCopyToFixedDirectory();
    void onDirectoryViewFileActivated(const QString &filePath);
    bool loadFileIndex(int index, bool async, bool preload);
    void enableDocumentView();
    void onPlaybackFinished();
    void setFoldersDisplay(bool mode);
    void loadParentDir();
    void print();
};
