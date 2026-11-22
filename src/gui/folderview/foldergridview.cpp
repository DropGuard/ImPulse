#include "foldergridview.h"

#include <QMimeData>

// TODO: create a base class for this and the one on panel

FolderGridView::FolderGridView(QWidget *parent) : ThumbnailView(Qt::Vertical, parent), m_shiftedCol(-1) {
    m_offscreenPreloadArea = 2300;

    this->setAcceptDrops(true);

    this->viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    this->m_scene.setBackgroundBrush(settings->colorScheme().folderview);
    this->setCacheMode(QGraphicsView::CacheBackground);

    // turn this off until [multi]selection is implemented
    setDrawScrollbarIndicator(false);
    setSelectMode(ACTIVATE_BY_DOUBLECLICK);

    // `settings` outlives every widget, so the connection must die with this one
    connect(settings, &Settings::settingsChanged, this, [this]() {
        this->m_scene.setBackgroundBrush(settings->colorScheme().folderview);
    });

    setupLayout();
    connect(this, &ThumbnailView::itemActivated, this, &FolderGridView::onitemSelected);
}

FolderGridView::~FolderGridView() {
    m_flowLayout->clear();
    qDeleteAll(m_thumbnails);
    m_thumbnails.clear();
}

void FolderGridView::dropEvent(QDropEvent *event) {
    event->accept();
    ThumbnailWidget *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->position().toPoint()));
    int index = -1;
    if(item) {
        index = m_thumbnails.indexOf(item);
        item->setDropHovered(false);
    }
    emit droppedInto(event->mimeData(), event->source(), index);
}

void FolderGridView::dragEnterEvent(QDragEnterEvent *event) {
    event->accept();
}

void FolderGridView::dragMoveEvent(QDragMoveEvent *event) {
    event->accept();
    ThumbnailWidget *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->position().toPoint()));
    int index = -1;
    if(item)
        index = m_thumbnails.indexOf(item);
    // unselect previous
    if(index != m_lastDragTarget && checkRange(m_lastDragTarget))
        m_thumbnails.at(m_lastDragTarget)->setDropHovered(false);
    emit draggedOver(index);
    m_lastDragTarget = index;
}

void FolderGridView::dragLeaveEvent(QDragLeaveEvent *event) {
    event->accept();
    if(m_lastDragTarget < 0 || m_lastDragTarget >= m_thumbnails.count())
        return;
    m_thumbnails.at(m_lastDragTarget)->setDropHovered(false);
}

void FolderGridView::setDragHover(int index) {
    if(!checkRange(index))
        return;
    auto item = m_thumbnails.at(index);
    item->setDropHovered(true);
}

void FolderGridView::onitemSelected() {
    m_shiftedCol = -1;
}

void FolderGridView::updateScrollbarIndicator() {
    if(!m_thumbnails.count() || !selection().count())
        return;
    ThumbnailWidget *thumb = m_thumbnails.at(lastSelected());
    qreal itemCenter = (thumb->pos().y() + (thumb->height() / 2)) / m_scene.height();
    m_indicator =
        QRect(2, m_scrollBar->height() * itemCenter - m_indicatorSize, m_scrollBar->width() - 4, m_indicatorSize);
}

// probably unneeded
void FolderGridView::show() {
    ThumbnailView::show();
    setFocus();
}

// probably unneeded
void FolderGridView::hide() {
    ThumbnailView::hide();
    clearFocus();
}

void FolderGridView::setShowLabels(bool mode) {
    ThumbnailStyle style = mode ? THUMB_NORMAL : THUMB_SIMPLE;
    for(int i = 0; i < m_thumbnails.count(); i++)
        m_thumbnails.at(i)->setThumbStyle(style);
    updateLayout();
    fitSceneToContents();
    focusOnSelection();
}

void FolderGridView::focusOnSelection() {
    if(!m_thumbnails.count() || lastSelected() == -1)
        return;
    ThumbnailWidget *thumb = m_thumbnails.at(lastSelected());
    ensureVisible(thumb, 0, 0);
}

void FolderGridView::selectAll() {
    QList<int> list;
    for(int i = 0; i < m_thumbnails.count(); i++)
        list << i;
    // preserve last selected index by putting it at the end of a new selection
    // this is simpler but it changes selection order a bit
    if(lastSelected() != -1) {
        // in this case list is sorted so no need to indexOf()
        list.move(lastSelected(), list.count() - 1);
    }
    select(list);
}

