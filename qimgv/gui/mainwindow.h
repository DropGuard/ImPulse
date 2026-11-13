#pragma once

#include <QApplication>
#include <QObject>
#include <QWidget>
#include <QHBoxLayout>
#include <QMimeData>
#include <QLocale>
#include <QProperty>
#include <QWindow>

struct State;

class WindowShell;
class OverlayHost;
class DialogHost;
class CropController;

#include "gui/customwidgets/floatingwidgetcontainer.h"
#include "gui/iuserinteractions.h"
#include "gui/icoreview.h"
#include "gui/viewers/viewerwidget.h"
#include "gui/overlays/fullscreeninfooverlayproxy.h"
#include "gui/panels/mainpanel/thumbnailstrip.h"
#include "gui/panels/sidepanel/sidepanel.h"
#include "gui/centralwidget.h"
#include "components/actionmanager/actionmanager.h"
#include "settings.h"
#include "gui/viewers/documentwidget.h"
#include "gui/folderview/folderviewproxy.h"
#include "gui/panels/infobar/infobarproxy.h"

#ifdef USE_KDE_BLUR
#include <KWindowEffects>
#endif

enum ActiveSidePanel {
    SIDEPANEL_CROP,
    SIDEPANEL_NONE
};

class MW : public FloatingWidgetContainer, public IUserInteractions, public ICoreView {
    Q_OBJECT
  public:
    explicit MW(State *state, QWidget *parent = nullptr);
    ~MW() override;
    void onScalingFinished(std::unique_ptr<const QImage> scaled) override;
    void showImage(std::shared_ptr<const QImage> image) override;
    void showAnimation(const std::shared_ptr<QMovie> &movie) override;
    void showVideo(const QString &file) override;

    FolderViewProxy *getFolderView();
    ThumbnailStripProxy *getThumbnailPanel();

    ViewMode currentViewMode();
    WindowShell *shell();
    OverlayHost *overlays();

    bool showConfirmation(const QString &title, const QString &msg) override;
    DialogResult fileReplaceDialog(const QString &source, const QString &target, FileReplaceMode mode,
                                   bool multiple) override;
    DialogHost *dialogs();
    ViewerWidget *viewer();
    CropController *crop();

  private:
    // the parent tree owns every widget; these observe. See the rule comment
    // at mainpanel.cpp ("The pointer here only observes"), generalized here.
    ViewerWidget *viewerWidget = nullptr;
    QHBoxLayout layout;
    // const by construction: the views read state, they never write it. The
    // one non-const use — wiring the notifiers — happens in the constructor
    // through the State* parameter, and never again.
    const State *appState = nullptr;

    bool showInfoBarFullscreen, showInfoBarWindowed;
    DocumentWidget *docWidget = nullptr;
    FolderViewProxy *folderView = nullptr;
    CentralWidget *centralWidget = nullptr;
    ActiveSidePanel activeSidePanel;
    SidePanel *sidePanel;
    FullscreenInfoOverlayProxy *infoBarFullscreen;
    InfoBarProxy *infoBarWindowed = nullptr;
    OverlayHost *overlayHost = nullptr;
    DialogHost *dialogHost = nullptr;
    CropController *cropController = nullptr;

    QPropertyNotifier infoObserver;
    QPropertyNotifier slideshowObserver;
    QPropertyNotifier shuffleObserver;
    QPropertyNotifier directoryObserver;
    // the keymap's inspect scope follows the viewer's fit mode
    QPropertyNotifier fitModeObserver;
    WindowShell *windowShell = nullptr;
    void setupUi();

    void mouseDoubleClickEvent(QMouseEvent *event) override;

  private slots:
    void readSettings();
    void adaptToWindowState();
    void refreshInfoViews();

  protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

    // bool focusNextPrevChild(bool);
  signals:
    void fullscreenStateChanged(bool);
    void copyUrlsRequested(QList<QString>, QString);
    void moveUrlsRequested(QList<QString>, QString);
    void showFoldersChanged(bool);
    void sortingSelected(SortingMode);

    // viewerWidget -> Core channel
    void scalingRequested(QSize, ScalingFilter);
    void droppedIn(const QMimeData *, QObject *);
    void draggedOut();
    void setLoopPlayback(bool) override;
    void playbackFinished();

  public slots:
    void setupFullUi();
    // viewer commands: one-liners emitting the ViewerWidget signals directly.
    // These were 17 declared signals relay-connected to the same-named viewer
    // signals — plumbing with no body of its own; ActionManager connects to
    // the slots unchanged.
    void enableFolderView() override;
    void enableDocumentView() override;
    void close();
    void fitWindow();
    void fitWidth();
    void fitOriginal();
    void cycleFitMode();
    void switchFitMode();
    void closeImage() override;
    void showContextMenu();
    void onSortingChanged(SortingMode) override;
    void setFilter(ScalingFilter filter);
    void setFilterNearest();
    void setFilterBilinear();
    void toggleScalingFilter();
    void toggleLockZoom();
    void toggleLockView();
    void toggleFullscreenInfoBar() override;
    void repaintView() override;
    void setUpdatesEnabled(bool enabled) override;
    void showWindowDefault() override;
    bool isWindowVisible() override;
};
