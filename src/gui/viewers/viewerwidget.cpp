/*
 * This widget combines ImageViewer / VideoPlayer.
 * Only one is displayed at a time.
 */

#include "viewerwidget.h"

#include <QScrollBar>

#include <utility>

ViewerWidget::ViewerWidget(QWidget *parent)
    : FloatingWidgetContainer(parent), m_imageViewer(nullptr), m_videoPlayer(nullptr), m_contextMenu(nullptr),
      m_videoControls(nullptr), m_currentWidget(UNSET), m_canvasInputPolicy(CanvasInputPolicy::All),
      m_waylandCursorWorkaround(false), m_isFullscreen(false) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
#ifdef Q_OS_LINUX
    // we cant check cursor position on wayland until the mouse is moved
    // use this to skip cursor check once
    if(qgetenv("XDG_SESSION_TYPE") == "wayland")
        m_waylandCursorWorkaround = true;
#endif
    m_layout.setContentsMargins(0, 0, 0, 0);
    m_layout.setSpacing(0);
    this->setLayout(&m_layout);

    m_imageViewer.reset(new ImageViewerV2(this));
    m_layout.addWidget(m_imageViewer.get());
    m_imageViewer->hide();

    connect(m_imageViewer.get(), &ImageViewerV2::scalingRequested, this, &ViewerWidget::scalingRequested);
    connect(m_imageViewer.get(), &ImageViewerV2::draggedOut, this, &ViewerWidget::draggedOut);
    connect(m_imageViewer.get(), &ImageViewerV2::scaleChanged, this, &ViewerWidget::onScaleChanged);
    connect(m_imageViewer.get(), &ImageViewerV2::scaleChanged, this, &ViewerWidget::canvasScaleChanged);
    connect(m_imageViewer->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            &ViewerWidget::canvasViewportChanged);
    connect(m_imageViewer->verticalScrollBar(), &QScrollBar::valueChanged, this, &ViewerWidget::canvasViewportChanged);
    connect(m_imageViewer.get(), &ImageViewerV2::playbackFinished, this, &ViewerWidget::onAnimationPlaybackFinished);

    m_videoPlayer.reset(new VideoPlayerInitProxy(this));
    m_layout.addWidget(m_videoPlayer.get());
    m_videoPlayer->hide();
    m_videoControls = new VideoControlsProxyWrapper(this);
    // ZoomIndicatorOverlayProxy and VideoControlsProxyWrapper are unparented
    // QObject wrappers (their ctors keep `container` as a plain member) — they
    // are this widget's C++ ownership, discharged here

    // tmp no wrapper
    m_zoomIndicator = new ZoomIndicatorOverlayProxy(this);
    m_clickZoneOverlay = new ClickZoneOverlay(this);

    connect(m_videoPlayer.get(), &VideoPlayer::playbackFinished, this, &ViewerWidget::onVideoPlaybackFinished);

    connect(m_videoControls, &VideoControlsProxyWrapper::seekBackward, this, &ViewerWidget::seekBackward);
    connect(m_videoControls, &VideoControlsProxyWrapper::seekForward, this, &ViewerWidget::seekForward);
    connect(m_videoControls, &VideoControlsProxyWrapper::seek, this, &ViewerWidget::seek);

    enableImageViewer();
    setCanvasInputPolicy(CanvasInputPolicy::All);

    connect(&m_cursorTimer, &QTimer::timeout, this, &ViewerWidget::hideCursor);

    connect(settings, &Settings::settingsChanged, this, &ViewerWidget::readSettings);
    readSettings();
}
ViewerWidget::~ViewerWidget() {
    // unparented QObject wrappers are this widget's C++ ownership; their inner
    // overlays are Qt-parented here and go with the tree sweep that follows
    delete m_videoControls;
    delete m_zoomIndicator;
}

QRect ViewerWidget::imageRect() {
    if(m_imageViewer && m_currentWidget == IMAGEVIEWER)
        return m_imageViewer->scaledRectR();
    else
        return QRect(0, 0, 0, 0);
}

float ViewerWidget::currentScale() {
    if(m_currentWidget == IMAGEVIEWER)
        return m_imageViewer->currentScale();
    else
        return 1.0f;
}

QSize ViewerWidget::sourceSize() {
    if(m_currentWidget == IMAGEVIEWER)
        return m_imageViewer->sourceSize();
    else
        return QSize(0, 0);
}

