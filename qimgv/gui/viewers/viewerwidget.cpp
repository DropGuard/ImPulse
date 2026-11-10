/*
 * This widget combines ImageViewer / VideoPlayer.
 * Only one is displayed at a time.
 */

#include "viewerwidget.h"

#include <QScrollBar>

#include <utility>

ViewerWidget::ViewerWidget(QWidget *parent)
    : FloatingWidgetContainer(parent), imageViewer(nullptr), videoPlayer(nullptr), contextMenu(nullptr),
      videoControls(nullptr), currentWidget(UNSET), canvasInputPolicy(CanvasInputPolicy::All),
      mWaylandCursorWorkaround(false), mIsFullscreen(false) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
#ifdef Q_OS_LINUX
    // we cant check cursor position on wayland until the mouse is moved
    // use this to skip cursor check once
    if(qgetenv("XDG_SESSION_TYPE") == "wayland")
        mWaylandCursorWorkaround = true;
#endif
    layout.setContentsMargins(0, 0, 0, 0);
    layout.setSpacing(0);
    this->setLayout(&layout);

    imageViewer.reset(new ImageViewerV2(this));
    layout.addWidget(imageViewer.get());
    imageViewer->hide();

    connect(imageViewer.get(), &ImageViewerV2::scalingRequested, this, &ViewerWidget::scalingRequested);
    connect(imageViewer.get(), &ImageViewerV2::draggedOut, this, &ViewerWidget::draggedOut);
    connect(imageViewer.get(), &ImageViewerV2::scaleChanged, this, &ViewerWidget::onScaleChanged);
    connect(imageViewer.get(), &ImageViewerV2::scaleChanged, this, &ViewerWidget::canvasScaleChanged);
    connect(imageViewer->horizontalScrollBar(), &QScrollBar::valueChanged, this, &ViewerWidget::canvasViewportChanged);
    connect(imageViewer->verticalScrollBar(), &QScrollBar::valueChanged, this, &ViewerWidget::canvasViewportChanged);
    connect(imageViewer.get(), &ImageViewerV2::playbackFinished, this, &ViewerWidget::onAnimationPlaybackFinished);

    videoPlayer.reset(new VideoPlayerInitProxy(this));
    layout.addWidget(videoPlayer.get());
    videoPlayer->hide();
    videoControls = new VideoControlsProxyWrapper(this);
    // ZoomIndicatorOverlayProxy and VideoControlsProxyWrapper are unparented
    // QObject wrappers (their ctors keep `container` as a plain member) — they
    // are this widget's C++ ownership, discharged here

    // tmp no wrapper
    zoomIndicator = new ZoomIndicatorOverlayProxy(this);
    clickZoneOverlay = new ClickZoneOverlay(this);

    connect(videoPlayer.get(), &VideoPlayer::playbackFinished, this, &ViewerWidget::onVideoPlaybackFinished);

    connect(videoControls, &VideoControlsProxyWrapper::seekBackward, this, &ViewerWidget::seekBackward);
    connect(videoControls, &VideoControlsProxyWrapper::seekForward, this, &ViewerWidget::seekForward);
    connect(videoControls, &VideoControlsProxyWrapper::seek, this, &ViewerWidget::seek);

    enableImageViewer();
    setCanvasInputPolicy(CanvasInputPolicy::All);

    connect(&cursorTimer, &QTimer::timeout, this, &ViewerWidget::hideCursor);

    connect(settings, &Settings::settingsChanged, this, &ViewerWidget::readSettings);
    readSettings();
}
ViewerWidget::~ViewerWidget() {
    // unparented QObject wrappers are this widget's C++ ownership; their inner
    // overlays are Qt-parented here and go with the tree sweep that follows
    delete videoControls;
    delete zoomIndicator;
}

QRect ViewerWidget::imageRect() {
    if(imageViewer && currentWidget == IMAGEVIEWER)
        return imageViewer->scaledRectR();
    else
        return QRect(0, 0, 0, 0);
}

float ViewerWidget::currentScale() {
    if(currentWidget == IMAGEVIEWER)
        return imageViewer->currentScale();
    else
        return 1.0f;
}

