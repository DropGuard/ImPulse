#include "imageviewerv2.h"

ImageViewerV2::ImageViewerV2(QWidget *parent)
    : QGraphicsView(parent), m_pixmap(nullptr), m_pixmapScaled(nullptr), m_movie(nullptr), m_transparencyGrid(false),
      m_expandImage(false), m_smoothAnimatedImages(true), m_smoothUpscaling(true), m_forceFastScale(false),
      m_loopPlayback(true), m_isFullscreen(false), m_scrollBarWorkaround(true), m_useFixedZoomLevels(false),
      m_trackpadDetection(true), m_mouseInteraction(MouseInteractionState::MOUSE_NONE), m_minScale(0.01f),
      m_maxScale(500.0f), m_fitWindowScale(0.125f), m_viewLock(LOCK_NONE), m_imageFitMode(FIT_WINDOW),
      m_scalingFilter(QI_FILTER_BILINEAR), m_imageFitModeDefault(FIT_WINDOW), m_scene(nullptr),
      m_checkboard(":res/icons/common/other/checkerboard.png") {
    setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    this->viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAcceptDrops(false);

    if(qApp->platformName() == "wayland")
        m_wayland = true;

    m_dpr = this->devicePixelRatioF();
    m_hs = horizontalScrollBar();
    m_vs = verticalScrollBar();

    m_scrollTimeLineY = new QTimeLine(1000, this);
    m_scrollTimeLineY->setEasingCurve(QEasingCurve::OutSine);
    m_scrollTimeLineY->setDuration(m_animationSpeed);
    m_scrollTimeLineY->setUpdateInterval(m_scrollUpdateRate);
    m_scrollTimeLineX = new QTimeLine(1000, this);
    m_scrollTimeLineX->setEasingCurve(QEasingCurve::OutSine);
    m_scrollTimeLineX->setDuration(m_animationSpeed);
    m_scrollTimeLineX->setUpdateInterval(m_scrollUpdateRate);
    connect(m_scrollTimeLineX, &QTimeLine::finished, this, &ImageViewerV2::onScrollTimelineFinished);
    connect(m_scrollTimeLineY, &QTimeLine::finished, this, &ImageViewerV2::onScrollTimelineFinished);

    m_animationTimer = new QTimer(this);
    m_animationTimer->setSingleShot(true);

    m_scaleTimer = new QTimer(this);
    m_scaleTimer->setSingleShot(true);
    m_scaleTimer->setInterval(80);

    m_lastTouchpadScroll.start();

    m_zoomThreshold = static_cast<int>(m_dpr * 4.);

    m_pixmapItem.setTransformationMode(Qt::SmoothTransformation);
    m_pixmapItem.setScale(1.0f);
    m_pixmapItem.setOffset(10000, 10000);
    m_pixmapItem.setTransformOriginPoint(10000, 10000);
    m_pixmapItemScaled.setScale(1.0f);
    m_pixmapItemScaled.setOffset(10000, 10000);
    m_pixmapItemScaled.setTransformOriginPoint(10000, 10000);

    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_scene = new QGraphicsScene(this);
    m_scene->setSceneRect(0, 0, 200000, 200000);
    m_scene->setBackgroundBrush(QColor(60, 60, 103));
    m_scene->addItem(&m_pixmapItem);
    m_scene->addItem(&m_pixmapItemScaled);
    m_pixmapItemScaled.hide();

    this->setFrameShape(QFrame::NoFrame);
    this->setScene(m_scene);

    connect(m_scrollTimeLineX, &QTimeLine::frameChanged, this, &ImageViewerV2::scrollToX);
    connect(m_scrollTimeLineY, &QTimeLine::frameChanged, this, &ImageViewerV2::scrollToY);

    connect(m_animationTimer, &QTimer::timeout, this, &ImageViewerV2::onAnimationTimer, Qt::UniqueConnection);

    QObject::connect(m_scaleTimer, &QTimer::timeout, this, [this]() {
        this->requestScaling();
    });

    readSettings();
    connect(settings, &Settings::settingsChanged, this, &ImageViewerV2::readSettings);
}

ImageViewerV2::~ImageViewerV2() {
}

// devicePixelRatioF() does not provide correct value on wayland until the first paint event occurs
// catch change event & do the needful
bool ImageViewerV2::eventFilter(QObject *o, QEvent *ev) {
    if(ev->type() == QEvent::DevicePixelRatioChange) {
        onDPRChanged();
    }
    return QObject::eventFilter(o, ev);
}

void ImageViewerV2::onDPRChanged() {
    if(m_dpr == this->devicePixelRatioF())
        return;
    qDebug() << "DPR CHANGED " << m_dpr << " >> " << this->devicePixelRatioF();
    m_dpr = this->devicePixelRatioF();
    m_zoomThreshold = static_cast<int>(m_dpr * 4.);
    if(m_pixmap) {
        m_pixmap->setDevicePixelRatio(m_dpr);
        m_pixmapItem.setPixmap(*m_pixmap);
        m_pixmapItem.show();
        m_pixmapItem.update();
        updateMinScale();
        applyFitMode();
        requestScaling();
        update();
    }
}

void ImageViewerV2::readSettings() {
    m_transparencyGrid = settings->transparencyGrid();
    m_smoothAnimatedImages = settings->smoothAnimatedImages();
    m_smoothUpscaling = settings->smoothUpscaling();
    m_expandImage = settings->expandImage();
    m_expandLimit = static_cast<float>(settings->expandLimit());
    if(m_expandLimit < 1.0f)
        m_expandLimit = m_maxScale;
    m_imageFitModeDefault = settings->imageFitMode();
    m_zoomStep = settings->zoomStep();
    m_focusIn1to1 = settings->focusPointIn1to1Mode();
    m_trackpadDetection = settings->trackpadDetection();
    m_useFixedZoomLevels = settings->useFixedZoomLevels();
    if(m_useFixedZoomLevels) {
        // zoomlevels are stored as a string, parse into list
        m_zoomLevels.clear();
        const auto levelsStr = settings->zoomLevels().split(',');
        for(const auto &i : levelsStr)
            m_zoomLevels.append(i.toFloat());
        std::sort(m_zoomLevels.begin(), m_zoomLevels.end());
    }
    // set bg color
    onFullscreenModeChanged(m_isFullscreen);
    updateMinScale();
    setScalingFilter(settings->scalingFilter());
    setFitMode(m_imageFitModeDefault);
}

