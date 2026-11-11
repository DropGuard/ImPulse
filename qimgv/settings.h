#pragma once

#include <QObject>
#include <QSettings>
#include <QApplication>
#include <QStandardPaths>
#include <QDebug>
#include <QImageReader>
#include <QStringList>
#include <QColor>
#include <QPalette>
#include <QDir>
#include <QKeySequence>
#include <QMap>
#include <QFont>
#include <QFontMetrics>
#include <QVersionNumber>
#include <QThread>
#include "utils/script.h"
#include "themestore.h"
#include "settings_table.h"

enum SortingMode {
    SORT_NAME,
    SORT_NAME_DESC,
    SORT_SIZE,
    SORT_SIZE_DESC,
    SORT_TIME,
    SORT_TIME_DESC
};

enum ImageFitMode {
    FIT_WINDOW = 0,
    FIT_WIDTH = 1,
    FIT_ORIGINAL = 2,
    // 3 was FIT_WINDOW_STRETCH, retired — the slot stays reserved so stale
    // configs carrying it clamp in the loader instead of shifting FIT_FREE
    FIT_FREE = 4
};

enum PanelPosition {
    PANEL_TOP,
    PANEL_BOTTOM,
    PANEL_LEFT,
    PANEL_RIGHT
};

enum ScalingFilter {
    QI_FILTER_NEAREST,
    QI_FILTER_BILINEAR,
    QI_FILTER_CV_BILINEAR_SHARPEN,
    QI_FILTER_CV_CUBIC,
    QI_FILTER_CV_CUBIC_SHARPEN
};

enum ZoomIndicatorMode {
    INDICATOR_DISABLED,
    INDICATOR_ENABLED,
    INDICATOR_AUTO
};

enum DefaultCropAction {
    ACTION_CROP,
    ACTION_CROP_SAVE
};

enum ImageFocusPoint {
    FOCUS_TOP,
    FOCUS_CENTER,
    FOCUS_CURSOR
};

enum ImageScrolling {
    SCROLL_NONE,
    SCROLL_BY_TRACKPAD,
    SCROLL_BY_TRACKPAD_AND_WHEEL
};

enum ViewMode {
    MODE_DOCUMENT,
    MODE_FOLDERVIEW
};

enum FolderEndAction {
    FOLDER_END_NO_ACTION,
    FOLDER_END_LOOP,
    FOLDER_END_GOTO_ADJACENT
};

enum FolderViewMode {
    FV_SIMPLE,
    FV_EXTENDED,
    FV_EXT_FOLDERS
};

enum ThumbPanelStyle {
    TH_PANEL_SIMPLE,
    TH_PANEL_EXTENDED
};

class Settings : public QObject {
    Q_OBJECT
  public:
    static Settings *getInstance();
    // re-reads the theme from themeConf; public for tests and theme reloads
    void loadTheme();
    // Typed access to the declarative table (settings_table.h): read with
    // default + conversion + clamp semantics, write raw. Public because the
    // table is the typed API; the named accessors are one-line wrappers.
    template <class T> T load(const SettingDesc &d) const;
    template <class T> void store(const SettingDesc &d, const T &v);
    // Direct construction is allowed: each instance binds its own QSettings
    // identity, so a test can run against an isolated config. The application
    // keeps using getInstance().
    explicit Settings(QObject *parent = nullptr);
    ~Settings() override;
    QStringList supportedMimeTypes();
    QList<QByteArray> supportedFormats();
    QString supportedFormatsFilter();
    QString supportedFormatsRegex();
    int panelPreviewsSize();
    void setPanelPreviewsSize(int size);
    bool usePreloader();
    void setUsePreloader(bool mode);
    bool fullscreenMode();
    void setFullscreenMode(bool mode);
    ImageFitMode imageFitMode();
    void setImageFitMode(ImageFitMode mode);
    QRect windowGeometry();
    void setWindowGeometry(QRect geometry);
    bool playVideoSounds();
    void setPlayVideoSounds(bool mode);
    void setVolume(int vol);
    int volume();
    QString mpvBinary();
    void setMpvBinary(const QString &path);
    QString quickCopyDirectory();
    void setQuickCopyDirectory(const QString &dir);
    PanelPosition panelPosition();
    void setPanelPosition(PanelPosition);
    bool loopSlideshow();
    void setLoopSlideshow(bool mode);
    void readShortcuts(QMap<QString, QString> &shortcuts);
    void saveShortcuts(const QMap<QString, QString> &shortcuts);
    bool panelEnabled();
    void setPanelEnabled(bool mode);
    int lastDisplay();
    void setLastDisplay(int display);
    bool squareThumbnails();
    void setSquareThumbnails(bool mode);
    bool transparencyGrid();
    void setTransparencyGrid(bool mode);
    bool enableSmoothScroll();
    void setEnableSmoothScroll(bool mode);
    bool useThumbnailCache();
    void setUseThumbnailCache(bool mode);
    QStringList savedPaths();
    void setSavedPaths(const QStringList &paths);
    QString tmpDir();
    // single source of truth for the cache location, usable before any
    // Settings instance exists (main installs the file log handler with it)
    static QString resolveCacheDir();
    // The pre-XDG private thumbnail store (this identity's) is removed once per
    // cache root. The legacy "qimgv" name is deliberately NOT touched: upstream
    // qimgv uses it too, and making it regenerate its library is the exact waste
    // this change removes.
    static void retirePrivateThumbnailStores(const QString &cacheRoot);