void FolderGridView::selectAbove() {
    if(!m_thumbnails.count() || lastSelected() == -1 || m_flowLayout->sameRow(0, lastSelected()))
        return;
    int newIndex;
    newIndex = m_flowLayout->itemAbove(lastSelected());
    if(m_shiftedCol >= 0) {
        int diff = m_shiftedCol - m_flowLayout->columnOf(lastSelected());
        newIndex += diff;
        m_shiftedCol = -1;
    }
    if(!checkRange(newIndex))
        newIndex = 0;
    if(m_rangeSelection)
        addSelectionRange(newIndex);
    else
        select(newIndex);
    scrollToCurrent();
}

void FolderGridView::selectBelow() {
    if(!m_thumbnails.count() || lastSelected() == -1 || m_flowLayout->sameRow(lastSelected(), m_thumbnails.count() - 1))
        return;
    m_shiftedCol = -1;
    int newIndex = m_flowLayout->itemBelow(lastSelected());
    if(!checkRange(newIndex))
        newIndex = m_thumbnails.count() - 1;
    if(m_flowLayout->columnOf(newIndex) != m_flowLayout->columnOf(lastSelected()))
        m_shiftedCol = m_flowLayout->columnOf(lastSelected());
    if(m_rangeSelection)
        addSelectionRange(newIndex);
    else
        select(newIndex);
    scrollToCurrent();
}

void FolderGridView::selectNext() {
    if(!m_thumbnails.count() || lastSelected() == m_thumbnails.count() - 1)
        return;
    if(!m_rangeSelection && lastSelected() == m_thumbnails.count() - 1) {
        select(lastSelected());
        return;
    }
    m_shiftedCol = -1;
    int newIndex = lastSelected() + 1;
    if(!checkRange(newIndex))
        newIndex = m_thumbnails.count() - 1;
    if(m_rangeSelection)
        addSelectionRange(newIndex);
    else
        select(newIndex);
    scrollToCurrent();
}

void FolderGridView::selectPrev() {
    if(!m_thumbnails.count() || lastSelected() == 0)
        return;
    m_shiftedCol = -1;
    int newIndex = lastSelected() - 1;
    if(!checkRange(newIndex))
        newIndex = 0;
    if(m_rangeSelection)
        addSelectionRange(newIndex);
    else
        select(newIndex);
    scrollToCurrent();
}

void FolderGridView::activateCurrent() {
    const int current = lastSelected();
    if(current >= 0)
        emit itemActivated(current);
}

void FolderGridView::pageUp() {
    if(!m_thumbnails.count() || lastSelected() == -1 || m_flowLayout->sameRow(0, lastSelected()))
        return;
    int newIndex = lastSelected();
    int tmp;
    // 4 rows up
    for(int i = 0; i < 4; i++) {
        tmp = m_flowLayout->itemAbove(newIndex);
        if(checkRange(tmp))
            newIndex = tmp;
    }
    if(m_shiftedCol >= 0) {
        int diff = m_shiftedCol - m_flowLayout->columnOf(newIndex);
        newIndex += diff;
        m_shiftedCol = -1;
    }
    if(m_rangeSelection)
        addSelectionRange(newIndex);
    else
        select(newIndex);
    scrollToCurrent();
}

void FolderGridView::pageDown() {
    if(!m_thumbnails.count() || lastSelected() == -1 || m_flowLayout->sameRow(lastSelected(), m_thumbnails.count() - 1))
        return;
    m_shiftedCol = -1;
    int newIndex = lastSelected();
    int tmp;
    // 4 rows down
    for(int i = 0; i < 4; i++) {
        tmp = m_flowLayout->itemBelow(newIndex);
        if(checkRange(tmp))
            newIndex = tmp;
    }
    if(m_flowLayout->columnOf(newIndex) != m_flowLayout->columnOf(lastSelected()))
        m_shiftedCol = m_flowLayout->columnOf(lastSelected());
    if(m_rangeSelection)
        addSelectionRange(newIndex);
    else
        select(newIndex);
    scrollToCurrent();
}

void FolderGridView::selectFirst() {
    if(!m_thumbnails.count())
        return;
    m_shiftedCol = -1;
    if(m_rangeSelection)
        addSelectionRange(0);
    else
        select(0);
    scrollToCurrent();
}

void FolderGridView::selectLast() {
    if(!m_thumbnails.count())
        return;
    m_shiftedCol = -1;
    if(m_rangeSelection)
        addSelectionRange(m_thumbnails.count() - 1);
    else
        select(m_thumbnails.count() - 1);
    scrollToCurrent();
}

void FolderGridView::scrollToCurrent() {
    scrollToItem(lastSelected());
}