QSize ViewerWidget::sourceSize() {
    if(currentWidget == IMAGEVIEWER)
        return imageViewer->sourceSize();
    else
        return QSize(0, 0);
}

// hide videoPlayer, show imageViewer
void ViewerWidget::enableImageViewer() {
    if(currentWidget != IMAGEVIEWER) {
        disableVideoPlayer();
        videoControls->setMode(PLAYBACK_ANIMATION);
        connect(imageViewer.get(), &ImageViewerV2::durationChanged, videoControls,
                &VideoControlsProxyWrapper::setPlaybackDuration);
        connect(imageViewer.get(), &ImageViewerV2::frameChanged, videoControls,
                &VideoControlsProxyWrapper::setPlaybackPosition);
        connect(imageViewer.get(), &ImageViewerV2::animationPaused, videoControls,
                &VideoControlsProxyWrapper::onPlaybackPaused);
        imageViewer->show();
        currentWidget = IMAGEVIEWER;
    }
}

// hide imageViewer, show videoPlayer
void ViewerWidget::enableVideoPlayer() {
    if(currentWidget != VIDEOPLAYER) {
        disableImageViewer();
        videoControls->setMode(PLAYBACK_VIDEO);
        connect(videoPlayer.get(), &VideoPlayer::durationChanged, videoControls,
                &VideoControlsProxyWrapper::setPlaybackDuration);
        connect(videoPlayer.get(), &VideoPlayer::positionChanged, videoControls,
                &VideoControlsProxyWrapper::setPlaybackPosition);
        connect(videoPlayer.get(), &VideoPlayer::videoPaused, videoControls,
                &VideoControlsProxyWrapper::onPlaybackPaused);
        videoPlayer->show();
        currentWidget = VIDEOPLAYER;
    }
}

void ViewerWidget::disableImageViewer() {
    if(currentWidget == IMAGEVIEWER) {
        currentWidget = UNSET;
        imageViewer->closeImage();
        imageViewer->hide();
        zoomIndicator->hide();
        disconnect(imageViewer.get(), &ImageViewerV2::durationChanged, videoControls,
                   &VideoControlsProxyWrapper::setPlaybackDuration);
        disconnect(imageViewer.get(), &ImageViewerV2::frameChanged, videoControls,
                   &VideoControlsProxyWrapper::setPlaybackPosition);
        disconnect(imageViewer.get(), &ImageViewerV2::animationPaused, videoControls,
                   &VideoControlsProxyWrapper::onPlaybackPaused);
    }
}

void ViewerWidget::disableVideoPlayer() {
    if(currentWidget == VIDEOPLAYER) {
        currentWidget = UNSET;
        //videoControls->hide();
        disconnect(videoPlayer.get(), &VideoPlayer::durationChanged, videoControls,
                   &VideoControlsProxyWrapper::setPlaybackDuration);
        disconnect(videoPlayer.get(), &VideoPlayer::positionChanged, videoControls,
                   &VideoControlsProxyWrapper::setPlaybackPosition);
        disconnect(videoPlayer.get(), &VideoPlayer::videoPaused, videoControls,
                   &VideoControlsProxyWrapper::onPlaybackPaused);
        videoPlayer->setPaused(true);
        // even after calling hide() the player sends a few video frames
        // which paints over the imageviewer, causing corruption
        // so we do not HIDE it, but rather just cover it by imageviewer's widget
        // seems to work fine, might even feel a bit snappier
        if(!videoPlayer->isInitialized())
            videoPlayer->hide();
    }
}

void ViewerWidget::onScaleChanged(qreal scale) {
    if(!this->isVisible())
        return;
    if(scale != 1.0f) {
        zoomIndicator->setScale(scale);
        if(settings->zoomIndicatorMode() == ZoomIndicatorMode::INDICATOR_ENABLED)
            zoomIndicator->show();
        else if((settings->zoomIndicatorMode() == ZoomIndicatorMode::INDICATOR_AUTO))
            zoomIndicator->show(1500);
    } else {
        zoomIndicator->hide();
    }
}

