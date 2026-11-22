#include "thumbnailview.h"
#include <utility>

ThumbnailView::ThumbnailView(Qt::Orientation _orientation, QWidget *parent)
    : QGraphicsView(parent), m_blockThumbnailLoading(false), m_cropThumbnails(false), m_mouseReleaseSelect(false),
      m_drawScrollbarIndicator(true), m_thumbnailSize(120), m_rangeSelection(false), m_selectMode(ACTIVATE_BY_PRESS),
      m_lastScrollFrameTime(0), m_scrollTimeLine(nullptr) {
    setAccessibleName("thumbnailView");
    this->setMouseTracking(true);
    this->setAcceptDrops(false);
    this->setScene(&m_scene);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setAttribute(Qt::WA_TranslucentBackground, false);
    this->setOptimizationFlag(QGraphicsView::DontAdjustForAntialiasing, true);
    this->setOptimizationFlag(QGraphicsView::DontSavePainterState, true);
    setRenderHint(QPainter::Antialiasing, false);
    setRenderHint(QPainter::SmoothPixmapTransform, false);

    setOrientation(_orientation);

    m_lastTouchpadScroll.start();

    connect(&m_loadTimer, &QTimer::timeout, this, &ThumbnailView::loadVisibleThumbnails);
    m_loadTimer.setInterval(static_cast<int>(m_loadDelay));
    m_loadTimer.setSingleShot(true);

    qreal screenMaxRefreshRate = 60;
    const QList<QScreen *> screens = qApp->screens();
    for(auto screen : screens)
        if(screen->refreshRate() > screenMaxRefreshRate)
            screenMaxRefreshRate = screen->refreshRate();
    m_scrollRefreshRate = 1000 / screenMaxRefreshRate;

    createScrollTimeLine();

    horizontalScrollBar()->setContextMenuPolicy(Qt::NoContextMenu);
    horizontalScrollBar()->installEventFilter(this);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        loadVisibleThumbnails();
    });
    verticalScrollBar()->setContextMenuPolicy(Qt::NoContextMenu);
    verticalScrollBar()->installEventFilter(this);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        loadVisibleThumbnails();
    });
    if(qApp->platformName() == "wayland")
        m_wayland = true;
}

ThumbnailView::~ThumbnailView() {
    m_rowCount = 0; // the gate, for any re-entry the disconnects miss
    m_loadTimer.disconnect();
    disconnect(horizontalScrollBar(), nullptr, this, nullptr);
    disconnect(verticalScrollBar(), nullptr, this, nullptr);
    if(m_scrollTimeLine)
        m_scrollTimeLine->disconnect();
}

Qt::Orientation ThumbnailView::orientation() const {
    return m_orientation;
}

void ThumbnailView::setOrientation(Qt::Orientation _orientation) {
    m_orientation = _orientation;
    if(m_orientation == Qt::Horizontal) {
        this->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scrollBar = this->horizontalScrollBar();
        m_centerOn = [this](int value) {
            QGraphicsView::centerOn(value + 1, m_viewportCenter.y());
        };
    } else {
        this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        this->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_scrollBar = this->verticalScrollBar();
        m_centerOn = [this](int value) {
            QGraphicsView::centerOn(m_viewportCenter.x(), value + 1);
        };
    }
}

void ThumbnailView::hideEvent(QHideEvent *event) {
    QGraphicsView::hideEvent(event);
    m_rangeSelection = false;
}

void ThumbnailView::createScrollTimeLine() {
    if(m_scrollTimeLine) {
        m_scrollTimeLine->stop();
        m_scrollTimeLine->deleteLater();
    }
    m_scrollTimeLine = new QTimeLine(m_scrollDuration, this);
    m_scrollTimeLine->setEasingCurve(QEasingCurve::OutSine);
    m_scrollTimeLine->setUpdateInterval(m_scrollRefreshRate);

    connect(m_scrollTimeLine, &QTimeLine::frameChanged, this, [this](int value) {
        m_scrollFrameTimer.start();
        this->m_centerOn(value);
        // trigger repaint immediately
        qApp->processEvents();
        m_lastScrollFrameTime = m_scrollFrameTimer.elapsed();
        if(m_scrollTimeLine->state() == QTimeLine::Running && m_lastScrollFrameTime > m_scrollRefreshRate) {
            m_scrollTimeLine->setPaused(true);
            int newTime = qMin(m_scrollTimeLine->duration(), m_scrollTimeLine->currentTime() + m_lastScrollFrameTime);
            m_scrollTimeLine->setCurrentTime(newTime);
            m_scrollTimeLine->setPaused(false);
        }
    });

    connect(m_scrollTimeLine, &QTimeLine::finished, this, [this]() {
        m_blockThumbnailLoading = false;
        loadVisibleThumbnails();
    });
}