// hide videoPlayer, show imageViewer
void ViewerWidget::enableImageViewer() {
    if(m_currentWidget != IMAGEVIEWER) {
        disableVideoPlayer();
        m_videoControls->setMode(PLAYBACK_ANIMATION);
        connect(m_imageViewer.get(), &ImageViewerV2::durationChanged, m_videoControls,
                &VideoControlsProxyWrapper::setPlaybackDuration);
        connect(m_imageViewer.get(), &ImageViewerV2::frameChanged, m_videoControls,
                &VideoControlsProxyWrapper::setPlaybackPosition);
        connect(m_imageViewer.get(), &ImageViewerV2::animationPaused, m_videoControls,
                &VideoControlsProxyWrapper::onPlaybackPaused);
        m_imageViewer->show();
        m_currentWidget = IMAGEVIEWER;
    }
}

// hide imageViewer, show videoPlayer
void ViewerWidget::enableVideoPlayer() {
    if(m_currentWidget != VIDEOPLAYER) {
        disableImageViewer();
        m_videoControls->setMode(PLAYBACK_VIDEO);
        connect(m_videoPlayer.get(), &VideoPlayer::durationChanged, m_videoControls,
                &VideoControlsProxyWrapper::setPlaybackDuration);
        connect(m_videoPlayer.get(), &VideoPlayer::positionChanged, m_videoControls,
                &VideoControlsProxyWrapper::setPlaybackPosition);
        connect(m_videoPlayer.get(), &VideoPlayer::videoPaused, m_videoControls,
                &VideoControlsProxyWrapper::onPlaybackPaused);
        m_videoPlayer->show();
        m_currentWidget = VIDEOPLAYER;
    }
}

void ViewerWidget::disableImageViewer() {
    if(m_currentWidget == IMAGEVIEWER) {
        m_currentWidget = UNSET;
        m_imageViewer->closeImage();
        m_imageViewer->hide();
        m_zoomIndicator->hide();
        disconnect(m_imageViewer.get(), &ImageViewerV2::durationChanged, m_videoControls,
                   &VideoControlsProxyWrapper::setPlaybackDuration);
        disconnect(m_imageViewer.get(), &ImageViewerV2::frameChanged, m_videoControls,
                   &VideoControlsProxyWrapper::setPlaybackPosition);
        disconnect(m_imageViewer.get(), &ImageViewerV2::animationPaused, m_videoControls,
                   &VideoControlsProxyWrapper::onPlaybackPaused);
    }
}

void ViewerWidget::disableVideoPlayer() {
    if(m_currentWidget == VIDEOPLAYER) {
        m_currentWidget = UNSET;
        //videoControls->hide();
        disconnect(m_videoPlayer.get(), &VideoPlayer::durationChanged, m_videoControls,
                   &VideoControlsProxyWrapper::setPlaybackDuration);
        disconnect(m_videoPlayer.get(), &VideoPlayer::positionChanged, m_videoControls,
                   &VideoControlsProxyWrapper::setPlaybackPosition);
        disconnect(m_videoPlayer.get(), &VideoPlayer::videoPaused, m_videoControls,
                   &VideoControlsProxyWrapper::onPlaybackPaused);
        m_videoPlayer->setPaused(true);
        // even after calling hide() the player sends a few video frames
        // which paints over the imageviewer, causing corruption
        // so we do not HIDE it, but rather just cover it by imageviewer's widget
        // seems to work fine, might even feel a bit snappier
        if(!m_videoPlayer->isInitialized())
            m_videoPlayer->hide();
    }
}

void ViewerWidget::onScaleChanged(qreal scale) {
    if(!this->isVisible())
        return;
    if(scale != 1.0f) {
        m_zoomIndicator->setScale(scale);
        if(settings->zoomIndicatorMode() == ZoomIndicatorMode::INDICATOR_ENABLED)
            m_zoomIndicator->show();
        else if((settings->zoomIndicatorMode() == ZoomIndicatorMode::INDICATOR_AUTO))
            m_zoomIndicator->show(1500);
    } else {
        m_zoomIndicator->hide();
    }
}

void ViewerWidget::onVideoPlaybackFinished() {
    if(m_currentWidget == VIDEOPLAYER)
        emit playbackFinished();
}

void ViewerWidget::onAnimationPlaybackFinished() {
    if(m_currentWidget == IMAGEVIEWER)
        emit playbackFinished();
}

void ViewerWidget::setCanvasInputPolicy(CanvasInputPolicy policy) {
    if(m_canvasInputPolicy == policy)
        return;
    m_canvasInputPolicy = policy;
    m_imageViewer->setAttribute(Qt::WA_TransparentForMouseEvents, policy != CanvasInputPolicy::All);
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
    m_imageViewer->zoomIn();
}