void ViewerWidget::onVideoPlaybackFinished() {
    if(currentWidget == VIDEOPLAYER)
        emit playbackFinished();
}

void ViewerWidget::onAnimationPlaybackFinished() {
    if(currentWidget == IMAGEVIEWER)
        emit playbackFinished();
}

void ViewerWidget::setCanvasInputPolicy(CanvasInputPolicy policy) {
    if(canvasInputPolicy == policy)
        return;
    canvasInputPolicy = policy;
    imageViewer->setAttribute(Qt::WA_TransparentForMouseEvents, policy != CanvasInputPolicy::All);
    if(policy != CanvasInputPolicy::All)
        hideContextMenu();
}

// ---- canvas commands ----
// every user-facing canvas command carries the policy guard; the filter /
// grid / lock family is ungated on purpose (it worked mid crop under the old
// wiring bank, and that behavior is preserved verbatim)
void ViewerWidget::zoomIn() {
    // zoom survives Framing: it is the fine-adjustment affordance of the
    // crop flow, and the overlay re-derives its mapping from live queries
    imageViewer->zoomIn();
}

void ViewerWidget::zoomOut() {
    // zoom survives Framing: it is the fine-adjustment affordance of the
    // crop flow, and the overlay re-derives its mapping from live queries
    imageViewer->zoomOut();
}

void ViewerWidget::zoomInCursor() {
    // zoom survives Framing: it is the fine-adjustment affordance of the
    // crop flow, and the overlay re-derives its mapping from live queries
    imageViewer->zoomInCursor();
}

void ViewerWidget::zoomOutCursor() {
    // zoom survives Framing: it is the fine-adjustment affordance of the
    // crop flow, and the overlay re-derives its mapping from live queries
    imageViewer->zoomOutCursor();
}

void ViewerWidget::scrollUp() {
    imageViewer->scrollUp();
}

void ViewerWidget::scrollDown() {
    imageViewer->scrollDown();
}

void ViewerWidget::scrollLeft() {
    imageViewer->scrollLeft();
}

void ViewerWidget::scrollRight() {
    imageViewer->scrollRight();
}

void ViewerWidget::fitWindow() {
    if(canvasInputPolicy != CanvasInputPolicy::All)
        return;
    imageViewer->setFitWindow();
}

void ViewerWidget::fitWidth() {
    if(canvasInputPolicy != CanvasInputPolicy::All)
        return;
    imageViewer->setFitWidth();
}

void ViewerWidget::fitOriginal() {
    if(canvasInputPolicy != CanvasInputPolicy::All)
        return;
    imageViewer->setFitOriginal();
}

void ViewerWidget::toggleTransparencyGrid() {
    imageViewer->toggleTransparencyGrid();
}

void ViewerWidget::setFilterNearest() {
    imageViewer->setFilterNearest();
}

void ViewerWidget::setFilterBilinear() {
    imageViewer->setFilterBilinear();
}

void ViewerWidget::setScalingFilter(ScalingFilter filter) {
    imageViewer->setScalingFilter(filter);
}

void ViewerWidget::toggleLockZoom() {
    imageViewer->toggleLockZoom();
}

void ViewerWidget::toggleLockView() {
    imageViewer->toggleLockView();
}

bool ViewerWidget::interactionEnabled() {
    return canvasInputPolicy == CanvasInputPolicy::All;
}

bool ViewerWidget::showImage(std::unique_ptr<QPixmap> pixmap) {
    if(!pixmap)
        return false;
    stopPlayback();
    videoControls->hide();
    enableImageViewer();
    imageViewer->showImage(std::move(pixmap));
    hideCursorTimed(false);
    return true;
}

bool ViewerWidget::showAnimation(const std::shared_ptr<QMovie> &movie) {
    if(!movie)
        return false;
    stopPlayback();
    enableImageViewer();
    imageViewer->showAnimation(movie);
    hideCursorTimed(false);
    return true;
}

bool ViewerWidget::showVideo(QString file) {
    stopPlayback();
    enableVideoPlayer();
    videoPlayer->showVideo(std::move(file));
    hideCursorTimed(false);
    return true;
}