void ImageViewerV2::onFullscreenModeChanged(bool mode) {
    QColor bgColor;
    m_isFullscreen = mode;
    if(mode) {
        bgColor = settings->colorScheme().background_fullscreen;
        bgColor.setAlphaF(1.0);
    } else {
        bgColor = settings->colorScheme().background;
        bgColor.setAlphaF(settings->backgroundOpacity());
    }
    m_scene->setBackgroundBrush(bgColor);
}

void ImageViewerV2::startAnimation() {
    if(m_movie && m_movie->frameCount() > 1) {
        stopAnimation();
        emit animationPaused(false);
        //movie->jumpToFrame(0);
        //emit frameChanged(0);
        m_animationTimer->start(m_movie->nextFrameDelay());
    }
}

void ImageViewerV2::stopAnimation() {
    if(m_movie) {
        emit animationPaused(true);
        m_animationTimer->stop();
    }
}

void ImageViewerV2::pauseResume() {
    if(m_movie) {
        if(m_animationTimer->isActive())
            stopAnimation();
        else
            startAnimation();
    }
}

void ImageViewerV2::enableDrags() {
    m_dragsEnabled = true;
}

void ImageViewerV2::disableDrags() {
    m_dragsEnabled = false;
}

void ImageViewerV2::onAnimationTimer() {
    if(!m_movie)
        return;
    if(m_movie->currentFrameNumber() == m_movie->frameCount() - 1) {
        // last frame
        if(!m_loopPlayback) {
            emit animationPaused(true);
            emit playbackFinished();
            return;
        } else {
            m_movie->jumpToFrame(0);
        }
    } else {
        if(!m_movie->jumpToNextFrame()) {
            qDebug() << "[Error] QMovie:" << m_movie->lastErrorString();
            this->stopAnimation();
            return;
        }
    }
    emit frameChanged(m_movie->currentFrameNumber());
    std::unique_ptr<QPixmap> newFrame(new QPixmap());
    *newFrame = m_movie->currentPixmap();
    updatePixmap(std::move(newFrame));
    m_animationTimer->start(m_movie->nextFrameDelay());
}

void ImageViewerV2::nextFrame() {
    if(!m_movie) {
        return;
    } else if(m_movie->currentFrameNumber() == m_movie->frameCount() - 1) {
        showAnimationFrame(0);
    } else {
        showAnimationFrame(m_movie->currentFrameNumber() + 1);
    }
}

void ImageViewerV2::prevFrame() {
    if(!m_movie) {
        return;
    } else if(m_movie->currentFrameNumber() == 0) {
        showAnimationFrame(m_movie->frameCount() - 1);
    } else {
        showAnimationFrame(m_movie->currentFrameNumber() - 1);
    }
}

bool ImageViewerV2::showAnimationFrame(int frame) {
    if(!m_movie || frame < 0 || frame >= m_movie->frameCount())
        return false;
    if(m_movie->currentFrameNumber() == frame)
        return true;
    // at the first glance this may seem retarded
    // because it is
    // unfortunately i dont see a *better* way to do seeking with QMovie
    // QMovie::CacheAll is buggy and memory inefficient
    if(frame < m_movie->currentFrameNumber())
        m_movie->jumpToFrame(0);
    while(frame != m_movie->currentFrameNumber()) {
        if(!m_movie->jumpToNextFrame()) {
            qDebug() << "[Error] QMovie:" << m_movie->lastErrorString();
            break;
        }
    }
    emit frameChanged(m_movie->currentFrameNumber());
    std::unique_ptr<QPixmap> newFrame(new QPixmap());
    *newFrame = m_movie->currentPixmap();
    updatePixmap(std::move(newFrame));
    return true;
}

void ImageViewerV2::updatePixmap(std::unique_ptr<QPixmap> newPixmap) {
    m_pixmap = std::move(newPixmap);
    m_pixmap->setDevicePixelRatio(m_dpr);
    m_pixmapItem.setPixmap(*m_pixmap);
    m_pixmapItem.show();
    m_pixmapItem.update();
}

void ImageViewerV2::showAnimation(std::shared_ptr<QMovie> _movie) {
    if(_movie && _movie->isValid()) {
        reset();
        m_movie = _movie;
        m_movie->jumpToFrame(0);
        Qt::TransformationMode mode = m_smoothAnimatedImages ? Qt::SmoothTransformation : Qt::FastTransformation;
        m_pixmapItem.setTransformationMode(mode);
        std::unique_ptr<QPixmap> newFrame(new QPixmap());
        *newFrame = m_movie->currentPixmap();
        updatePixmap(std::move(newFrame));
        emit durationChanged(m_movie->frameCount());
        emit frameChanged(0);

        updateMinScale();
        if(m_imageFitMode.value() == FIT_FREE)
            m_imageFitMode = m_imageFitModeDefault;

        if(m_viewLock == LOCK_NONE) {
            applyFitMode();
            // fit original declares "I am inspecting": the viewport follows
            // the user across switches instead of re-centering
            if(m_imageFitMode.value() == FIT_ORIGINAL)
                applySavedViewportPos();
        } else {
            m_imageFitMode = FIT_FREE;
            fitFree(m_lockedScale);
            if(m_viewLock == LOCK_ALL)
                applySavedViewportPos();
        }
        startAnimation();
    }
}

