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
    bool m_shownOnceFired = false;
    // the parent tree owns every widget; these observe. See the rule comment
    // at mainpanel.cpp ("The pointer here only observes"), generalized here.
    ViewerWidget *m_viewerWidget = nullptr;
    QHBoxLayout m_layout;
    // const by construction: the views read state, they never write it. The
    // one non-const use — wiring the notifiers — happens in the constructor
    // through the State* parameter, and never again.
    const State *m_appState = nullptr;

    bool m_showInfoBarFullscreen, m_showInfoBarWindowed;
    DocumentWidget *m_docWidget = nullptr;
    FolderViewProxy *m_folderView = nullptr;
    CentralWidget *m_centralWidget = nullptr;
    SidePanel *m_sidePanel;
    FullscreenInfoOverlayProxy *m_infoBarFullscreen;
    InfoBarProxy *m_infoBarWindowed = nullptr;
    OverlayHost *m_overlayHost = nullptr;
    DialogHost *m_dialogHost = nullptr;
    CropController *m_cropController = nullptr;

    QPropertyNotifier m_infoObserver;
    QPropertyNotifier m_slideshowObserver;
    QPropertyNotifier m_shuffleObserver;
    QPropertyNotifier m_directoryObserver;
    QPropertyNotifier m_viewModeObserver;
    // the keymap's inspect scope follows the viewer's fit mode
    QPropertyNotifier m_fitModeObserver;
    WindowShell *m_windowShell = nullptr;
    // The window's children, produced as a value instead of assigned to
    // members from inside a setup function. The point is that the creation order lives in ONE function
    // and that a missing field is caught before the window exists — by the
    // compiler for an out-of-order dependency (a local named before it is
    // declared), by -Wmissing-field-initializers for an omission (this build
    // holds zero warnings). An explicit nullptr still compiles; the
    // constructor's runtime census covers that case. The members below remain
    // the long-lived owners, filled in one place by adoptUi().
    struct Ui {
        // No default initializers on purpose: with them an omitted field is
        // indistinguishable from a deliberate null, and -Wmissing-field-
        // initializers stays quiet. Without them, leaving one out is the
        // warning that this tree's zero-warning contract turns into a failure.
        ViewerWidget *viewer;
        InfoBarProxy *infoBarWindowed;
        DocumentWidget *doc;
        DialogHost *dialog;
        FolderViewProxy *folderView;
        CentralWidget *central;
        FullscreenInfoOverlayProxy *infoBarFullscreen;
        SidePanel *sidePanel;
    };
    // Creates every child in dependency order and wires the ones that only
    // need what it created. The order lives HERE and nowhere else.
    Ui createUi();
    // The single place the members are filled from the struct, plus the two
    // steps that need `this` in a state createUi cannot see (appState's view
    // mode) or that must run after every child exists (the layout).
    void adoptUi(const Ui &ui);

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
    void wheelEvent(QWheelEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

    // bool focusNextPrevChild(bool);
  signals:
    // implements ICoreView::setLoopPlayback — Core's relay feeds this seam,
    // and the connection below hands it to viewerWidget
    void setLoopPlayback(bool) override;
    // fired exactly once, from the first show: the trigger for the heavy
    // panel assembly (setupFullUi) — the real event, not a timer guessing at
    // "is it on screen yet"
    void shownOnce();
    void fullscreenStateChanged(bool);
    void copyUrlsRequested(QList<QString>, QString);
    void moveUrlsRequested(QList<QString>, QString);
    void showFoldersChanged(bool);
    void sortingSelected(SortingMode);

    // viewerWidget -> Core channel
    void scalingRequested(QSize, ScalingFilter);
    void droppedIn(const QMimeData *, QObject *);
    void draggedOut();
    void playbackFinished();

  public slots:
    void setupFullUi();
    // viewer commands: one-liners emitting the ViewerWidget signals directly.
    // These were 17 declared signals relay-connected to the same-named viewer
    // signals — plumbing with no body of its own; ActionManager connects to
    // the slots unchanged.
    void enableFolderView();
    void enableDocumentView();
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
};