// same as scrollToItem minus the animation
void FolderGridView::focusOn(int index) {
    if(!checkRange(index))
        return;
    ThumbnailWidget *thumb = m_thumbnails.at(index);
    ensureVisible(thumb, 0, 0);
    loadVisibleThumbnailsDelayed();
}

void FolderGridView::setupLayout() {
    this->setAlignment(Qt::AlignHCenter);

    m_flowLayout = new FlowLayout();
    m_flowLayout->setContentsMargins(9, 6, 9, 0);
    setFrameShape(QFrame::NoFrame);
    m_scene.addItem(&m_holderWidget);
    m_holderWidget.setLayout(m_flowLayout);
    m_holderWidget.setContentsMargins(0, 0, 0, 0);
}

ThumbnailWidget *FolderGridView::createThumbnailWidget() {
    ThumbnailWidget *widget = new ThumbnailWidget();
    widget->setPadding(8);
    ThumbnailStyle style = (settings->folderViewMode() == FV_SIMPLE) ? THUMB_SIMPLE : THUMB_NORMAL;
    widget->setThumbStyle(style);
    widget->setThumbnailSize(this->m_thumbnailSize); // TODO: constructor
    return widget;
}

void FolderGridView::syncThumbnailLayout() {
    QList<QGraphicsLayoutItem *> items;
    items.reserve(m_thumbnails.size());
    for(auto *thumbnail : std::as_const(m_thumbnails)) {
        thumbnail->setDropHovered(false);
        items.append(thumbnail);
    }
    m_lastDragTarget = -1;
    m_flowLayout->setItems(std::move(items));
}

void FolderGridView::updateLayout() {
    m_shiftedCol = -1;
    m_flowLayout->invalidate();
    m_flowLayout->activate();
}

// block native tab-switching so we can use it in shortcuts
bool FolderGridView::focusNextPrevChild(bool) {
    return false;
}

void FolderGridView::keyPressEvent(QKeyEvent *event) {
    ThumbnailView::keyPressEvent(event); // shift state for range selection
    // the grid's bindings ride its QShortcuts (WidgetWithChildrenShortcut),
    // armed by the focus-driven de-claim — they fire before this handler
    // exists to see them. What still arrives is unclaimed: consumed, so a
    // key nobody bound never wanders off into the widget chain.
    event->accept();
}

void FolderGridView::focusInEvent(QFocusEvent *event) {
    actionManager->setGridActive(true);
    QGraphicsView::focusInEvent(event);
}

void FolderGridView::focusOutEvent(QFocusEvent *event) {
    actionManager->setGridActive(false);
    ThumbnailView::focusOutEvent(event);
}

void FolderGridView::wheelEvent(QWheelEvent *event) {
    if(event->modifiers().testFlag(Qt::ControlModifier)) {
        if(event->pixelDelta().y() > 0 || event->angleDelta().y() > 0)
            zoomIn();
        else if(event->pixelDelta().y() < 0 || event->angleDelta().y() < 0)
            zoomOut();
    } else {
        ThumbnailView::wheelEvent(event);
    }
}

void FolderGridView::zoomIn() {
    setThumbnailSize(this->m_thumbnailSize + ZOOM_STEP);
}

void FolderGridView::zoomOut() {
    setThumbnailSize(this->m_thumbnailSize - ZOOM_STEP);
}

void FolderGridView::setThumbnailSize(int newSize) {
    newSize = clamp(newSize, THUMBNAIL_SIZE_MIN, THUMBNAIL_SIZE_MAX);
    m_thumbnailSize = newSize;
    for(int i = 0; i < m_thumbnails.count(); i++) {
        m_thumbnails.at(i)->setThumbnailSize(newSize);
    }
    updateLayout();
    fitSceneToContents();
    if(lastSelected() != -1)
        ensureVisible(m_thumbnails.at(lastSelected()), 0, 40);
    emit thumbnailSizeChanged(m_thumbnailSize);
    loadVisibleThumbnails();
}

void FolderGridView::fitSceneToContents() {
    if(m_scrollBar->isVisible())
        m_holderWidget.setGeometry(0, 0, width() - m_scrollBar->width(), height());
    else
        m_holderWidget.setGeometry(0, 0, width(), height());
    ThumbnailView::fitSceneToContents();
}

void FolderGridView::resizeEvent(QResizeEvent *event) {
    if(this->isVisible()) {
        ThumbnailView::resizeEvent(event);
        fitSceneToContents();
        //focusOn(selectedIndex());
        loadVisibleThumbnailsDelayed();
    }
}