// display & initialize
void ImageViewerV2::showImage(std::unique_ptr<QPixmap> _pixmap) {
    reset();
    if(_pixmap) {
        m_pixmapItemScaled.hide();
        m_pixmap = std::move(_pixmap);
        m_pixmap->setDevicePixelRatio(m_dpr);
        m_pixmapItem.setPixmap(*m_pixmap);
        Qt::TransformationMode mode = Qt::SmoothTransformation;
        if(m_scalingFilter == QI_FILTER_NEAREST)
            mode = Qt::FastTransformation;
        m_pixmapItem.setTransformationMode(mode);
        m_pixmapItem.show();
        updateMinScale();

        if(m_imageFitMode.value() == FIT_FREE)
            m_imageFitMode = m_imageFitModeDefault;

        if(m_viewLock == LOCK_NONE) {
            applyFitMode();
            if(m_imageFitMode.value() == FIT_ORIGINAL)
                applySavedViewportPos();
        } else {
            m_imageFitMode = FIT_FREE;
            fitFree(m_lockedScale);
            if(m_viewLock == LOCK_ALL)
                applySavedViewportPos();
        }
        requestScaling();
        update();
    }
}

// reset state, remove image & stop animation
void ImageViewerV2::reset() {
    stopPosAnimation();
    m_pixmapItemScaled.setPixmap(QPixmap());
    m_pixmapScaled.reset(nullptr);
    m_pixmapItem.setPixmap(QPixmap());
    m_pixmapItem.setScale(1.0f);
    m_pixmapItem.setOffset(10000, 10000);
    m_pixmap.reset();
    stopAnimation();
    m_movie = nullptr;
    centerOn(10000, 10000);
    // when this view is not in focus this it won't update the background
    // so we force it here
    viewport()->update();
}

void ImageViewerV2::closeImage() {
    reset();
}

void ImageViewerV2::setScaledPixmap(std::unique_ptr<QPixmap> newFrame) {
    if(!m_movie && newFrame->size() != scaledSizeR() * m_dpr)
        return;
    m_pixmapScaled = std::move(newFrame);
    m_pixmapScaled->setDevicePixelRatio(m_dpr);
    m_pixmapItemScaled.setPixmap(*m_pixmapScaled);
    m_pixmapItem.hide();
    m_pixmapItemScaled.show();
}

bool ImageViewerV2::isDisplaying() const {
    return (m_pixmap != nullptr);
}

void ImageViewerV2::scrollUp() {
    scroll(0, -m_defaultScrollDistance, true);
}

void ImageViewerV2::scrollDown() {
    scroll(0, m_defaultScrollDistance, true);
}

void ImageViewerV2::scrollLeft() {
    scroll(-m_defaultScrollDistance, 0, true);
}

void ImageViewerV2::scrollRight() {
    scroll(m_defaultScrollDistance, 0, true);
}

// temporary override till application restart
void ImageViewerV2::toggleTransparencyGrid() {
    m_transparencyGrid = !m_transparencyGrid;
    m_scene->update();
}

void ImageViewerV2::setScalingFilter(ScalingFilter filter) {
    if(m_scalingFilter == filter)
        return;
    m_scalingFilter = filter;
    m_pixmapItem.setTransformationMode(selectTransformationMode());
    if(m_scalingFilter == QI_FILTER_NEAREST)
        swapToOriginalPixmap();
    requestScaling();
}

void ImageViewerV2::setLoopPlayback(bool mode) {
    if(m_movie && mode && m_loopPlayback != mode)
        startAnimation();
    m_loopPlayback = mode;
}

void ImageViewerV2::setFilterNearest() {
    if(m_scalingFilter != QI_FILTER_NEAREST) {
        m_scalingFilter = QI_FILTER_NEAREST;
        m_pixmapItem.setTransformationMode(selectTransformationMode());
        swapToOriginalPixmap();
        requestScaling();
    }
}

void ImageViewerV2::setFilterBilinear() {
    if(m_scalingFilter != QI_FILTER_BILINEAR) {
        m_scalingFilter = QI_FILTER_BILINEAR;
        m_pixmapItem.setTransformationMode(selectTransformationMode());
        requestScaling();
    }
}

// returns a mode based on current zoom level and a bunch of toggles
Qt::TransformationMode ImageViewerV2::selectTransformationMode() {
    Qt::TransformationMode mode = Qt::SmoothTransformation;
    if(m_forceFastScale) {
        mode = Qt::FastTransformation;
    } else if(m_movie) {
        if(!m_smoothAnimatedImages || (m_pixmapItem.scale() > 1.0f && !m_smoothUpscaling))
            mode = Qt::FastTransformation;
    } else {
        if((m_pixmapItem.scale() > 1.0f && !m_smoothUpscaling) || m_scalingFilter == QI_FILTER_NEAREST)
            mode = Qt::FastTransformation;
    }
    return mode;
}

void ImageViewerV2::setExpandImage(bool mode) {
    m_expandImage = mode;
    updateMinScale();
    applyFitMode();
    requestScaling();
}

void ImageViewerV2::show() {
    setMouseTracking(false);
    QGraphicsView::show();
    setMouseTracking(true);
}

void ImageViewerV2::hide() {
    setMouseTracking(false);
    QWidget::hide();
}

void ImageViewerV2::requestScaling() {
    // the emitter owns its timing: while hidden (cold open, folder view) the
    // request is suppressed at the source — the visible resize/show paths
    // re-request with fresh parameters when the surface is ready
    if(!isVisible())
        return;
    if(!m_pixmap || m_pixmapItem.scale() == 1.0f || (!m_smoothUpscaling && m_pixmapItem.scale() >= 1.0f) || m_movie)
        return;
    if(m_scaleTimer->isActive())
        m_scaleTimer->stop();
    // request "real" scaling when graphicsscene scaling is insufficient
    // (it uses a single pass bilinear which is sharp but produces artifacts on low zoom levels)
    if(currentScale() < m_fastScaleThreshold)
        emit scalingRequested(scaledSizeR() * m_dpr, m_scalingFilter);
}