bool ThumbnailView::eventFilter(QObject *o, QEvent *ev) {
    if(o == horizontalScrollBar() || o == verticalScrollBar()) {
        if(ev->type() == QEvent::Wheel) {
            this->wheelEvent(dynamic_cast<QWheelEvent *>(ev));
            return true;
        } else if(ev->type() == QEvent::Paint && m_drawScrollbarIndicator) {
            QPainter p(m_scrollBar);
            p.setOpacity(0.3f);
            p.fillRect(m_indicator, QBrush(Qt::gray));
            p.setOpacity(1.0f);
            return false;
        }
    }
    return QObject::eventFilter(o, ev);
}

void ThumbnailView::setDirectoryPath(QString path) {
    Q_UNUSED(path)
}

// Highlight is pushed to whichever widgets are materialized; off-window rows
// carry the selection in m_selection alone and get their highlight when their
// widget is (re)created by materializeWindow().
void ThumbnailView::select(QList<int> indices) {
    QList<int>::iterator it = indices.begin();
    while(it != indices.end()) {
        // sanity check
        if(*it < 0 || *it >= itemCount()) {
            it = indices.erase(it);
        } else {
            ++it;
        }
    }
    m_selection = indices;
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
        itw.value()->setHighlighted(m_selection.contains(itw.key()));
    updateScrollbarIndicator();
}

void ThumbnailView::select(int index) {
    // fallback
    if(!checkRange(index))
        index = 0;
    this->select(QList<int>() << index);
}

void ThumbnailView::deselect(int index) {
    if(!checkRange(index))
        return;
    if(m_selection.count() > 1) {
        m_selection.removeAll(index);
        if(auto *widget = m_visible.value(index))
            widget->setHighlighted(false);
    }
}

void ThumbnailView::addSelectionRange(int indexTo) {
    if(!m_rangeSelectionSnapshot.count() || !selection().count())
        return;
    auto list = m_rangeSelectionSnapshot;
    if(indexTo > m_rangeSelectionSnapshot.last()) {
        for(int i = m_rangeSelectionSnapshot.last() + 1; i <= indexTo; i++) {
            if(list.contains(i))
                list.removeAll(i);
            list << i;
        }
    } else {
        for(int i = m_rangeSelectionSnapshot.last() - 1; i >= indexTo; i--) {
            if(list.contains(i))
                list.removeAll(i);
            list << i;
        }
    }
    select(list);
}

QList<int> ThumbnailView::selection() {
    return m_selection;
}

void ThumbnailView::clearSelection() {
    m_selection.clear();
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
        itw.value()->setHighlighted(false);
}

int ThumbnailView::lastSelected() {
    if(!selection().count())
        return -1;
    else
        return selection().constLast();
}

// the row count, not the widget count: off-window rows exist in the model
// without widgets, and everything that used to read m_thumbnails.count() as
// "last row" reads this instead
int ThumbnailView::itemCount() {
    return m_rowCount;
}

void ThumbnailView::show() {
    QGraphicsView::show();
    focusOnSelection();
    loadVisibleThumbnails();
}

void ThumbnailView::showEvent(QShowEvent *event) {
    QGraphicsView::showEvent(event);
    // ensure we are properly resized
    qApp->processEvents();
    updateSceneExtent();
    updateScrollbarIndicator();
    loadVisibleThumbnails();
}

void ThumbnailView::populate(int newCount) {
    if(newCount < 0)
        return;
    m_lastScrollDirection = SCROLL_FORWARDS;
    applyRows(QList<DirectoryViewRow>(newCount));
    resetViewport();
    loadVisibleThumbnails();
}