void ViewerWidget::stopPlayback() {
    if(currentWidget == IMAGEVIEWER && imageViewer->hasAnimation())
        imageViewer->stopAnimation();
    if(currentWidget == VIDEOPLAYER) {
        // stopping is visibly slower
        //videoPlayer->stop();
        videoPlayer->setPaused(true);
    }
}

void ViewerWidget::startPlayback() {
    if(currentWidget == IMAGEVIEWER && imageViewer->hasAnimation())
        imageViewer->startAnimation();
    if(currentWidget == VIDEOPLAYER) {
        // stopping is visibly slower
        //videoPlayer->stop();
        videoPlayer->setPaused(false);
    }
}

// programmatic fit — NOT a user command, deliberately ungated: mode
// persistence and scripted fits must apply while the canvas input policy
// is off (e.g. crop mode)
void ViewerWidget::setFitMode(ImageFitMode mode) {
    if(mode == FIT_WINDOW)
        imageViewer->setFitWindow();
    else if(mode == FIT_WIDTH)
        imageViewer->setFitWidth();
    else if(mode == FIT_ORIGINAL)
        imageViewer->setFitOriginal();
}

ImageFitMode ViewerWidget::fitMode() {
    return imageViewer->fitMode();
}

QPropertyNotifier ViewerWidget::observeFitMode(std::function<void()> callback) {
    return imageViewer->observeFitMode(std::move(callback));
}

void ViewerWidget::onScalingFinished(std::unique_ptr<QPixmap> scaled) {
    imageViewer->setScaledPixmap(std::move(scaled));
}

void ViewerWidget::closeImage() {
    imageViewer->closeImage();
    videoPlayer->stop();
    showCursor();
}

void ViewerWidget::pauseResumePlayback() {
    if(currentWidget == VIDEOPLAYER)
        videoPlayer.get()->pauseResume();
    else if(imageViewer->hasAnimation())
        imageViewer->pauseResume();
}

void ViewerWidget::seek(int pos) {
    if(currentWidget == VIDEOPLAYER) {
        videoPlayer.get()->seek(pos);
    } else if(imageViewer->hasAnimation()) {
        imageViewer->stopAnimation();
        imageViewer->showAnimationFrame(pos);
    }
}

void ViewerWidget::seekRelative(int pos) {
    if(currentWidget == VIDEOPLAYER)
        videoPlayer.get()->seekRelative(pos);
}

void ViewerWidget::seekBackward() {
    if(currentWidget == VIDEOPLAYER)
        videoPlayer.get()->seekRelative(-10);
}

void ViewerWidget::seekForward() {
    if(currentWidget == VIDEOPLAYER)
        videoPlayer.get()->seekRelative(10);
}

void ViewerWidget::frameStep() {
    if(currentWidget == VIDEOPLAYER)
        videoPlayer.get()->frameStep();
    else if(imageViewer->hasAnimation()) {
        imageViewer->stopAnimation();
        imageViewer->nextFrame();
    }
}

void ViewerWidget::frameStepBack() {
    if(currentWidget == VIDEOPLAYER)
        videoPlayer.get()->frameStepBack();
    else if(imageViewer->hasAnimation()) {
        imageViewer->stopAnimation();
        imageViewer->prevFrame();
    }
}

void ViewerWidget::toggleMute() {
    if(currentWidget == VIDEOPLAYER) {
        videoPlayer->setMuted(!videoPlayer->muted());
        videoControls->onVideoMuted(videoPlayer->muted());
    }
}

void ViewerWidget::volumeUp() {
    if(currentWidget == VIDEOPLAYER)
        videoPlayer->volumeUp();
}

void ViewerWidget::volumeDown() {
    if(currentWidget == VIDEOPLAYER) {
        videoPlayer->volumeDown();
    }
}

bool ViewerWidget::isDisplaying() {
    if(currentWidget == IMAGEVIEWER && imageViewer->isDisplaying())
        return true;
    if(currentWidget == VIDEOPLAYER)
        return true;
    else
        return false;
}

bool ViewerWidget::lockZoomEnabled() {
    return imageViewer->lockZoomEnabled();
}