bool ImageViewerV2::imageFits() const {
    if(!m_pixmap)
        return true;
    return (m_pixmap->width() <= (viewport()->width() * m_dpr) && m_pixmap->height() <= (viewport()->height() * m_dpr));
}

bool ImageViewerV2::scaledImageFits() const {
    if(!m_pixmap)
        return true;
    QSize sz = scaledSizeR();
    return (sz.width() <= viewport()->width() && sz.height() <= viewport()->height());
}

ScalingFilter ImageViewerV2::scalingFilter() const {
    return m_scalingFilter;
}

QWidget *ImageViewerV2::widget() {
    return this;
}

bool ImageViewerV2::hasAnimation() const {
    return (m_movie != nullptr);
}

//  Right button zooming / dragging logic
//  mouseMoveStartPos: stores the previous mouseMoveEvent() position,
//                     used to calculate delta.
//  mousePressPos: used to filter out accidental zoom events
//  mouseInteraction: tracks which action we are performing since the last mousePressEvent()
//
void ImageViewerV2::mousePressEvent(QMouseEvent *event) {
    if(!m_pixmap) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_mouseMoveStartPos = event->pos();
    m_mousePressPos = m_mouseMoveStartPos;
    if(event->button() & Qt::RightButton) {
        setZoomAnchor(event->pos());
    } else {
        QGraphicsView::mousePressEvent(event);
    }
}

void ImageViewerV2::mouseMoveEvent(QMouseEvent *event) {
    QWidget::mouseMoveEvent(event);
    if(!m_pixmap || m_mouseInteraction == MouseInteractionState::MOUSE_DRAG ||
       m_mouseInteraction == MouseInteractionState::MOUSE_WHEEL_ZOOM)
        return;

    if(event->buttons() & Qt::LeftButton) {
        // ---------------- DRAG / PAN -------------------
        // select which action to start
        if(m_mouseInteraction == MouseInteractionState::MOUSE_NONE) {
            if(scaledImageFits()) {
                if(m_dragsEnabled)
                    m_mouseInteraction = MouseInteractionState::MOUSE_DRAG_BEGIN;
            } else {
                m_mouseInteraction = MouseInteractionState::MOUSE_PAN;
                if(cursor().shape() != Qt::ClosedHandCursor)
                    setCursor(Qt::ClosedHandCursor);
            }
        }
        // emit a signal to start dnd; set flag to ignore further mouse move events
        if(m_mouseInteraction == MouseInteractionState::MOUSE_DRAG_BEGIN) {
            if((abs(m_mousePressPos.x() - event->pos().x()) > m_dragThreshold) ||
               abs(m_mousePressPos.y() - event->pos().y()) > m_dragThreshold) {
                m_mouseInteraction = MouseInteractionState::MOUSE_NONE;
                emit draggedOut();
            }
        }
        // panning
        if(m_mouseInteraction == MouseInteractionState::MOUSE_PAN) {
            mousePan(event);
        }
        return;
    } else if(event->buttons() & Qt::RightButton) {
        // ------------------- ZOOM ----------------------
        // filter out possible mouse jitter by ignoring low delta drags
        if(m_mouseInteraction == MouseInteractionState::MOUSE_ZOOM ||
           abs(m_mousePressPos.y() - event->pos().y()) > m_zoomThreshold / m_dpr) {
            if(cursor().shape() != Qt::SizeVerCursor) {
                setCursor(Qt::SizeVerCursor);
            }
            m_mouseInteraction = MouseInteractionState::MOUSE_ZOOM;
            // avoid visible lags by forcing fast scale for large viewport sizes
            // this value possibly needs tweaking
            if(viewport()->width() * viewport()->height() > m_largeViewportSize)
                m_forceFastScale = true;
            mouseMoveZoom(event);
        }
        return;
    } else {
        event->ignore();
    }
}

void ImageViewerV2::mouseReleaseEvent(QMouseEvent *event) {
    unsetCursor();
    if(m_forceFastScale) {
        m_forceFastScale = false;
        m_pixmapItem.setTransformationMode(selectTransformationMode());
    }
    if(!m_pixmap || m_mouseInteraction == MouseInteractionState::MOUSE_NONE) {
        QGraphicsView::mouseReleaseEvent(event);
        event->ignore();
    }
    m_mouseInteraction = MouseInteractionState::MOUSE_NONE;
}

