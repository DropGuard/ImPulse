#pragma once

#include <QObject>
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
#include "components/directorymodel.h"
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
    if(posOut) *posOut = pos;
    if(resOut) *resOut = res;
    if(sizeOut) *sizeOut = size;
    return pos;
}

inline QString titleStateSuffix(bool slideshow, bool shuffle, bool zoomLock, bool viewLock) {
    QString states;
    if(slideshow) states.append(" [slideshow]");
    if(shuffle)   states.append(" [shuffle]");
    if(zoomLock)  states.append(" [zoom lock]");
    if(viewLock)  states.append(" [view lock]");
    return states;
}

inline QString assembleWindowTitle(bool folderView, const CurrentInfo &info,
                                   bool zoomLock, bool viewLock,
                                   bool extendedInfo, bool appendStates,
                                   const QString &appName,
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
    std::shared_ptr<Image> currentImg;
};

enum MimeDataTarget {
    TARGET_CLIPBOARD,
    TARGET_DROP
};

class Core : public QObject {
    Q_OBJECT
public:
    Core();
    ~Core() override;
    void showGui();
    // the file on screen; the state Core owns and every navigation decision reads
    QString currentFilePath() const;
    bool firstFramePending() const;
    // Nothing asked of the model is outstanding and no deferred hop is armed.
    // Exists so a caller can wait instead of sleeping; it answers no question
    // about what the listing contains, so it cannot substitute for an assert.
    bool isSettled() const;

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

    // the one executor Loader/Scaler/Thumbnailer run on, injected into every
    // component that needs it. Declared before them so its destruction — the
    // only place the workers are joined — happens after each of them has
    // drained and waited
    WorkerPool pool_;

    // the dialog answers, through the interface MW implements; tests inject a
    // scripted fake here to drive the interactive command machines. Non-owning:
    // it points at mw in production and is set in initGui
    IUserInteractions *ui_ = nullptr;

    State state;
    FolderEndAction folderEndAction;
    bool loopSlideshow;

    // components
    std::shared_ptr<DirectoryModel> model;

    // White-box seam for the navigation test: the direction-aware preload
    // decision lives in loadFileIndex() and is only observable through the
    // model's cache, which is deliberately not part of Core's public API.
    friend class TestCoreNav;

    // the one Thumbnailer in the process, shared by both presenters (rule R9).
    // Declared after pool_ and before the presenters, so destruction runs
    // presenters → thumbnailer → pool_ and the pool outlives its drain
    Thumbnailer thumbnailer;

    DirectoryPresenter thumbPanelPresenter, folderViewPresenter;

    void rotateByDegrees(int degrees);
    void reset();
    bool setDirectory(const QString & path);

    QMimeData *getMimeDataForImage(std::shared_ptr<Image> img, MimeDataTarget target);
    QMimeData *mimeDataForPaths(const QList<QString> &paths);
    QTranslator *translator = nullptr;

    Randomizer randomizer;
    void syncRandomizer();

    void attachModel(DirectoryModel *_model);
    QString selectedPath();
    void guiSetImage(std::shared_ptr<Image> img);
    QTimer slideshowTimer;

    void startSlideshowTimer();
    void startSlideshow();
    void stopSlideshow();

    bool saveFile(const QString &filePath, const QString &newPath);
    bool saveFile(const QString &filePath);

    std::shared_ptr<ImageStatic> getEditableImage(const QString &filePath);
    QList<QString> currentSelection();

    template<typename... Args>
    void edit_template(bool save, const QString & actionName, const std::function<std::unique_ptr<const QImage>(std::shared_ptr<const QImage>, Args...)>& func, Args&&... as);

    void doInteractiveCopy(const QString & path, const QString & destDirectory, DialogResult &overwriteAllFiles);
    void doInteractiveMove(const QString & path, const QString & destDirectory, DialogResult &overwriteAllFiles);

private slots:
    void readSettings();
    void nextImage();
    void prevImage();
    void nextImageSlideshow();
    void jumpToFirst();
    void jumpToLast();
    void onModelItemReady(std::shared_ptr<Image>, const QString&, bool scaled);
    void onModelItemUpdated(const QString & fileName);
    void onModelSortingChanged(SortingMode mode);
    void onLoadFailed(const QString &path);
    void rotateLeft();
    void rotateRight();
    void close();
    void scalingRequest(QSize, ScalingFilter);
    void onScalingFinished(QPixmap scaled, const ScalerRequest &req);
    void copyCurrentFile(const QString & destDirectory);
    void moveCurrentFile(const QString & destDirectory);
    void copyPathsTo(const QList<QString> & paths, const QString & destDirectory);
    void interactiveCopy(const QList<QString> & paths, const QString & destDirectory);
    void interactiveMove(const QList<QString> & paths, const QString & destDirectory);
    void movePathsTo(const QList<QString> & paths, const QString & destDirectory);
    FileOpResult removeFile(const QString & fileName, bool trash);
    void onFileRemoved(const QString & filePath, int index);
    void onFileAdded(const QString & filePath);
    void onFileModified(const QString & filePath);
    void onFileRenamed(const QString & filePath, const QString & newPath, int oldIndex);
    void showResizeDialog();
    void resize(QSize size);
    void flipH();
    void flipV();
    void crop(QRect rect);
    void cropAndSave(QRect rect);
    void discardEdits();
    void toggleCropPanel();
    void toggleFullscreenInfoBar();
    void requestSavePath();
    void saveCurrentFile();
    void saveCurrentFileAs(const QString &);
    void runScript(const QString&);
    void setWallpaper();
    // the wallpaperCommand dispatch, exposed for the audit test
    static QPair<QString, QStringList> wallpaperCommand(const QString &session, const QString &path);
    void removePermanent();
    void moveToTrash();
    void reloadImage();
    void reloadImage(const QString & fileName);
    void copyFileClipboard();
    void copyPathClipboard();
    void openFromClipboard();
    void renameCurrentSelection(const QString & newName);
    void sortBy(SortingMode mode);
    void sortByName();
    void sortByTime();
    void sortBySize();
    void showRenameDialog();
    void onDraggedOut();
    void onDraggedOut(const QList<QString> & paths);
    void onDropIn(const QMimeData *mimeData, QObject* source);
    void toggleShuffle();
    void onModelLoaded();
    void outputError(const FileOpResult &error) const;
    void showOpenDialog();
    void showInDirectory();
    void quickCopyToFixedDirectory();
    void onDirectoryViewFileActivated(const QString & filePath);
    bool loadFileIndex(int index, bool async, bool preload);
    void enableDocumentView();
    void enableFolderView();
    void toggleFolderView();
    void toggleSlideshow();
    void onPlaybackFinished();
    void setFoldersDisplay(bool mode);
    void loadParentDir();
    void nextDirectory();
    void prevDirectory(bool selectLast);
    void prevDirectory();
    void print();
};