bool ViewerWidget::lockViewEnabled() {
    return imageViewer->lockViewEnabled();
}

ScalingFilter ViewerWidget::scalingFilter() {
    return imageViewer->scalingFilter();
}

void ViewerWidget::mousePressEvent(QMouseEvent *event) {
    hideContextMenu();
    event->ignore();
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent *event) {
    showCursor();
    hideCursorTimed(false);
    event->ignore();
}

void ViewerWidget::mouseMoveEvent(QMouseEvent *event) {
    mWaylandCursorWorkaround = false;
    if(!(event->buttons() & Qt::LeftButton) && !(event->buttons() & Qt::RightButton)) {
        showCursor();
        hideCursorTimed(true);
    }
    if(currentWidget == VIDEOPLAYER || imageViewer->hasAnimation()) {
        if(videoControlsArea().contains(event->pos()))
            videoControls->show();
        else
            videoControls->hide();
    }
    event->ignore();
}

void ViewerWidget::hideCursorTimed(bool restartTimer) {
    if(restartTimer || !cursorTimer.isActive())
        cursorTimer.start(CURSOR_HIDE_TIMEOUT_MS);
}

void ViewerWidget::hideCursor() {
    cursorTimer.stop();
    // ignore if we have something else open like settings window
    if(!isDisplaying() || !isActiveWindow())
        return;
    // ignore when menu is up
    if(contextMenu && contextMenu->isVisible())
        return;
    if(settings->cursorAutohide()) {
        // force hide on wayland until we can get the cursor pos
        if(mWaylandCursorWorkaround) {
            setCursor(QCursor(Qt::BlankCursor));
            videoControls->hide();
        } else {
            QPoint posMapped = mapFromGlobal(QCursor::pos());
            //if(settings->enableClickZoneThing())
            // ignore when we are hovering the click zone
            if(clickZoneOverlay->leftZone().contains(posMapped) || clickZoneOverlay->leftZone().contains(posMapped)) {
                return;
            }

            // only hide when we are under viewer or player widget
            QWidget *w = qApp->widgetAt(QCursor::pos());
            if(w && (w == imageViewer.get()->viewport() || w == videoPlayer->getPlayer())) {
                if(!videoControls->isVisible() || !videoControlsArea().contains(posMapped)) {
                    setCursor(QCursor(Qt::BlankCursor));
                    videoControls->hide();
                }
            }
        }
    }
}

QRect ViewerWidget::videoControlsArea() {
    QRect vcontrolsRect;
    if(settings->panelEnabled() && settings->panelPosition() == PANEL_BOTTOM)
        vcontrolsRect = QRect(0, 0, width(), 160); // inverted (top)
    else
        vcontrolsRect = QRect(0, height() - 160, width(), height());
    return vcontrolsRect;
}