// The listing diff, index-based. Widgets that follow their row (via
// previousIndex) keep identity and loaded pixmaps; rows leaving the window or
// the listing are destroyed immediately (the reuse pin holds a QPointer);
// rows entering it are materialized with their full state — highlight,
// invalidation, drag hover — re-applied from the index-based model.
void ThumbnailView::applyRows(const QList<DirectoryViewRow> &rows) {
    QList<int> newIndices(previousSizeFor(rows), -1);
    for(int index = 0; index < rows.size(); ++index)
        if(rows[index].previousIndex >= 0 && rows[index].previousIndex < m_rowCount)
            newIndices[rows[index].previousIndex] = index;

    // selection remap, unchanged semantics
    QList<int> mappedSelection;
    for(int index : std::as_const(m_selection)) {
        const int target = index < newIndices.size() ? newIndices[index] : -1;
        if(target >= 0)
            mappedSelection.append(target);
    }
    QList<int> mappedRange;
    for(int index : std::as_const(m_rangeSelectionSnapshot)) {
        const int target = index < newIndices.size() ? newIndices[index] : -1;
        if(target >= 0)
            mappedRange.append(target);
    }
    m_selection = mappedSelection;
    m_rangeSelectionSnapshot = mappedRange;
    m_rowCount = rows.size();

    // content changes lift into the index-based invalidated set
    for(int index = 0; index < rows.size(); ++index)
        if(rows[index].contentChanged)
            m_invalidated.insert(index);

    // rekey the materialized widgets: a widget follows its row, whatever the
    // window does
    QHash<int, ThumbnailWidget *> rekeyed;
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw) {
        const int newIndex = itw.key() < newIndices.size() ? newIndices[itw.key()] : -1;
        if(newIndex < 0 || newIndex >= m_rowCount) {
            delete itw.value();
            continue;
        }
        ThumbnailWidget *widget = itw.value();
        widget->setRowIndex(newIndex);
        placeWidget(widget, newIndex);
        rekeyed.insert(newIndex, widget);
    }
    m_visible = std::move(rekeyed);

    // per-row state over the materialized window: renames and invalidation
    for(int index = 0; index < rows.size(); ++index) {
        ThumbnailWidget *widget = m_visible.value(index);
        if(!widget)
            continue;
        if(!rows[index].newName.isNull() && widget->isLoaded)
            widget->renameThumbnail(rows[index].newName);
        if(m_invalidated.contains(index))
            widget->invalidateThumbnail();
    }

    materializeWindow();
    updateSceneExtent();
    updateScrollbarIndicator();
}

int ThumbnailView::previousSizeFor(const QList<DirectoryViewRow> &rows) {
    // the largest referenced previous index + 1 bounds the old listing size
    int largest = -1;
    for(const auto &row : rows)
        largest = qMax(largest, row.previousIndex);
    return largest + 1;
}

void ThumbnailView::materializeWindow() {
    if(m_rowCount == 0) {
        for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
            delete itw.value();
        m_visible.clear();
        return;
    }
    const int first = qBound(0, materializeFirst(), m_rowCount - 1);
    const int last = qBound(first, materializeLast(), m_rowCount - 1);
    // destroy out-of-window widgets
    for(auto itw = m_visible.begin(); itw != m_visible.end();) {
        if(itw.key() < first || itw.key() > last) {
            delete itw.value();
            itw = m_visible.erase(itw);
        } else {
            ++itw;
        }
    }
    // create in-window widgets with their full state
    for(int index = first; index <= last; ++index) {
        if(m_visible.contains(index))
            continue;
        ThumbnailWidget *widget = createThumbnailWidget();
        widget->setRowIndex(index);
        m_scene.addItem(widget);
        placeWidget(widget, index);
        widget->setHighlighted(m_selection.contains(index));
        if(m_dragHoverIndex == index)
            widget->setDropHovered(true);
        if(m_invalidated.contains(index))
            widget->invalidateThumbnail();
        m_visible.insert(index, widget);
    }
}

void ThumbnailView::destroyVisible(int index) {
    if(auto *widget = m_visible.take(index))
        delete widget;
}

ThumbnailWidget *ThumbnailView::createThumbnailWidget() {
    auto *widget = new ThumbnailWidget();
    configureWidget(widget);
    return widget;
}

void ThumbnailView::setDragHover(int index) {
    if(index == m_dragHoverIndex)
        return;
    if(m_dragHoverIndex >= 0)
        if(auto *widget = m_visible.value(m_dragHoverIndex))
            widget->setDropHovered(false);
    m_dragHoverIndex = index;
    if(index >= 0)
        if(auto *widget = m_visible.value(index))
            widget->setDropHovered(true);
}