// warning for future me:
// for some reason in qgraphicsview wheelEvent is followed by moveEvent (wtf?)
void ImageViewerV2::wheelEvent(QWheelEvent *event) {
    if(event->buttons() & Qt::RightButton) {
        event->accept();
        m_mouseInteraction = MOUSE_WHEEL_ZOOM;
        int angleDelta = event->angleDelta().ry();
        if(angleDelta > 0)
            zoomInCursor();
        else if(angleDelta < 0)
            zoomOutCursor();
    } else if(event->modifiers() == Qt::NoModifier) {
        QPoint pixelDelta = event->pixelDelta();
        QPoint angleDelta = event->angleDelta();
        /* for reference
         * linux
         *   trackpad/xorg:
         *     pixelDelta = (x,y) OR (0,0)
         *     angleDelta = (x*scale,y*scale) OR (x,y)
         *   trackpad/wayland:
         *     pixelDelta = (x,y)
         *     angleDelta = (x*scale,y*scale)
         *   wheel:
         *     pixelDelta = (0,0)     - libinput <= 1.18
         *     pixelDelta = (0,120*m) - libinput 1.19
         *     angleDelta = (0,120*m)
         * -----------------------------------------
         * macOS
         *   trackpad:
         *     pixelDelta = (x,y)
         *     angleDelta = (x*scale,y*scale)
         *   wheel:
         *     pixelDelta = (0,y*scrollAccel)
         *     angleDelta = (0,120*m)
         * -----------------------------------------
         * windows
         *   trackpad:
         *     pixelDelta = (0,0)
         *     angleDelta = (x,y)
         *   wheel:
         *     pixelDelta = (0,0)
         *     AngleDelta = (0,120*m)
         */

        bool isWheel = true;
        if(m_trackpadDetection) {
            if(m_wayland) // we should have scroll phase support
                isWheel = (event->phase() == Qt::NoScrollPhase);
            else // fallback to guesswork
                isWheel = angleDelta.y() && (abs(angleDelta.y()) >= 120 && !(angleDelta.y() % 60)) &&
                          m_lastTouchpadScroll.elapsed() > 250;
        }
        //qDebug() << "isWheel:" << isWheel << " angle / pixel delta:" << angleDelta << pixelDelta << lastTouchpadScroll.elapsed() << event->phase();

        if(!isWheel) {
            m_lastTouchpadScroll.restart();
            event->accept();
            if(settings->imageScrolling() != ImageScrolling::SCROLL_NONE) {
                // scroll (high precision)
                stopPosAnimation();
                // one of these (pixel/angleDelta) may be multiplied by some scale value
                // we'll use whichever is larger
                int dx = abs(angleDelta.x()) > abs(pixelDelta.x()) ? angleDelta.x() : pixelDelta.x();
                int dy = abs(angleDelta.y()) > abs(pixelDelta.y()) ? angleDelta.y() : pixelDelta.y();
                m_hs->setValue(m_hs->value() - dx * m_trackpadScrollMultiplier);
                m_vs->setValue(m_vs->value() - dy * m_trackpadScrollMultiplier);
                centerIfNecessary();
                snapToEdges();
            }
        } else if(isWheel && settings->imageScrolling() == SCROLL_BY_TRACKPAD_AND_WHEEL) {
            // scroll by interval
            QRect imgRect = scaledRectR();
            // shift by 2px in case of img edge misalignment
            // todo: maybe even increase it to skip small distance scrolls?
            if((event->angleDelta().y() < 0 && imgRect.bottom() > height() + 2) ||
               (event->angleDelta().y() > 0 && imgRect.top() < -2)) {
                event->accept();
                scroll(0, -angleDelta.y() * m_wheelScrollMultiplier * settings->mouseScrollingSpeed(), true);
            } else {
                event->ignore(); // not scrollable; passthrough event
            }
        } else {
            event->ignore();
            QWidget::wheelEvent(event);
        }
        saveViewportPos();
    } else {
        event->ignore();
        QWidget::wheelEvent(event);
    }
}

void ImageViewerV2::showEvent(QShowEvent *event) {
    QGraphicsView::showEvent(event);
    // ensure we are properly resized
    qApp->processEvents();
    // the window-ready moment: whatever scaling was suppressed while hidden
    // is re-requested here with fresh parameters — this is "ready, then act"
    if(m_pixmap)
        requestScaling();
    // reapply fitmode to fix viewport position; the saved viewport follows
    // so a show in the middle of an inspection does not re-center
    if(m_imageFitMode.value() == FIT_ORIGINAL) {
        applyFitMode();
        applySavedViewportPos();
    }
}

void ImageViewerV2::drawBackground(QPainter *painter, const QRectF &rect) {
    QGraphicsView::drawBackground(painter, rect);
    if(!isDisplaying() || !m_transparencyGrid || !m_pixmap->hasAlphaChannel())
        return;
    painter->drawTiledPixmap(m_pixmapItem.sceneBoundingRect(), m_checkboard);
}

// simple pan behavior (cursor stops at the screen edges)
inline void ImageViewerV2::mousePan(QMouseEvent *event) {
    if(scaledImageFits())
        return;
    m_mouseMoveStartPos -= event->pos();
    scroll(m_mouseMoveStartPos.x(), m_mouseMoveStartPos.y(), false);
    m_mouseMoveStartPos = event->pos();
    saveViewportPos();
}

//  zooming while the right button is pressed
//  note: on reaching min zoom level the fitMode is set to FIT_WINDOW;
//        mid-zoom it is set to FIT_FREE.
//        FIT_FREE mode does not persist when changing images.
inline void ImageViewerV2::mouseMoveZoom(QMouseEvent *event) {
    float stepMultiplier = 0.003f; // this one feels ok
    int currentPos = event->pos().y();
    int moveDistance = m_mouseMoveStartPos.y() - currentPos;
    float newScale = currentScale() * (1.0f + stepMultiplier * moveDistance * m_dpr);
    m_mouseMoveStartPos = event->pos();
    m_imageFitMode = FIT_FREE;

    zoomAnchored(newScale);
    centerIfNecessary();
    snapToEdges();
    if(m_pixmapItem.scale() == m_fitWindowScale)
        m_imageFitMode = FIT_WINDOW;
}

// scale at which current image fills the window
void ImageViewerV2::updateFitWindowScale() {
    float scaleFitX = static_cast<float>(viewport()->width()) * m_dpr / m_pixmap->width();
    float scaleFitY = static_cast<float>(viewport()->height()) * m_dpr / m_pixmap->height();
    if(scaleFitX < scaleFitY) {
        m_fitWindowScale = scaleFitX;
    } else {
        m_fitWindowScale = scaleFitY;
    }
    if(m_expandImage && m_fitWindowScale > m_expandLimit)
        m_fitWindowScale = m_expandLimit;
}

void ImageViewerV2::updateMinScale() {
    if(!m_pixmap)
        return;
    updateFitWindowScale();
    if(settings->unlockMinZoom()) {
        if(!m_pixmap->isNull())
            m_minScale = qMax(10. / m_pixmap->width(), 10. / m_pixmap->height());
        else
            m_minScale = 1.0f;
    } else {
        if(imageFits())
            m_minScale = 1.0f;
        else
            m_minScale = m_fitWindowScale;
    }
    if(m_viewLock != LOCK_NONE && m_lockedScale < m_minScale)
        m_minScale = m_lockedScale;
}