// click zone input crutch
// --
// we can't process mouse events in the overlay
// cause they won't propagate to the ImageViewer, only to overlay's container (this widget)
// so we just grab them before they reach ImageViewer and do the needful
bool ViewerWidget::eventFilter(QObject *object, QEvent *event) {
    Q_UNUSED(object)
    // catch press and doubleclick
    // force doubleclick to act as press event for click zones
    if(event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
        // disable feature for very small windows
        if(width() <= 250)
            return false;

        auto mouseEvent = dynamic_cast<QMouseEvent *>(event);
        if(mouseEvent->button() != Qt::LeftButton || mouseEvent->modifiers()) {
            clickZoneOverlay->disableHighlight();
            return false;
        }
        if(clickZoneOverlay->leftZone().contains(mouseEvent->pos())) {
            clickZoneOverlay->setPressed(true);
            clickZoneOverlay->highlightLeft();
            imageViewer.get()->disableDrags();
            actionManager->invokeAction("prevImage");
            return true; // do not pass the event to imageViewer
        }
        if(clickZoneOverlay->rightZone().contains(mouseEvent->pos())) {
            clickZoneOverlay->setPressed(true);
            clickZoneOverlay->highlightRight();
            imageViewer.get()->disableDrags();
            actionManager->invokeAction("nextImage");
            return true;
        }
    }
    // right click produces QEvent::ContextMenu instead of QEvent::MouseButtonPress
    // this is NOT a QMouseEvent
    if(event->type() == QEvent::ContextMenu) {
        clickZoneOverlay->disableHighlight();
        return false;
    }

    if(event->type() == QEvent::MouseButtonRelease) {
        clickZoneOverlay->setPressed(false);
        imageViewer.get()->enableDrags();
    }

    if(event->type() == QEvent::MouseMove || event->type() == QEvent::Enter) {
        QPoint mousePos;
        if(event->type() == QEvent::MouseMove) {
            auto mouseEvent = dynamic_cast<QMouseEvent *>(event);
            mousePos = mouseEvent->pos();
            if(mouseEvent->buttons())
                return false;
        } else {
            auto enterEvent = dynamic_cast<QEnterEvent *>(event);
            mousePos = enterEvent->position().toPoint();
        }
        if(clickZoneOverlay->leftZone().contains(mousePos)) {
            clickZoneOverlay->setPressed(false);
            clickZoneOverlay->highlightLeft();
            setCursor(Qt::PointingHandCursor);
            return true;
        } else if(clickZoneOverlay->rightZone().contains(mousePos)) {
            clickZoneOverlay->setPressed(false);
            clickZoneOverlay->highlightRight();
            setCursor(Qt::PointingHandCursor);
            return true;
        } else {
            clickZoneOverlay->disableHighlight();
            setCursor(Qt::ArrowCursor);
        }
    }

    if(event->type() == QEvent::Leave) {
        clickZoneOverlay->disableHighlight();
        setCursor(Qt::ArrowCursor);
    }

    return false; // send event to imageViewer / videoplayer
}

void ViewerWidget::showCursor() {
    cursorTimer.stop();
    if(cursor().shape() == Qt::BlankCursor)
        setCursor(QCursor(Qt::ArrowCursor));
}

void ViewerWidget::showContextMenu() {
    QPoint pos = cursor().pos();
    showContextMenu(pos);
}

void ViewerWidget::showContextMenu(QPoint pos) {
    if(isVisible() && interactionEnabled()) {
        if(!contextMenu) {
            contextMenu.reset(new ContextMenu(this));
            connect(contextMenu.get(), &ContextMenu::showScriptSettings, this, &ViewerWidget::showScriptSettings);
        }
        contextMenu->setImageEntriesEnabled(isDisplaying());
        if(!contextMenu->isVisible())
            contextMenu->showAt(pos);
        else
            contextMenu->hide();
    }
}

void ViewerWidget::onFullscreenModeChanged(bool mode) {
    imageViewer->onFullscreenModeChanged(mode);
    mIsFullscreen = mode;
}

void ViewerWidget::readSettings() {
    videoControls->onVideoMuted(!settings->playVideoSounds());
    if(settings->clickableEdges()) {
        imageViewer->viewport()->installEventFilter(this);
        videoPlayer->installEventFilter(this);
        clickZoneOverlay->show();
    } else {
        imageViewer->viewport()->removeEventFilter(this);
        videoPlayer->removeEventFilter(this);
        imageViewer.get()->enableDrags();
        clickZoneOverlay->hide();
    }
}

void ViewerWidget::setLoopPlayback(bool mode) {
    imageViewer->setLoopPlayback(mode);
    videoPlayer->setLoopPlayback(mode);
}

void ViewerWidget::hideContextMenu() {
    if(contextMenu)
        contextMenu->hide();
}

void ViewerWidget::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    hideContextMenu();
}

// block native tab-switching so we can use it in shortcuts
bool ViewerWidget::focusNextPrevChild(bool mode) {
    Q_UNUSED(mode)
    return false;
}

// keyboard events are not handled here: an unhandled press propagates to
// MW, whose keyPressEvent is the single funnel into the action system.
// The video-mode Space intercept that used to live in this door moved to
// MW::switchFitMode, which owns the context now.

void ViewerWidget::toggleVideoPlayback() {
    if(currentWidget == VIDEOPLAYER)
        videoPlayer->pauseResume();
}

void ViewerWidget::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    videoControls->hide();
}