void ThumbnailView::setCropThumbnails(bool mode) {
    if(mode != m_cropThumbnails) {
        unloadAllThumbnails();
        m_cropThumbnails = mode;
        loadVisibleThumbnails();
    }
}

void ThumbnailView::setDrawScrollbarIndicator(bool mode) {
    m_drawScrollbarIndicator = mode;
}

void ThumbnailView::setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) {
    if(!thumb || thumb->size() != floor(m_thumbnailSize * qApp->devicePixelRatio()) || !checkRange(pos))
        return;
    m_invalidated.remove(pos);
    if(auto *widget = m_visible.value(pos))
        widget->setThumbnail(thumb);
    // an off-window delivery is dropped: the row has no widget, and when it
    // scrolls in, loadVisibleThumbnails re-requests it (cache hit)
}

void ThumbnailView::unloadAllThumbnails() {
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
        itw.value()->unsetThumbnail();
}

// The materialize window doubles as the load window: the same viewport +
// preload band arithmetic names both the widgets that must exist and the
// rows whose thumbnails must be requested.
void ThumbnailView::loadVisibleThumbnails() {
    m_loadTimer.stop();
    if(!isVisible() || m_blockThumbnailLoading || m_rowCount == 0)
        return;
    materializeWindow();
    const int first = qBound(0, materializeFirst(), m_rowCount - 1);
    const int last = qBound(first, materializeLast(), m_rowCount - 1);
    QList<int> loadList;
    bool force = false;
    for(int index = first; index <= last; ++index) {
        ThumbnailWidget *widget = m_visible.value(index);
        if(widget && !widget->isLoaded) {
            force |= widget->needsFreshThumbnail() || m_invalidated.contains(index);
            loadList.append(index);
        }
    }
    if(loadList.count())
        emit thumbnailsRequested(loadList, static_cast<int>(qApp->devicePixelRatio() * m_thumbnailSize),
                                 m_cropThumbnails, force);
    // unload pixmaps off-window: widgets stay, pixels go
    if(settings->unloadThumbs()) {
        for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
            if(itw.key() < first || itw.key() > last)
                itw.value()->unsetThumbnail();
    }
}

void ThumbnailView::loadVisibleThumbnailsDelayed() {
    m_loadTimer.stop();
    m_loadTimer.start();
}

void ThumbnailView::resetViewport() {
    if(m_scrollTimeLine->state() == QTimeLine::Running)
        m_scrollTimeLine->stop();
    m_scrollBar->setValue(0);
}

int ThumbnailView::thumbnailSize() {
    return m_thumbnailSize;
}

bool ThumbnailView::atSceneStart() {
    if(m_orientation == Qt::Horizontal) {
        if(viewportTransform().dx() == 0.0)
            return true;
    } else {
        if(viewportTransform().dy() == 0.0)
            return true;
    }
    return false;
}

bool ThumbnailView::atSceneEnd() {
    if(m_orientation == Qt::Horizontal) {
        if(viewportTransform().dx() == viewport()->width() - sceneRect().width())
            return true;
    } else {
        if(viewportTransform().dy() == viewport()->height() - sceneRect().height())
            return true;
    }
    return false;
}

bool ThumbnailView::checkRange(int pos) {
    return pos >= 0 && pos < m_rowCount;
}

//################### scrolling ######################
void ThumbnailView::wheelEvent(QWheelEvent *event) {
    event->accept();

    int pixelDelta = event->pixelDelta().y();
    int angleDelta = event->angleDelta().y();
    bool isWheel = true;
    if(settings->trackpadDetection()) {
        if(m_wayland) // we should have scroll phase support
            isWheel = (event->phase() == Qt::NoScrollPhase);
        else // fallback to guesswork
            isWheel =
                angleDelta && (abs(angleDelta) >= 120 && !(angleDelta % 60)) && m_lastTouchpadScroll.elapsed() > 250;
    }

    if(isWheel) {
        angleDelta *= settings->mouseScrollingSpeed();
        if(!settings->enableSmoothScroll()) {
            if(pixelDelta)
                scrollByItem(pixelDelta);
            else if(angleDelta)
                scrollByItem(angleDelta);
        } else if(angleDelta) { // what about pixelDelta?
            scrollSmooth(angleDelta, m_wheelScrollMultiplier, m_scrollAcceleration, true);
        }
    } else {
        m_lastTouchpadScroll.restart();
        // one of these (pixel/angleDelta) may be multiplied by some scale value
        // we'll use whichever is larger
        bool useAngleDelta = abs(angleDelta) > abs(pixelDelta);
        if(useAngleDelta)
            scrollPrecise(angleDelta);
        else
            scrollPrecise(pixelDelta);
    }
}