void ImageViewerV2::fitWidth() {
    if(!m_pixmap)
        return;
    float scaleX = static_cast<float>(viewport()->width()) * m_dpr / m_pixmap->width();
    if(!m_expandImage && scaleX > 1.0f)
        scaleX = 1.0f;
    if(scaleX > m_expandLimit)
        scaleX = m_expandLimit;
    if(currentScale() != scaleX) {
        swapToOriginalPixmap();
        doZoom(scaleX);
    }
    centerIfNecessary();
    // just center somewhere at the top then do snap
    if(scaledSizeR().height() > viewport()->height()) {
        QPointF centerTarget = mapToScene(viewport()->rect()).boundingRect().center();
        centerTarget.setY(0);
        centerOn(centerTarget);
    }
    snapToEdges();
}

void ImageViewerV2::fitWindow() {
    if(!m_pixmap)
        return;
    if(imageFits() && !m_expandImage) {
        fitNormal();
    } else {
        if(currentScale() != m_fitWindowScale) {
            swapToOriginalPixmap();
            doZoom(m_fitWindowScale);
        }
        // There's either a qt bug or I am misusing something.
        // First call to scrollbar->setValue() produces wrong results
        // - unless when called from eventloop
        if(m_scrollBarWorkaround) {
            m_scrollBarWorkaround = false;
            QTimer::singleShot(0, this, SLOT(centerOnPixmap()));
        } else {
            centerOnPixmap();
        }
    }
}

void ImageViewerV2::fitNormal() {
    fitFree(1.0f);
}

void ImageViewerV2::fitFree(float scale) {
    if(!m_pixmap)
        return;
    if(m_focusIn1to1 == FOCUS_TOP) {
        doZoom(scale);
        centerIfNecessary();
        if(scaledSizeR().height() > viewport()->height()) {
            QPointF centerTarget = m_pixmapItem.sceneBoundingRect().center();
            centerTarget.setY(0);
            centerOn(centerTarget);
        }
        snapToEdges();
    } else {
        if(m_focusIn1to1 == FOCUS_CENTER)
            setZoomAnchor(viewport()->rect().center());
        else
            setZoomAnchor(mapFromGlobal(cursor().pos()));
        zoomAnchored(scale);
        centerIfNecessary();
        snapToEdges();
    }
}

void ImageViewerV2::applyFitMode() {
    switch(m_imageFitMode.value()) {
    case FIT_ORIGINAL: fitNormal(); break;
    case FIT_WIDTH: fitWidth(); break;
    case FIT_WINDOW: fitWindow(); break;
    default: break;
    }
}

// public, sends scale request
void ImageViewerV2::setFitMode(ImageFitMode newMode) {
    if(m_scaleTimer->isActive())
        m_scaleTimer->stop();
    stopPosAnimation();
    m_imageFitMode = newMode;
    applyFitMode();
    requestScaling();
}

// public, sends scale request
void ImageViewerV2::setFitOriginal() {
    setFitMode(FIT_ORIGINAL);
}

// public, sends scale request
void ImageViewerV2::setFitWidth() {
    setFitMode(FIT_WIDTH);
    requestScaling();
}

// public, sends scale request
void ImageViewerV2::setFitWindow() {
    setFitMode(FIT_WINDOW);
    requestScaling();
}

void ImageViewerV2::resizeEvent(QResizeEvent *event) {
    QGraphicsView::resizeEvent(event);
    // reset this so we won't generate unnecessary drag'n'drop event
    m_mousePressPos = mapFromGlobal(cursor().pos());
    // Qt emits some unnecessary resizeEvents on startup
    // so we try to ignore them
    if(parentWidget()->isVisible()) {
        stopPosAnimation();
        updateMinScale();
        if(m_imageFitMode.value() == FIT_FREE || m_imageFitMode.value() == FIT_ORIGINAL) {
            centerIfNecessary();
            snapToEdges();
        } else {
            applyFitMode();
        }
        update();
        if(m_scaleTimer->isActive())
            m_scaleTimer->stop();
        m_scaleTimer->start();
        saveViewportPos();
    }
}

void ImageViewerV2::centerOnPixmap() {
    auto imgRect = m_pixmapItem.sceneBoundingRect();
    auto vport = mapToScene(viewport()->geometry()).boundingRect();
    m_hs->setValue(m_pixmapItem.offset().x() - static_cast<int>((vport.width() - imgRect.width()) / 2));
    m_vs->setValue(m_pixmapItem.offset().y() - static_cast<int>((vport.height() - imgRect.height()) / 2));
}

void ImageViewerV2::stopPosAnimation() {
    if(m_scrollTimeLineX->state() == QTimeLine::Running)
        m_scrollTimeLineX->stop();
    if(m_scrollTimeLineY->state() == QTimeLine::Running)
        m_scrollTimeLineY->stop();
}

inline void ImageViewerV2::scroll(int dx, int dy, bool smooth) {
    if(smooth) {
        scrollSmooth(dx, dy);
    } else {
        scrollPrecise(dx, dy);
    }
}

void ImageViewerV2::scrollSmooth(int dx, int dy) {
    if(dx) {
        bool redirect = false;
        int currentXPos = m_hs->value();
        int newEndFrame = currentXPos + static_cast<int>(dx);
        if((newEndFrame < currentXPos && currentXPos < m_scrollTimeLineX->endFrame()) ||
           (newEndFrame > currentXPos && currentXPos > m_scrollTimeLineX->endFrame())) {
            redirect = true;
        }
        if(m_scrollTimeLineX->state() == QTimeLine::Running) {
            int oldEndFrame = m_scrollTimeLineX->endFrame();
            //if(oldEndFrame == currentYPos)
            //    createScrollTimeLine();
            if(!redirect)
                newEndFrame = oldEndFrame + static_cast<int>(dx);
        }
        m_scrollTimeLineX->stop();
        m_scrollTimeLineX->setFrameRange(currentXPos, newEndFrame);
        m_scrollTimeLineX->start();
    }
    if(dy) {
        bool redirect = false;
        int currentYPos = m_vs->value();
        int newEndFrame = currentYPos + static_cast<int>(dy);
        if((newEndFrame < currentYPos && currentYPos < m_scrollTimeLineY->endFrame()) ||
           (newEndFrame > currentYPos && currentYPos > m_scrollTimeLineY->endFrame())) {
            redirect = true;
        }
        if(m_scrollTimeLineY->state() == QTimeLine::Running) {
            int oldEndFrame = m_scrollTimeLineY->endFrame();
            //if(oldEndFrame == currentYPos)
            //    createScrollTimeLine();
            if(!redirect)
                newEndFrame = oldEndFrame + static_cast<int>(dy);
        }
        m_scrollTimeLineY->stop();
        m_scrollTimeLineY->setFrameRange(currentYPos, newEndFrame);
        m_scrollTimeLineY->start();
    }
    saveViewportPos();
}