    // Importing a qimgv configuration, on the user's explicit request. This is
    // NOT automatic: qimgv and this fork shared the identity "qimgv/qimgv"
    // until 2026.11, so the directory is equally likely to be an upstream
    // installation — reading it without being asked would act on another
    // application's files.
    //
    // The outcome is one value, not a set of flags to be re-derived by the
    // caller: "imported something", "everything was already here", "there was
    // nothing to import", "it failed" are mutually exclusive answers, and the
    // dialog only has to pick a message.
    enum class ImportOutcome {
        NoSource,   // no qimgv.conf at the source
        Imported,   // at least one file landed (some may also have been skipped)
        AllSkipped, // the source had files, but every one already exists here
        Failed,     // copying was attempted and could not be completed
    };
    struct ImportReport {
        ImportOutcome outcome = ImportOutcome::NoSource;
        QStringList copied;  // file names that landed
        QStringList skipped; // file names that already existed here
        QString detail;      // source/destination path for the failure, when there is one
    };
    // The candidate directory, resolved the way QSettings would for the shared
    // legacy identity. Exposed for the dialog's "would this work" text.
    static QString qimgvConfigDir();
    // Explicit paths so the import is testable without QSettings' process-wide
    // per-file state. Copies, never moves: upstream must keep working.
    static ImportReport importQimgvConfig(const QString &sourceDir, const QString &destinationDir);

    int thumbnailerThreadCount();
    void setThumbnailerThreadCount(int count);
    bool smoothUpscaling();
    void setSmoothUpscaling(bool mode);
    void setExpandImage(bool mode);
    bool expandImage();
    ScalingFilter scalingFilter();
    void setScalingFilter(ScalingFilter mode);
    bool smoothAnimatedImages();
    void setSmoothAnimatedImages(bool mode);
    bool panelFullscreenOnly();
    void setPanelFullscreenOnly(bool mode);
    QVersionNumber lastVersion();
    void setLastVersion(QVersionNumber &ver);
    void setShowChangelogs(bool mode);
    bool showChangelogs();
    qreal backgroundOpacity();
    void setBackgroundOpacity(qreal value);
    bool blurBackground();
    void setBlurBackground(bool mode);
    void setSortingMode(SortingMode mode);
    SortingMode sortingMode();
    void readScripts(QMap<QString, Script> &scripts);
    void saveScripts(const QMap<QString, Script> &scripts);
    int folderViewIconSize();
    void setFolderViewIconSize(int value);

    bool firstRun();
    void setFirstRun(bool mode);

    void sync();
    bool cursorAutohide();
    void setCursorAutohide(bool mode);

    bool infoBarFullscreen();
    void setInfoBarFullscreen(bool mode);
    bool infoBarWindowed();
    void setInfoBarWindowed(bool mode);

    bool windowTitleExtendedInfo();
    void setWindowTitleExtendedInfo(bool mode);

    bool maximizedWindow();
    void setMaximizedWindow(bool mode);