void ThumbnailView::scrollPrecise(int delta) {
    if(delta < 0)
        m_lastScrollDirection = SCROLL_FORWARDS;
    else
        m_lastScrollDirection = SCROLL_BACKWARDS;
    m_viewportCenter = mapToScene(viewport()->rect().center());
    if(m_scrollTimeLine->state() == QTimeLine::Running) {
        m_scrollTimeLine->stop();
        m_blockThumbnailLoading = false;
    }
    // ignore if we reached boundaries
    if((delta > 0 && atSceneStart()) || (delta < 0 && atSceneEnd()))
        return;
    // pixel scrolling (precise)
    if(m_orientation == Qt::Horizontal)
        m_centerOn(static_cast<int>(m_viewportCenter.x() - delta));
    else
        m_centerOn(static_cast<int>(m_viewportCenter.y() - delta));
    loadVisibleThumbnails();
}

// windows explorer-like behavior
// scrolls exactly by item width / height
void ThumbnailView::scrollByItem(int delta) {
    // do not scroll less than a certain value in px, to avoid feeling unresponsive
    int minScroll = qMin(thumbnailSize() / 2, 100);
    QRectF visRect =
        mapToScene(viewport()->geometry()).boundingRect().adjusted(-minScroll, -minScroll, minScroll, minScroll);
    if(m_rowCount == 0 || m_visible.isEmpty())
        return;
    int firstVisible = m_rowCount, lastVisible = -1;
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw) {
        if(cellSceneRect(itw.key()).intersects(visRect)) {
            firstVisible = qMin(firstVisible, itw.key());
            lastVisible = qMax(lastVisible, itw.key());
        }
    }
    if(lastVisible < 0)
        return;
    int target = 0;
    if(delta > 0) // up / left
        target = firstVisible - 1;
    else // down / right
        target = lastVisible + 1;
    scrollToItem(target);
}

// the scroll target's geometry is arithmetic: a row without a widget is
// still a rect in the scene
void ThumbnailView::scrollToItem(int index) {
    if(!checkRange(index))
        return;
    QRectF sceneRect = mapToScene(viewport()->rect()).boundingRect();
    QRectF itemRect = cellSceneRect(index);
    bool visible = sceneRect.contains(itemRect);
    if(!visible) {
        int delta = 0;
        if(m_orientation == Qt::Vertical) {
            if(itemRect.top() >= sceneRect.top()) // UP
                delta = sceneRect.bottom() - itemRect.bottom();
            else // DOWN
                delta = sceneRect.top() - itemRect.top();
        } else {
            if(itemRect.left() >= sceneRect.left()) // LEFT
                delta = sceneRect.right() - itemRect.right();
            else // RIGHT
                delta = sceneRect.left() - itemRect.left();
        }
        if(settings->enableSmoothScroll())
            scrollSmooth(delta);
        else
            scrollPrecise(delta);
    }
}