void ImageViewerV2::scrollPrecise(int dx, int dy) {
    stopPosAnimation();
    m_hs->setValue(m_hs->value() + dx);
    m_vs->setValue(m_vs->value() + dy);
    centerIfNecessary();
    snapToEdges();
    saveViewportPos();
}

// used by scrollTimeLine: paint this frame now, and only paint it. The
// processEvents this used to run executed every queued and input event nested
// inside a half-finished frame (a wheel event restarting this very timeline).
// repaint() is synchronous and runs no event loop.
void ImageViewerV2::scrollToX(int x) {
    m_hs->setValue(x);
    centerIfNecessary();
    snapToEdges();
    viewport()->repaint();
}

// used by scrollTimeLine: paint this frame now, and only paint it. The
// processEvents this used to run executed every queued and input event nested
// inside a half-finished frame (a wheel event restarting this very timeline).
// repaint() is synchronous and runs no event loop.
void ImageViewerV2::scrollToY(int y) {
    m_vs->setValue(y);
    centerIfNecessary();
    snapToEdges();
    viewport()->repaint();
}

void ImageViewerV2::onScrollTimelineFinished() {
    saveViewportPos();
}

void ImageViewerV2::swapToOriginalPixmap() {
    if(!m_pixmap || !m_pixmapItemScaled.isVisible())
        return;
    m_pixmapItemScaled.hide();
    m_pixmapItemScaled.setPixmap(QPixmap());
    m_pixmapScaled.reset(nullptr);
    m_pixmapItem.show();
}

void ImageViewerV2::setZoomAnchor(QPoint viewportPos) {
    m_zoomAnchor = QPair<QPointF, QPoint>(m_pixmapItem.mapFromScene(mapToScene(viewportPos)), viewportPos);
}

void ImageViewerV2::zoomAnchored(float newScale) {
    if(currentScale() != newScale) {
        QPointF vportCenter = mapToScene(viewport()->geometry()).boundingRect().center();
        doZoom(newScale);
        // calculate shift to adjust viewport center
        // we do this in viewport coordinates to avoid any rounding errors
        QPointF diff = m_zoomAnchor.second - mapFromScene(m_pixmapItem.mapToScene(m_zoomAnchor.first));
        centerOn(vportCenter - diff);
        requestScaling();
    }
}

// zoom in around viewport center
void ImageViewerV2::zoomIn() {
    doZoomIn(false);
}

// zoom in around cursor if its inside window
void ImageViewerV2::zoomInCursor() {
    doZoomIn(true);
}

void ImageViewerV2::doZoomIn(bool atCursor) {
    if(atCursor && underMouse())
        setZoomAnchor(mapFromGlobal(cursor().pos()));
    else
        setZoomAnchor(viewport()->rect().center());
    float newScale = currentScale() * (1.0f + m_zoomStep);
    if(m_useFixedZoomLevels && m_zoomLevels.count()) {
        if(currentScale() < m_zoomLevels.first()) {
            newScale = qMin(currentScale() * (1.0f + m_zoomStep), m_zoomLevels.first());
        } else if(currentScale() >= m_zoomLevels.last()) {
            newScale = currentScale() * (1.0f + m_zoomStep);
        } else {
            for(int i = 0; i < m_zoomLevels.count(); i++) {
                float level = m_zoomLevels.at(i);
                if(currentScale() < level) {
                    newScale = level;
                    break;
                }
            }
        }
    }
    zoomAnchored(newScale);
    centerIfNecessary();
    snapToEdges();
    m_imageFitMode = FIT_FREE;
    if(m_pixmapItem.scale() == m_fitWindowScale)
        m_imageFitMode = FIT_WINDOW;
}

// zoom out around viewport center
void ImageViewerV2::zoomOut() {
    doZoomOut(false);
}

// zoom out around cursor if its inside window
void ImageViewerV2::zoomOutCursor() {
    doZoomOut(true);
}

void ImageViewerV2::doZoomOut(bool atCursor) {
    if(atCursor && underMouse())
        setZoomAnchor(mapFromGlobal(cursor().pos()));
    else
        setZoomAnchor(viewport()->rect().center());
    float newScale = currentScale() * (1.0f - m_zoomStep);
    if(m_useFixedZoomLevels && m_zoomLevels.count()) {
        if(currentScale() > m_zoomLevels.last()) {
            newScale = qMax(m_zoomLevels.last(), currentScale() * (1.0f - m_zoomStep));
        } else if(currentScale() <= m_zoomLevels.first()) {
            newScale = currentScale() * (1.0f - m_zoomStep);
        } else {
            for(int i = m_zoomLevels.count() - 1; i >= 0; i--) {
                float level = m_zoomLevels.at(i);
                if(currentScale() > level) {
                    newScale = level;
                    break;
                }
            }
        }
    }
    zoomAnchored(newScale);
    centerIfNecessary();
    snapToEdges();
    m_imageFitMode = FIT_FREE;
    if(m_pixmapItem.scale() == m_fitWindowScale)
        m_imageFitMode = FIT_WINDOW;
}