void ViewerWidget::zoomOut() {
    // zoom survives Framing: it is the fine-adjustment affordance of the
    // crop flow, and the overlay re-derives its mapping from live queries
    m_imageViewer->zoomOut();
}

void ViewerWidget::zoomInCursor() {
    // zoom survives Framing: it is the fine-adjustment affordance of the
    // crop flow, and the overlay re-derives its mapping from live queries
    m_imageViewer->zoomInCursor();
}

void ViewerWidget::zoomOutCursor() {
    // zoom survives Framing: it is the fine-adjustment affordance of the
    // crop flow, and the overlay re-derives its mapping from live queries
    m_imageViewer->zoomOutCursor();
}

void ViewerWidget::scrollUp() {
    m_imageViewer->scrollUp();
}

void ViewerWidget::scrollDown() {
    m_imageViewer->scrollDown();
}

void ViewerWidget::scrollLeft() {
    m_imageViewer->scrollLeft();
}

void ViewerWidget::scrollRight() {
    m_imageViewer->scrollRight();
}

void ViewerWidget::fitWindow() {
    if(m_canvasInputPolicy != CanvasInputPolicy::All)
        return;
    m_imageViewer->setFitWindow();
}

void ViewerWidget::fitWidth() {
    if(m_canvasInputPolicy != CanvasInputPolicy::All)
        return;
    m_imageViewer->setFitWidth();
}

void ViewerWidget::fitOriginal() {
    if(m_canvasInputPolicy != CanvasInputPolicy::All)
        return;
    m_imageViewer->setFitOriginal();
}

void ViewerWidget::toggleTransparencyGrid() {
    m_imageViewer->toggleTransparencyGrid();
}

void ViewerWidget::setFilterNearest() {
    m_imageViewer->setFilterNearest();
}

void ViewerWidget::setFilterBilinear() {
    m_imageViewer->setFilterBilinear();
}

void ViewerWidget::setScalingFilter(ScalingFilter filter) {
    m_imageViewer->setScalingFilter(filter);
}

void ViewerWidget::toggleLockZoom() {
    m_imageViewer->toggleLockZoom();
}

void ViewerWidget::toggleLockView() {
    m_imageViewer->toggleLockView();
}

bool ViewerWidget::interactionEnabled() {
    return m_canvasInputPolicy == CanvasInputPolicy::All;
}

bool ViewerWidget::showImage(std::unique_ptr<QPixmap> pixmap) {
    if(!pixmap)
        return false;
    stopPlayback();
    m_videoControls->hide();
    enableImageViewer();
    m_imageViewer->showImage(std::move(pixmap));
    hideCursorTimed(false);
    return true;
}

bool ViewerWidget::showAnimation(const std::shared_ptr<QMovie> &movie) {
    if(!movie)
        return false;
    stopPlayback();
    enableImageViewer();
    m_imageViewer->showAnimation(movie);
    hideCursorTimed(false);
    return true;
}

bool ViewerWidget::showVideo(QString file) {
    stopPlayback();
    enableVideoPlayer();
    m_videoPlayer->showVideo(std::move(file));
    hideCursorTimed(false);
    return true;
}

void ViewerWidget::stopPlayback() {
    if(m_currentWidget == IMAGEVIEWER && m_imageViewer->hasAnimation())
        m_imageViewer->stopAnimation();
    if(m_currentWidget == VIDEOPLAYER) {
        // stopping is visibly slower
        //videoPlayer->stop();
        m_videoPlayer->setPaused(true);
    }
}

void ViewerWidget::startPlayback() {
    if(m_currentWidget == IMAGEVIEWER && m_imageViewer->hasAnimation())
        m_imageViewer->startAnimation();
    if(m_currentWidget == VIDEOPLAYER) {
        // stopping is visibly slower
        //videoPlayer->stop();
        m_videoPlayer->setPaused(false);
    }
}

// programmatic fit — NOT a user command, deliberately ungated: mode
// persistence and scripted fits must apply while the canvas input policy
// is off (e.g. crop mode)
void ViewerWidget::setFitMode(ImageFitMode mode) {
    if(mode == FIT_WINDOW)
        m_imageViewer->setFitWindow();
    else if(mode == FIT_WIDTH)
        m_imageViewer->setFitWidth();
    else if(mode == FIT_ORIGINAL)
        m_imageViewer->setFitOriginal();
}

ImageFitMode ViewerWidget::fitMode() {
    return m_imageViewer->fitMode();
}

QPropertyNotifier ViewerWidget::observeFitMode(std::function<void()> callback) {
    return m_imageViewer->observeFitMode(std::move(callback));
}