    int expandLimit();
    void setExpandLimit(int value);

    float zoomStep();
    void setZoomStep(float value);
    int JPEGSaveQuality();
    void setJPEGSaveQuality(int value);
    void setZoomIndicatorMode(ZoomIndicatorMode mode);
    ZoomIndicatorMode zoomIndicatorMode();
    void setFocusPointIn1to1Mode(ImageFocusPoint mode);
    ImageFocusPoint focusPointIn1to1Mode();
    void setDefaultCropAction(DefaultCropAction mode);
    DefaultCropAction defaultCropAction();
    bool placesPanel();
    void setPlacesPanel(bool mode);

    bool placesPanelTreeExpanded();
    void setPlacesPanelTreeExpanded(bool mode);

    void setSlideshowInterval(int ms);
    int slideshowInterval();

    ImageScrolling imageScrolling();
    void setImageScrolling(ImageScrolling mode);

    int placesPanelWidth();
    void setPlacesPanelWidth(int width);

    ViewMode defaultViewMode();
    void setDefaultViewMode(ViewMode mode);

    FolderEndAction folderEndAction();
    void setFolderEndAction(FolderEndAction mode);

    const ColorScheme &colorScheme();
    void setColorScheme(ColorScheme scheme);
    void setColorTid(int tid);

    bool videoPlayback();
    void setVideoPlayback(bool mode);

    bool useSystemColorScheme();
    void setUseSystemColorScheme(bool mode);

    void loadStylesheet();

    bool showSaveOverlay();
    void setShowSaveOverlay(bool mode);
    bool confirmTrash();
    void setConfirmTrash(bool mode);

    FolderViewMode folderViewMode();
    void setFolderViewMode(FolderViewMode mode);

    const QMultiMap<QByteArray, QByteArray> &videoFormats() const;

    bool printLandscape();
    void setPrintLandscape(bool mode);
    bool printPdfDefault();
    void setPrintPdfDefault(bool mode);
    bool printColor();
    void setPrintColor(bool mode);
    bool printFitToPage();
    void setPrintFitToPage(bool mode);
    QString lastPrinter();
    void setLastPrinter(const QString &name);
    bool unloadThumbs();
    void setUnloadThumbs(bool mode);
    ThumbPanelStyle thumbPanelStyle();
    void setThumbPanelStyle(ThumbPanelStyle mode);

    bool jxlAnimation();
    void setJxlAnimation(bool mode);
    bool autoResizeWindow();
    void setAutoResizeWindow(bool mode);
    int autoResizeLimit();
    void setAutoResizeLimit(int percent);

    bool panelPinned();
    void setPanelPinned(bool mode);
    int memoryAllocationLimit();
    void setMemoryAllocationLimit(int limitMB);
    bool panelCenterSelection();
    void setPanelCenterSelection(bool mode);
    QString language();
    void setLanguage(const QString &lang);

    QString defaultZoomLevels();
    QString zoomLevels();
    void setZoomLevels(const QString &levels);
    bool useFixedZoomLevels();
    void setUseFixedZoomLevels(bool mode);
    bool unlockMinZoom();
    void setUnlockMinZoom(bool mode);
    bool trackpadDetection();
    void setTrackpadDetection(bool mode);

    bool clickableEdges();
    void setClickableEdges(bool mode);
    bool clickableEdgesVisible();
    void setClickableEdgesVisible(bool mode);

    float mouseScrollingSpeed();
    void setMouseScrollingSpeed(float value);

    bool showHiddenFiles();
    void setShowHiddenFiles(bool mode);

  private: // null-initialized: a directly-constructed instance is safe to destroy
    // even if its cache setup never ran (getInstance calls setupCache())
    QSettings *settingsConf = nullptr, *stateConf = nullptr, *themeConf = nullptr;
    QDir *mTmpDir = nullptr, *mConfDir = nullptr;
    ColorScheme mColorScheme;
    QMultiMap<QByteArray, QByteArray> mVideoFormatsMap; // [mimetype, format]
    void saveTheme();

    void setupCache();
    void fillVideoFormats();

  signals:
    void settingsChanged();

  public slots:
    void sendChangeNotification();
};

extern Settings *settings;