void ImageViewerV2::toggleLockZoom() {
    if(!isDisplaying())
        return;
    if(m_viewLock != LOCK_ZOOM) {
        m_viewLock = LOCK_ZOOM;
        lockZoom();
    } else {
        m_viewLock = LOCK_NONE;
    }
}

bool ImageViewerV2::lockZoomEnabled() {
    return (m_viewLock == LOCK_ZOOM);
}

void ImageViewerV2::lockZoom() {
    m_lockedScale = m_pixmapItem.scale();
    m_imageFitMode = FIT_FREE;
    saveViewportPos();
}

void ImageViewerV2::toggleLockView() {
    if(!isDisplaying())
        return;
    if(m_viewLock != LOCK_ALL) {
        m_viewLock = LOCK_ALL;
        lockZoom();
        saveViewportPos();
    } else {
        m_viewLock = LOCK_NONE;
    }
}

bool ImageViewerV2::lockViewEnabled() {
    return (m_viewLock == LOCK_ALL);
}

// savedViewportPos is [0...1][0...1]
// values are where viewport center is on the image
void ImageViewerV2::saveViewportPos() {
    // the position persists for LOCK_ALL, and for fit original: the mode
    // itself declares "I am inspecting", so the viewport follows the user
    // across image switches until they leave the mode
    if(m_viewLock != LOCK_ALL && m_imageFitMode.value() != FIT_ORIGINAL)
        return;
    QGraphicsPixmapItem *item = &m_pixmapItem;
    QPointF sceneCenter = mapToScene(viewport()->rect().center()) + QPointF(1, 1);
    auto itemRect = item->sceneBoundingRect();
    m_savedViewportPos.setX(qBound(qreal(0), (sceneCenter.x() - itemRect.left()) / itemRect.width(), qreal(1)));
    m_savedViewportPos.setY(qBound(qreal(0), (sceneCenter.y() - itemRect.top()) / itemRect.height(), qreal(1)));
}

void ImageViewerV2::applySavedViewportPos() {
    QGraphicsPixmapItem *item = &m_pixmapItem;
    auto itemRect = item->sceneBoundingRect();
    QPointF newScenePos;
    newScenePos.setX(itemRect.left() + itemRect.width() * m_savedViewportPos.x());
    newScenePos.setY(itemRect.top() + itemRect.height() * m_savedViewportPos.y());
    centerOn(newScenePos);
    centerIfNecessary();
    snapToEdges();
}

void ImageViewerV2::centerIfNecessary() {
    if(!m_pixmap)
        return;
    QSize sz = scaledSizeR();
    auto imgRect = m_pixmapItem.sceneBoundingRect();
    auto vport = mapToScene(viewport()->geometry()).boundingRect();
    if(sz.width() <= viewport()->width())
        m_hs->setValue(m_pixmapItem.offset().x() - static_cast<int>((vport.width() - imgRect.width()) / 2));
    if(sz.height() <= viewport()->height())
        m_vs->setValue(m_pixmapItem.offset().y() - static_cast<int>((vport.height() - imgRect.height()) / 2));
}

void ImageViewerV2::snapToEdges() {
    QRect imgRect = scaledRectR();
    // current vport center
    QPointF centerTarget = mapToScene(viewport()->rect()).boundingRect().center();
    qreal xShift = 0;
    qreal yShift = 0;
    if(imgRect.width() > width()) {
        if(imgRect.left() > 0)
            xShift = imgRect.left();
        else if(imgRect.right() < width())
            xShift = imgRect.right() - width();
    }
    if(imgRect.height() > height()) {
        if(imgRect.top() > 0)
            yShift = imgRect.top();
        else if(imgRect.bottom() < height())
            yShift = imgRect.bottom() - height();
    }
    centerOn(centerTarget + QPointF(xShift, yShift));
}

void ImageViewerV2::doZoom(float newScale) {
    if(!m_pixmap)
        return;
    newScale = qBound(m_minScale, newScale, 500.0f);
    // fix scene position to integer values
    auto tl = m_pixmapItem.sceneBoundingRect().topLeft().toPoint();
    m_pixmapItem.setOffset(tl);
    m_pixmapItem.setScale(newScale);

    m_pixmapItem.setTransformationMode(selectTransformationMode());
    swapToOriginalPixmap();
    emit scaleChanged(newScale);
}

ImageFitMode ImageViewerV2::fitMode() const {
    return m_imageFitMode.value();
}

// rounds a point in scene coordinates so it stays on the same spot on viewport
QPointF ImageViewerV2::sceneRoundPos(QPointF scenePoint) const {
    return mapToScene(mapFromScene(scenePoint));
}

// rounds a rect in scene coordinates so it stays on the same spot on viewport
// the result is what's actually drawn on screen (incl. size)
QRectF ImageViewerV2::sceneRoundRect(QRectF sceneRect) const {
    return QRectF(sceneRoundPos(sceneRect.topLeft()), sceneRect.size());
}

// size as it appears on screen (rounded)
QSize ImageViewerV2::scaledSizeR() const {
    if(!m_pixmap)
        return QSize(0, 0);
    QRectF pixmapSceneRect = m_pixmapItem.mapRectToScene(m_pixmapItem.boundingRect());
    return sceneRoundRect(pixmapSceneRect).size().toSize();
}

// in viewport coords (rounded up)
QRect ImageViewerV2::scaledRectR() const {
    QRectF pixmapSceneRect = m_pixmapItem.mapRectToScene(m_pixmapItem.boundingRect());
    return QRect(mapFromScene(pixmapSceneRect.topLeft()), mapFromScene(pixmapSceneRect.bottomRight()));
}

float ImageViewerV2::currentScale() const {
    return m_pixmapItem.scale();
}

QSize ImageViewerV2::sourceSize() const {
    if(!m_pixmap)
        return QSize(0, 0);
    return m_pixmap->size();
}