void ViewerWidget::onScalingFinished(std::unique_ptr<QPixmap> scaled) {
    m_imageViewer->setScaledPixmap(std::move(scaled));
}

void ViewerWidget::closeImage() {
    m_imageViewer->closeImage();
    m_videoPlayer->stop();
    showCursor();
}

void ViewerWidget::pauseResumePlayback() {
    if(m_currentWidget == VIDEOPLAYER)
        m_videoPlayer.get()->pauseResume();
    else if(m_imageViewer->hasAnimation())
        m_imageViewer->pauseResume();
}

void ViewerWidget::seek(int pos) {
    if(m_currentWidget == VIDEOPLAYER) {
        m_videoPlayer.get()->seek(pos);
    } else if(m_imageViewer->hasAnimation()) {
        m_imageViewer->stopAnimation();
        m_imageViewer->showAnimationFrame(pos);
    }
}

void ViewerWidget::seekRelative(int pos) {
    if(m_currentWidget == VIDEOPLAYER)
        m_videoPlayer.get()->seekRelative(pos);
}

void ViewerWidget::seekBackward() {
    if(m_currentWidget == VIDEOPLAYER)
        m_videoPlayer.get()->seekRelative(-10);
}

void ViewerWidget::seekForward() {
    if(m_currentWidget == VIDEOPLAYER)
        m_videoPlayer.get()->seekRelative(10);
}

void ViewerWidget::frameStep() {
    if(m_currentWidget == VIDEOPLAYER)
        m_videoPlayer.get()->frameStep();
    else if(m_imageViewer->hasAnimation()) {
        m_imageViewer->stopAnimation();
        m_imageViewer->nextFrame();
    }
}

void ViewerWidget::frameStepBack() {
    if(m_currentWidget == VIDEOPLAYER)
        m_videoPlayer.get()->frameStepBack();
    else if(m_imageViewer->hasAnimation()) {
        m_imageViewer->stopAnimation();
        m_imageViewer->prevFrame();
    }
}

void ViewerWidget::toggleMute() {
    if(m_currentWidget == VIDEOPLAYER) {
        m_videoPlayer->setMuted(!m_videoPlayer->muted());
        m_videoControls->onVideoMuted(m_videoPlayer->muted());
    }
}

void ViewerWidget::volumeUp() {
    if(m_currentWidget == VIDEOPLAYER)
        m_videoPlayer->volumeUp();
}

void ViewerWidget::volumeDown() {
    if(m_currentWidget == VIDEOPLAYER) {
        m_videoPlayer->volumeDown();
    }
}

bool ViewerWidget::isDisplaying() {
    if(m_currentWidget == IMAGEVIEWER && m_imageViewer->isDisplaying())
        return true;
    if(m_currentWidget == VIDEOPLAYER)
        return true;
    else
        return false;
}

bool ViewerWidget::lockZoomEnabled() {
    return m_imageViewer->lockZoomEnabled();
}

bool ViewerWidget::lockViewEnabled() {
    return m_imageViewer->lockViewEnabled();
}

ScalingFilter ViewerWidget::scalingFilter() {
    return m_imageViewer->scalingFilter();
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
    m_waylandCursorWorkaround = false;
    if(!(event->buttons() & Qt::LeftButton) && !(event->buttons() & Qt::RightButton)) {
        showCursor();
        hideCursorTimed(true);
    }
    if(m_currentWidget == VIDEOPLAYER || m_imageViewer->hasAnimation()) {
        if(videoControlsArea().contains(event->pos()))
            m_videoControls->show();
        else
            m_videoControls->hide();
    }
    event->ignore();
}

void ViewerWidget::hideCursorTimed(bool restartTimer) {
    if(restartTimer || !m_cursorTimer.isActive())
        m_cursorTimer.start(m_cursorHideTimeoutMs);
}

