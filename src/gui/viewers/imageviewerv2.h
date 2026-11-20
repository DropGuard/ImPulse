#pragma once

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QElapsedTimer>
#include <QWheelEvent>
#include <QTimeLine>
#include <QScrollBar>
#include <QMovie>
#include <QColor>
#include <QTimer>
#include <QProperty>
#include <functional>
#include <QDebug>
#include <memory>
#include <cmath>
#include "settings.h"

enum MouseInteractionState {
    MOUSE_NONE,
    MOUSE_DRAG_BEGIN,
    MOUSE_DRAG,
    MOUSE_PAN,
    MOUSE_ZOOM,
    MOUSE_WHEEL_ZOOM
};

enum ViewLockMode {
    LOCK_NONE,
    LOCK_ZOOM,
    LOCK_ALL
};

class ImageViewerV2 : public QGraphicsView {
    Q_OBJECT
  public:
    ImageViewerV2(QWidget *parent = nullptr);
    ~ImageViewerV2() override;
    virtual ImageFitMode fitMode() const;
    // the fit mode's change notifications, for consumers that react to mode
    // switches (the keymap's inspect scope). The property itself stays private:
    // writes remain the viewer's alone.
    QPropertyNotifier observeFitMode(std::function<void()> callback) {
        return m_imageFitMode.addNotifier(std::move(callback));
    }

  public:
    virtual QRect scaledRectR() const;
    virtual float currentScale() const;
    virtual QSize sourceSize() const;
    virtual void showImage(std::unique_ptr<QPixmap> _pixmap);
    virtual void showAnimation(std::shared_ptr<QMovie> _animation);
    virtual void setScaledPixmap(std::unique_ptr<QPixmap> newFrame);
    virtual bool isDisplaying() const;

    virtual bool imageFits() const;
    bool scaledImageFits() const;
    virtual ScalingFilter scalingFilter() const;
    virtual QWidget *widget();
    bool hasAnimation() const;

    QSize scaledSizeR() const;

    void pauseResume();
    void enableDrags();
    void disableDrags();

  signals:
    void scalingRequested(QSize, ScalingFilter);
    void scaleChanged(qreal);
    void draggedOut();
    void playbackFinished();
    void animationPaused(bool);
    void frameChanged(int);
    void durationChanged(int);

  public slots:
    virtual void setFitMode(ImageFitMode mode);
    virtual void setFitOriginal();
    virtual void setFitWidth();
    virtual void setFitWindow();
    virtual void zoomIn();
    virtual void zoomOut();
    virtual void zoomInCursor();
    virtual void zoomOutCursor();
    virtual void readSettings();
    virtual void scrollUp();
    virtual void scrollDown();
    virtual void scrollLeft();
    virtual void scrollRight();
    virtual void startAnimation();
    virtual void stopAnimation();
    virtual void closeImage();
    virtual void setExpandImage(bool mode);
    virtual void show();
    virtual void hide();
    virtual void setFilterNearest();
    virtual void setFilterBilinear();
    virtual void setScalingFilter(ScalingFilter filter);
    void setLoopPlayback(bool mode);
    void toggleTransparencyGrid();

    void nextFrame();
    void prevFrame();

    bool showAnimationFrame(int frame);
    void onFullscreenModeChanged(bool mode);
    void toggleLockZoom();
    bool lockZoomEnabled();
    void toggleLockView();
    bool lockViewEnabled();

  protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void drawBackground(QPainter *painter, const QRectF &rect) override;

    bool eventFilter(QObject *o, QEvent *ev) override;
  protected slots:
    void onAnimationTimer();

  private slots:
    void requestScaling();
    void scrollToX(int x);
    void scrollToY(int y);
    void centerOnPixmap();
    void onScrollTimelineFinished();

    void onDPRChanged();

  private:
    std::shared_ptr<QPixmap> m_pixmap;
    std::unique_ptr<QPixmap> m_pixmapScaled;
    std::shared_ptr<QMovie> m_movie;
    QGraphicsPixmapItem m_pixmapItem, m_pixmapItemScaled;
    QTimer *m_animationTimer, *m_scaleTimer;
    QScrollBar *m_hs, *m_vs;
    QPoint m_mouseMoveStartPos, m_mousePressPos, m_drawPos;
    bool m_transparencyGrid, m_expandImage, m_smoothAnimatedImages, m_smoothUpscaling, m_forceFastScale, m_loopPlayback,
        m_isFullscreen, m_scrollBarWorkaround, m_useFixedZoomLevels, m_trackpadDetection;
    QList<float> m_zoomLevels;
    MouseInteractionState m_mouseInteraction;
    const int m_scrollUpdateRate = 7;
    const int m_defaultScrollDistance = 240;
    const qreal m_trackpadScrollMultiplier = 0.7;
    const qreal m_wheelScrollMultiplier = 2.0f;
    const int m_animationSpeed = 150;
    const float m_fastScaleThreshold = 1.0f;
    const int m_largeViewportSize = 2073600;
    // how many px you can move while holding RMB until it counts as a zoom attempt
    int m_zoomThreshold = 4;
    int m_dragThreshold = 10;

    bool m_dragsEnabled = true;
    bool m_wayland = false;

    float m_zoomStep = 0.1, m_dpr;
    float m_minScale, m_maxScale, m_fitWindowScale, m_expandLimit, m_lockedScale;
    QPointF m_savedViewportPos;
    ViewLockMode m_viewLock;

    QPair<QPointF, QPoint> m_zoomAnchor; // [pixmap coords, viewport coords]

    QElapsedTimer m_lastTouchpadScroll;

    // QProperty: the fit mode has more than one consumer (applyFitMode
    // internally, the keymap's inspect scope, tests) — the property notifies
    // on every write, so no funnel discipline is needed at the call sites
    QProperty<ImageFitMode> m_imageFitMode;
    ScalingFilter m_scalingFilter;
    ImageFitMode m_imageFitModeDefault;
    QGraphicsScene *m_scene;
    ImageFocusPoint m_focusIn1to1;

    QPixmap m_checkboard;

    void zoomAnchored(float newScale);
    void fitNormal();
    void fitWidth();
    void fitWindow();

    void scroll(int dx, int dy, bool animated);

    void mousePan(QMouseEvent *event);
    void mouseMoveZoom(QMouseEvent *event);
    void reset();
    void applyFitMode();

    QTimeLine *m_scrollTimeLineX, *m_scrollTimeLineY;
    void stopPosAnimation();
    QPointF sceneRoundPos(QPointF scenePoint) const;
    QRectF sceneRoundRect(QRectF sceneRect) const;
    void doZoom(float newScale);
    void swapToOriginalPixmap();
    void setZoomAnchor(QPoint viewportPos);
    void updatePixmap(std::unique_ptr<QPixmap> newPixmap);
    Qt::TransformationMode selectTransformationMode();
    void centerIfNecessary();
    void snapToEdges();
    void scrollSmooth(int dx, int dy);
    void scrollPrecise(int dx, int dy);
    void updateFitWindowScale();
    void updateFitWindowStretchScale();
    void updateMinScale();
    void fitFree(float scale);
    void applySavedViewportPos();
    void saveViewportPos();
    void lockZoom();
    void doZoomIn(bool atCursor);
    void doZoomOut(bool atCursor);
};
