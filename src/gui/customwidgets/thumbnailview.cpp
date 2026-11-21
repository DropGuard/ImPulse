#include "thumbnailview.h"
#include "gui/customwidgets/selectionremap.h"
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

Qt::Orientation ThumbnailView::orientation() {
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
    // todo: layout
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

void ThumbnailView::select(QList<int> indices) {
    for(auto i : std::as_const(m_selection))
        m_thumbnails.at(i)->setHighlighted(false);
    QList<int>::iterator it = indices.begin();
    while(it != indices.end()) {
        // sanity check
        if(*it < 0 || *it >= itemCount()) {
            it = indices.erase(it);
        } else {
            m_thumbnails.at(*it)->setHighlighted(true);
            ++it;
        }
    }
    m_selection = indices;
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
        m_thumbnails.at(index)->setHighlighted(false);
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
    for(auto i : std::as_const(m_selection))
        m_thumbnails.at(i)->setHighlighted(false);
    m_selection.clear();
}

int ThumbnailView::lastSelected() {
    if(!selection().count())
        return -1;
    else
        return selection().constLast();
}

int ThumbnailView::itemCount() {
    return m_thumbnails.count();
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
    updateScrollbarIndicator();
    loadVisibleThumbnails();
}

void ThumbnailView::populate(int newCount) {
    // wait for possible queued layout events before removing items
    qApp->processEvents();

    clearSelection();
    // reset
    m_lastScrollDirection = SCROLL_FORWARDS;
    // pause updates until the layout is calculated
    // without this you will see scene moving when scrollbar appears
    this->setUpdatesEnabled(false);
    QElapsedTimer t;
    t.start();
    if(newCount >= 0) {
        /* test this later
        // reuse existing items
        auto currentCount = thumbnails.count();
        if(currentCount > newCount) {
            qDebug() << this << "removing";
            for(auto i = currentCount - 1; i >= newCount; i--) {
                //removeItemFromLayout(i); // slow. is this needed?
                delete thumbnails.takeLast();
            }
            // reset existing
            QList<ThumbnailWidget*>::iterator i;
            for(i = thumbnails.begin(); i != thumbnails.end(); ++i)
                (*i)->reset();
        } else if(currentCount <= newCount) {
            // reset existing
            QList<ThumbnailWidget*>::iterator i;
            for(i = thumbnails.begin(); i != thumbnails.end(); ++i)
                (*i)->reset();
            qDebug() << this << "adding";
            for(auto i = currentCount; i < newCount; i++) {
                ThumbnailWidget *widget = createThumbnailWidget();
                widget->setThumbnailSize(m_thumbnailSize);
                thumbnails.append(widget);
                addItemToLayout(widget, i);
            }
        }
        */

        if(newCount == m_thumbnails.count()) {
            QList<ThumbnailWidget *>::iterator i;
            for(i = m_thumbnails.begin(); i != m_thumbnails.end(); ++i) {
                (*i)->reset();
            }
        } else {
            removeAll();
            for(int i = 0; i < newCount; i++) {
                ThumbnailWidget *widget = createThumbnailWidget();
                widget->setThumbnailSize(m_thumbnailSize);
                m_thumbnails.append(widget);
                addItemToLayout(widget, i);
            }
        }
    }
    updateLayout();
    fitSceneToContents();
    resetViewport();
    //qDebug() << "_______POPULATE" << this << t.elapsed();
    // wait for layout before updating
    qApp->processEvents();
    this->setUpdatesEnabled(true);
    loadVisibleThumbnails();
}

void ThumbnailView::addItem() {
    insertItem(m_thumbnails.count());
}

// insert at index
void ThumbnailView::insertItem(int index) {
    ThumbnailWidget *widget = createThumbnailWidget();
    m_thumbnails.insert(index, widget);
    addItemToLayout(widget, index);
    updateLayout();
    fitSceneToContents();

    auto newSelection = m_selection;
    for(int i = 0; i < newSelection.count(); i++) {
        if(index <= newSelection[i])
            newSelection[i]++;
    }
    select(newSelection);

    updateScrollbarIndicator();
    loadVisibleThumbnails();
}

void ThumbnailView::removeItem(int index) {
    if(checkRange(index)) {
        auto newSelection = m_selection;
        clearSelection();
        removeItemFromLayout(index);
        delete m_thumbnails.takeAt(index);
        fitSceneToContents();
        newSelection.removeAll(index);
        for(int i = 0; i < newSelection.count(); i++) {
            if(newSelection[i] >= index)
                newSelection[i]--;
        }
        if(!newSelection.count() && itemCount())
            newSelection << ((index >= itemCount()) ? itemCount() - 1 : index);
        select(newSelection);
        updateScrollbarIndicator();
        loadVisibleThumbnails();
    }
}

void ThumbnailView::reloadItem(int index) {
    if(!checkRange(index))
        return;
    auto thumb = m_thumbnails.at(index);
    if(thumb->isLoaded)
        thumb->unsetThumbnail();
    emit thumbnailsRequested(QList<int>() << index, static_cast<int>(qApp->devicePixelRatio() * m_thumbnailSize),
                             m_cropThumbnails, true);
}

// One row moving: the widget keeps its identity, and so does the picture it
// holds. removeItem+insertItem would have destroyed both, asked for a decode
// the disk cache cannot answer for a renamed path, and renumbered every
// selection index around the hole — for a file whose bytes never changed.
void ThumbnailView::renameItem(int oldIndex, int newIndex, const QString &name) {
    if(!checkRange(oldIndex) || !checkRange(newIndex))
        return;
    ThumbnailWidget *widget = m_thumbnails.at(oldIndex);
    widget->renameThumbnail(name);
    if(oldIndex == newIndex)
        return;

    // the two layout hooks read the row list on either side of the change:
    // removeItemFromLayout expects the widget still in it, addItemToLayout
    // expects it already re-seated
    removeItemFromLayout(oldIndex);
    m_thumbnails.move(oldIndex, newIndex);
    addItemToLayout(widget, newIndex);
    updateLayout();

    // the same permutation the rows went through, applied to the selection —
    // the shared rule lives in remapSelectionForRename, one copy for this view
    // and the viewless proxy
    auto newSelection = m_selection;
    remapSelectionForRename(newSelection, oldIndex, newIndex);
    select(newSelection);
}

void ThumbnailView::setDragHover(int index) {
    Q_UNUSED(index)
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
    if(thumb && thumb->size() == floor(m_thumbnailSize * qApp->devicePixelRatio()) && checkRange(pos)) {
        m_thumbnails.at(pos)->setThumbnail(thumb);
    }
}

void ThumbnailView::unloadAllThumbnails() {
    for(int i = 0; i < m_thumbnails.count(); i++) {
        m_thumbnails.at(i)->unsetThumbnail();
    }
}

void ThumbnailView::loadVisibleThumbnails() {
    m_loadTimer.stop();
    if(isVisible() && !m_blockThumbnailLoading) {
        QRectF visRect = mapToScene(viewport()->geometry()).boundingRect();
        QRectF offRectBack;
        QRectF offRectFront;
        if(m_orientation == Qt::Horizontal) {
            offRectBack = QRectF(visRect.left() - m_offscreenPreloadArea, visRect.top(), m_offscreenPreloadArea,
                                 visRect.height());
            offRectFront = QRectF(visRect.right(), visRect.top(), m_offscreenPreloadArea, visRect.height());
        } else {
            offRectBack =
                QRectF(visRect.left(), visRect.top() - m_offscreenPreloadArea, visRect.width(), m_offscreenPreloadArea);
            offRectFront = QRectF(visRect.left(), visRect.bottom(), visRect.width(), m_offscreenPreloadArea);
        }
        QList<QGraphicsItem *> visibleItems;
        if(m_lastScrollDirection == SCROLL_FORWARDS)
            visibleItems = m_scene.items(visRect, Qt::IntersectsItemBoundingRect, Qt::AscendingOrder);
        else
            visibleItems = m_scene.items(visRect, Qt::IntersectsItemBoundingRect, Qt::DescendingOrder);
        visibleItems.append(m_scene.items(offRectBack, Qt::IntersectsItemBoundingRect, Qt::DescendingOrder));
        visibleItems.append(m_scene.items(offRectFront, Qt::IntersectsItemBoundingRect, Qt::AscendingOrder));
        // select
        QList<int> loadList;
        for(int i = 0; i < visibleItems.count(); i++) {
            ThumbnailWidget *widget = qgraphicsitem_cast<ThumbnailWidget *>(visibleItems.at(i));
            if(widget && !widget->isLoaded) {
                int idx = m_thumbnails.indexOf(widget);
                if(!loadList.contains(idx))
                    loadList.append(idx);
            }
        }
        // load
        if(loadList.count())
            emit thumbnailsRequested(loadList, static_cast<int>(qApp->devicePixelRatio() * m_thumbnailSize),
                                     m_cropThumbnails, false);
        // unload offscreen
        if(settings->unloadThumbs()) {
            for(int i = 0; i < m_thumbnails.count(); i++)
                if(!visibleItems.contains(m_thumbnails.at(i)))
                    m_thumbnails.at(i)->unsetThumbnail();
        }
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
    return pos >= 0 && pos < m_thumbnails.count();
}

void ThumbnailView::updateLayout() {
}

// fit scene to it's contents size
void ThumbnailView::fitSceneToContents() {
    QPointF center;
    if(this->m_orientation == Qt::Vertical) {
        int height = qMax(static_cast<int>(m_scene.itemsBoundingRect().height()), this->height());
        m_scene.setSceneRect(QRectF(0, 0, this->width(), height));
        center = mapToScene(viewport()->rect().center());
        QGraphicsView::centerOn(0, center.y() + 1);
    } else {
        int width = qMax(static_cast<int>(m_scene.itemsBoundingRect().width()), this->width());
        m_scene.setSceneRect(QRectF(0, 0, width, this->height()));
        center = mapToScene(viewport()->rect().center());
        QGraphicsView::centerOn(center.x() + 1, 0);
    }
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
    //qDebug() << "isWheel:" << isWheel << " angle / pixel delta:" << angleDelta << pixelDelta << lastTouchpadScroll.elapsed() << event->phase() << " wayland:" << wayland;

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
}

// windows explorer-like behavior
// scrolls exactly by item width / height
void ThumbnailView::scrollByItem(int delta) {
    // do not scroll less than a certain value in px, to avoid feeling unresponsive
    int minScroll = qMin(thumbnailSize() / 2, 100);
    // grab fully visible thumbs
    QRectF visRect =
        mapToScene(viewport()->geometry()).boundingRect().adjusted(-minScroll, -minScroll, minScroll, minScroll);
    QList<QGraphicsItem *> visibleItems;
    visibleItems = m_scene.items(visRect, Qt::ContainsItemBoundingRect, Qt::AscendingOrder);
    if(m_thumbnails.isEmpty() || visibleItems.isEmpty())
        return;
    int target = 0;
    // select scroll target
    if(delta > 0) { // up / left
        ThumbnailWidget *widget = qgraphicsitem_cast<ThumbnailWidget *>(visibleItems.first());
        if(!widget)
            return;
        target = m_thumbnails.indexOf(widget) - 1;
    } else { // down / right
        ThumbnailWidget *widget = qgraphicsitem_cast<ThumbnailWidget *>(visibleItems.last());
        if(!widget)
            return;
        target = m_thumbnails.indexOf(widget) + 1;
    }
    scrollToItem(target);
}

void ThumbnailView::scrollToItem(int index) {
    if(!checkRange(index))
        return;
    ThumbnailWidget *item = m_thumbnails.at(index);
    QRectF sceneRect = mapToScene(viewport()->rect()).boundingRect();
    QRectF itemRect = item->mapRectToScene(item->rect());
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
    ThumbnailWidget *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->pos()));
    if(item) {
        int index = m_thumbnails.indexOf(item);
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
        if(item && selection().contains(m_thumbnails.indexOf(item))) {
            emit draggedOut();
        }
    }
}

void ThumbnailView::mouseReleaseEvent(QMouseEvent *event) {
    QGraphicsView::mouseReleaseEvent(event);
    if(m_mouseReleaseSelect && QLineF(m_dragStartPos, event->pos()).length() < 40) {
        ThumbnailWidget *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->pos()));
        if(item) {
            int index = m_thumbnails.indexOf(item);
            select(index);
        }
    }
}

void ThumbnailView::mouseDoubleClickEvent(QMouseEvent *event) {
    if(event->button() == Qt::LeftButton) {
        ThumbnailWidget *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->pos()));
        if(item) {
            emit itemActivated(m_thumbnails.indexOf(item));
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