void ViewerWidget::hideCursor() {
    m_cursorTimer.stop();
    // ignore if we have something else open like settings window
    if(!isDisplaying() || !isActiveWindow())
        return;
    // ignore when menu is up
    if(m_contextMenu && m_contextMenu->isVisible())
        return;
    if(settings->cursorAutohide()) {
        // force hide on wayland until we can get the cursor pos
        if(m_waylandCursorWorkaround) {
            setCursor(QCursor(Qt::BlankCursor));
            m_videoControls->hide();
        } else {
            QPoint posMapped = mapFromGlobal(QCursor::pos());
            //if(settings->enableClickZoneThing())
            // ignore when we are hovering the click zone
            if(m_clickZoneOverlay->leftZone().contains(posMapped) ||
               m_clickZoneOverlay->leftZone().contains(posMapped)) {
                return;
            }

            // only hide when we are under viewer or player widget
            QWidget *w = qApp->widgetAt(QCursor::pos());
            if(w && (w == m_imageViewer.get()->viewport() || w == m_videoPlayer->getPlayer())) {
                if(!m_videoControls->isVisible() || !videoControlsArea().contains(posMapped)) {
                    setCursor(QCursor(Qt::BlankCursor));
                    m_videoControls->hide();
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
            m_clickZoneOverlay->disableHighlight();
            return false;
        }
        if(m_clickZoneOverlay->leftZone().contains(mouseEvent->pos())) {
            m_clickZoneOverlay->setPressed(true);
            m_clickZoneOverlay->highlightLeft();
            m_imageViewer.get()->disableDrags();
            actionManager->invokeAction("prevImage");
            return true; // do not pass the event to imageViewer
        }
        if(m_clickZoneOverlay->rightZone().contains(mouseEvent->pos())) {
            m_clickZoneOverlay->setPressed(true);
            m_clickZoneOverlay->highlightRight();
            m_imageViewer.get()->disableDrags();
            actionManager->invokeAction("nextImage");
            return true;
        }
    }
    // right click produces QEvent::ContextMenu instead of QEvent::MouseButtonPress
    // this is NOT a QMouseEvent
    if(event->type() == QEvent::ContextMenu) {
        m_clickZoneOverlay->disableHighlight();
        return false;
    }

    if(event->type() == QEvent::MouseButtonRelease) {
        m_clickZoneOverlay->setPressed(false);
        m_imageViewer.get()->enableDrags();
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
        if(m_clickZoneOverlay->leftZone().contains(mousePos)) {
            m_clickZoneOverlay->setPressed(false);
            m_clickZoneOverlay->highlightLeft();
            setCursor(Qt::PointingHandCursor);
            return true;
        } else if(m_clickZoneOverlay->rightZone().contains(mousePos)) {
            m_clickZoneOverlay->setPressed(false);
            m_clickZoneOverlay->highlightRight();
            setCursor(Qt::PointingHandCursor);
            return true;
        } else {
            m_clickZoneOverlay->disableHighlight();
            setCursor(Qt::ArrowCursor);
        }
    }

    if(event->type() == QEvent::Leave) {
        m_clickZoneOverlay->disableHighlight();
        setCursor(Qt::ArrowCursor);
    }

    return false; // send event to imageViewer / videoplayer
}

void ViewerWidget::showCursor() {
    m_cursorTimer.stop();
    if(cursor().shape() == Qt::BlankCursor)
        setCursor(QCursor(Qt::ArrowCursor));
}

void ViewerWidget::showContextMenu() {
    QPoint pos = cursor().pos();
    showContextMenu(pos);
}

void ViewerWidget::showContextMenu(QPoint pos) {
    if(isVisible() && interactionEnabled()) {
        if(!m_contextMenu) {
            m_contextMenu.reset(new ContextMenu(this));
            connect(m_contextMenu.get(), &ContextMenu::showScriptSettings, this, &ViewerWidget::showScriptSettings);
        }
        m_contextMenu->setImageEntriesEnabled(isDisplaying());
        if(!m_contextMenu->isVisible())
            m_contextMenu->showAt(pos);
        else
            m_contextMenu->hide();
    }
}

void ViewerWidget::onFullscreenModeChanged(bool mode) {
    m_imageViewer->onFullscreenModeChanged(mode);
    m_isFullscreen = mode;
}

void ViewerWidget::readSettings() {
    m_videoControls->onVideoMuted(!settings->playVideoSounds());
    if(settings->clickableEdges()) {
        m_imageViewer->viewport()->installEventFilter(this);
        m_videoPlayer->installEventFilter(this);
        m_clickZoneOverlay->show();
    } else {
        m_imageViewer->viewport()->removeEventFilter(this);
        m_videoPlayer->removeEventFilter(this);
        m_imageViewer.get()->enableDrags();
        m_clickZoneOverlay->hide();
    }
}

void ViewerWidget::setLoopPlayback(bool mode) {
    m_imageViewer->setLoopPlayback(mode);
    m_videoPlayer->setLoopPlayback(mode);
}

void ViewerWidget::hideContextMenu() {
    if(m_contextMenu)
        m_contextMenu->hide();
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
    if(m_currentWidget == VIDEOPLAYER)
        m_videoPlayer->pauseResume();
}

void ViewerWidget::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    m_videoControls->hide();
}
