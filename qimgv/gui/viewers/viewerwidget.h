#pragma once

#include "gui/customwidgets/floatingwidgetcontainer.h"
#include <QVBoxLayout>
#include <QProperty>
#include <functional>
#include "gui/viewers/imageviewerv2.h"
#include "gui/viewers/videoplayerinitproxy.h"
#include "gui/overlays/videocontrolsproxy.h"
#include "gui/overlays/zoomindicatoroverlayproxy.h"
#include "gui/overlays/clickzoneoverlay.h"
#include "gui/contextmenu.h"

enum CurrentWidget {
    IMAGEVIEWER,
    VIDEOPLAYER,
    UNSET
};

enum class CanvasInputPolicy {
    All,
    Framing
};

class ViewerWidget : public FloatingWidgetContainer {
    Q_OBJECT
  public:
    // forwards to ImageViewerV2's fit-mode notifications
    QPropertyNotifier observeFitMode(std::function<void()> callback);
    explicit ViewerWidget(QWidget *parent = nullptr);
    ~ViewerWidget() override;
    QRect imageRect();
    float currentScale();
    QSize sourceSize();

    // the canvas-input policy. All: every canvas command. Framing: an
    // overlay (crop) owns the canvas mouse and fit/scroll are blocked, but
    // zoom stays available — it is how the user fine-frames the selection,
    // and the overlay re-derives its mapping from the live queries.

    void setCanvasInputPolicy(CanvasInputPolicy policy);
    // true only under All: gates the fit keys' "temporarily disabled"
    // message path and the context menu
    bool interactionEnabled();

    bool showImage(std::unique_ptr<QPixmap> pixmap);
    bool showAnimation(const std::shared_ptr<QMovie> &movie);
    void onScalingFinished(std::unique_ptr<QPixmap> scaled);
    bool isDisplaying();
    bool lockZoomEnabled();
    bool lockViewEnabled();
    ScalingFilter scalingFilter();
    CurrentWidget currentWidgetType() const {
        return currentWidget;
    }
    void toggleVideoPlayback();

  private:
    QVBoxLayout layout;
    std::unique_ptr<ImageViewerV2> imageViewer;
    std::unique_ptr<VideoPlayerInitProxy> videoPlayer;
    std::unique_ptr<ContextMenu> contextMenu;
    VideoControlsProxyWrapper *videoControls;
    ZoomIndicatorOverlayProxy *zoomIndicator;
    ClickZoneOverlay *clickZoneOverlay;

    void enableImageViewer();
    void enableVideoPlayer();

    CurrentWidget currentWidget;
    CanvasInputPolicy canvasInputPolicy;
    bool mWaylandCursorWorkaround;
    QTimer cursorTimer;
    const int CURSOR_HIDE_TIMEOUT_MS = 1000;
    bool mIsFullscreen;

    void disableImageViewer();
    void disableVideoPlayer();

    QRect videoControlsArea();

    bool eventFilter(QObject *object, QEvent *event) override;

  private slots:
    void onScaleChanged(qreal);
    void onVideoPlaybackFinished();
    void onAnimationPlaybackFinished();

  signals:
    // feedback up-channel: these report what the viewer did, they are not
    // commands. Commands are methods below, gated by the canvas-input policy.
    void scalingRequested(QSize, ScalingFilter);
    void draggedOut();
    void playbackFinished();
    void showScriptSettings();

  public slots:
    bool showVideo(QString file);
    void stopPlayback();
    void setFitMode(ImageFitMode mode);
    ImageFitMode fitMode();
    void closeImage();
    void hideCursor();
    void showCursor();
    void hideCursorTimed(bool restartTimer);

    // video control
    void pauseResumePlayback();
    void seek(int pos);
    void seekRelative(int pos);
    void seekBackward();
    void seekForward();
    void frameStep();
    void frameStepBack();
    void toggleMute();
    void volumeUp();
    void volumeDown();

    // canvas commands: each carries the canvas-input policy guard. The
    // filter/grid/lock family is deliberately UNGATED — it kept working mid
    // crop under the old wiring bank, and that behavior is preserved.
    void zoomIn();
    void zoomOut();
    void zoomInCursor();
    void zoomOutCursor();
    void scrollUp();
    void scrollDown();
    void scrollLeft();
    void scrollRight();
    void fitWindow();
    void fitWidth();
    void fitOriginal();
    void toggleTransparencyGrid();
    void setFilterNearest();
    void setFilterBilinear();
    void setScalingFilter(ScalingFilter filter);
    void toggleLockZoom();
    void toggleLockView();

    void startPlayback();
    void showContextMenu();
    void hideContextMenu();
    void showContextMenu(QPoint pos);
    void onFullscreenModeChanged(bool);
    void readSettings();
    void setLoopPlayback(bool mode);

  signals:
    // the viewer's geometry moved (zoom, programmatic fit, pan) — overlays
    // that map image coordinates re-derive on either
    void canvasScaleChanged();
    void canvasViewportChanged();

  protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    void leaveEvent(QEvent *event) override;
    bool focusNextPrevChild(bool mode) override;
};