void ThumbnailView::scrollSmooth(int delta, qreal multiplier, qreal acceleration, bool additive) {
    if(delta < 0)
        m_lastScrollDirection = SCROLL_FORWARDS;
    else
        m_lastScrollDirection = SCROLL_BACKWARDS;
    m_viewportCenter = mapToScene(viewport()->rect().center());
    // ignore if we reached boundaries
    if((delta > 0 && atSceneStart()) || (delta < 0 && atSceneEnd())) {
        return;
    }
    int center;
    if(m_orientation == Qt::Horizontal)
        center = static_cast<int>(m_viewportCenter.x());
    else
        center = static_cast<int>(m_viewportCenter.y());
    bool redirect = false, accelerate = false;
    int newEndFrame = center - static_cast<int>(delta * multiplier);
    if((newEndFrame < center && center < m_scrollTimeLine->endFrame()) ||
       (newEndFrame > center && center > m_scrollTimeLine->endFrame())) {
        redirect = true;
    }
    if(m_scrollTimeLine->state() == QTimeLine::Running || m_scrollTimeLine->state() == QTimeLine::Paused) {
        int oldEndFrame = m_scrollTimeLine->endFrame();
        if(m_scrollTimeLine->currentTime() < m_scrollAccelerationThreshold)
            accelerate = true;
        // QTimeLine has this weird issue when it is already finished (at the last frame)
        // but is stuck in the running state. So we just create a new one.
        if(oldEndFrame == center)
            createScrollTimeLine();
        if(!redirect && additive)
            newEndFrame = oldEndFrame - static_cast<int>(delta * multiplier * acceleration);
        // force load thumbs inbetween scroll animations
        m_blockThumbnailLoading = false;
        loadVisibleThumbnails();
    }
    m_scrollTimeLine->stop();
    if(accelerate)
        m_scrollTimeLine->setDuration(m_scrollDuration / m_scrollAcceleration);
    else
        m_scrollTimeLine->setDuration(m_scrollDuration);
    m_blockThumbnailLoading = true;
    m_scrollTimeLine->setFrameRange(center, newEndFrame);
    m_scrollTimeLine->start();
}

void ThumbnailView::scrollSmooth(int delta, qreal multiplier, qreal acceleration) {
    scrollSmooth(delta, multiplier, acceleration, false);
}

void ThumbnailView::scrollSmooth(int angleDelta) {
    scrollSmooth(angleDelta, 1.0, 1.0, false);
}

void ThumbnailView::mousePressEvent(QMouseEvent *event) {
    m_mouseReleaseSelect = false;
    m_dragStartPos = QPoint(0, 0);
    auto *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->pos()));
    if(item) {
        int index = item->rowIndex();
        if(event->button() == Qt::LeftButton) {
            if(event->modifiers() & Qt::ControlModifier) {
                if(!selection().contains(index))
                    select(selection() << index);
                else
                    deselect(index);
            } else if(event->modifiers() & Qt::ShiftModifier) {
                addSelectionRange(index);
            } else if(selection().count() <= 1) {
                if(m_selectMode == ACTIVATE_BY_PRESS) {
                    emit itemActivated(index);
                    return;
                } else {
                    select(index);
                }
            } else {
                m_mouseReleaseSelect = true;
            }
            m_dragStartPos = event->pos();
        } else if(event->button() == Qt::RightButton) { // todo: context menu maybe?
            select(index);
            return;
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void ThumbnailView::mouseMoveEvent(QMouseEvent *event) {
    QGraphicsView::mouseMoveEvent(event);
    if(event->buttons() != Qt::LeftButton || !selection().count())
        return;
    if(QLineF(m_dragStartPos, event->pos()).length() >= 40) {
        auto *item = dynamic_cast<ThumbnailWidget *>(itemAt(m_dragStartPos));
        if(item && selection().contains(item->rowIndex())) {
            emit draggedOut();
        }
    }
}

void ThumbnailView::mouseReleaseEvent(QMouseEvent *event) {
    QGraphicsView::mouseReleaseEvent(event);
    if(m_mouseReleaseSelect && QLineF(m_dragStartPos, event->pos()).length() < 40) {
        auto *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->pos()));
        if(item) {
            select(item->rowIndex());
        }
    }
}

void ThumbnailView::mouseDoubleClickEvent(QMouseEvent *event) {
    if(event->button() == Qt::LeftButton) {
        auto *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->pos()));
        if(item) {
            emit itemActivated(item->rowIndex());
            return;
        }
    }
    event->ignore();
}

void ThumbnailView::focusOutEvent(QFocusEvent *event) {
    QGraphicsView::focusOutEvent(event);
    m_rangeSelection = false;
}

void ThumbnailView::keyPressEvent(QKeyEvent *event) {
    if(event->key() == Qt::Key_Shift)
        m_rangeSelectionSnapshot = selection();
    if(event->modifiers() & Qt::ShiftModifier)
        m_rangeSelection = true;
}

void ThumbnailView::keyReleaseEvent(QKeyEvent *event) {
    if(event->key() == Qt::Key_Shift)
        m_rangeSelection = false;
}

void ThumbnailView::resizeEvent(QResizeEvent *event) {
    QGraphicsView::resizeEvent(event);
    updateScrollbarIndicator();
}

void ThumbnailView::setSelectMode(ThumbnailSelectMode mode) {
    m_selectMode = mode;
}
