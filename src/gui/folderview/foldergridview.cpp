#include "foldergridview.h"

#include <QMimeData>

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

    connect(this, &ThumbnailView::itemActivated, this, &FolderGridView::onitemSelected);
}

FolderGridView::~FolderGridView() {
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
        delete itw.value();
    m_visible.clear();
}

void FolderGridView::dropEvent(QDropEvent *event) {
    event->accept();
    auto *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->position().toPoint()));
    int index = -1;
    if(item) {
        index = item->rowIndex();
        item->setDropHovered(false);
    }
    emit droppedInto(event->mimeData(), event->source(), index);
}

void FolderGridView::dragEnterEvent(QDragEnterEvent *event) {
    event->accept();
}

void FolderGridView::dragMoveEvent(QDragMoveEvent *event) {
    event->accept();
    auto *item = dynamic_cast<ThumbnailWidget *>(itemAt(event->position().toPoint()));
    setDragHover(item ? item->rowIndex() : -1);
    emit draggedOver(m_dragHoverIndex);
}

void FolderGridView::dragLeaveEvent(QDragLeaveEvent *event) {
    event->accept();
    setDragHover(-1);
}

void FolderGridView::onitemSelected() {
    m_shiftedCol = -1;
}

// the scrollbar-indicator position is index arithmetic: the selected row's
// cell rect exists whether or not a widget is materialized for it
void FolderGridView::updateScrollbarIndicator() {
    if(!m_rowCount || !selection().count())
        return;
    qreal itemCenter = cellSceneRect(lastSelected()).center().y() / m_scene.height();
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
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
        itw.value()->setThumbStyle(style);
    refreshCellSize();
    updateSceneExtent();
    focusOnSelection();
}

// the selected row's rect is arithmetic — ensureVisible takes a rect, no
// widget needed
void FolderGridView::focusOnSelection() {
    if(!m_rowCount || lastSelected() == -1)
        return;
    ensureVisible(cellSceneRect(lastSelected()), 0, 0);
    loadVisibleThumbnailsDelayed();
}

void FolderGridView::selectAll() {
    QList<int> list;
    for(int i = 0; i < m_rowCount; i++)
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
    if(!m_rowCount || lastSelected() == -1 || sameRow(0, lastSelected()))
        return;
    int newIndex = itemAbove(lastSelected());
    if(m_shiftedCol >= 0) {
        int diff = m_shiftedCol - columnOf(lastSelected());
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
    if(!m_rowCount || lastSelected() == -1 || sameRow(lastSelected(), m_rowCount - 1))
        return;
    m_shiftedCol = -1;
    int newIndex = itemBelow(lastSelected());
    if(!checkRange(newIndex))
        newIndex = m_rowCount - 1;
    if(columnOf(newIndex) != columnOf(lastSelected()))
        m_shiftedCol = columnOf(lastSelected());
    if(m_rangeSelection)
        addSelectionRange(newIndex);
    else
        select(newIndex);
    scrollToCurrent();
}

void FolderGridView::selectNext() {
    if(!m_rowCount || lastSelected() == m_rowCount - 1)
        return;
    if(!m_rangeSelection && lastSelected() == m_rowCount - 1) {
        select(lastSelected());
        return;
    }
    m_shiftedCol = -1;
    int newIndex = lastSelected() + 1;
    if(!checkRange(newIndex))
        newIndex = m_rowCount - 1;
    if(m_rangeSelection)
        addSelectionRange(newIndex);
    else
        select(newIndex);
    scrollToCurrent();
}

void FolderGridView::selectPrev() {
    if(!m_rowCount || lastSelected() == 0)
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
    if(!m_rowCount || lastSelected() == -1 || sameRow(0, lastSelected()))
        return;
    int newIndex = lastSelected();
    int tmp;
    // 4 rows up
    for(int i = 0; i < 4; i++) {
        tmp = itemAbove(newIndex);
        if(checkRange(tmp))
            newIndex = tmp;
    }
    if(m_shiftedCol >= 0) {
        int diff = m_shiftedCol - columnOf(newIndex);
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
    if(!m_rowCount || lastSelected() == -1 || sameRow(lastSelected(), m_rowCount - 1))
        return;
    m_shiftedCol = -1;
    int newIndex = lastSelected();
    int tmp;
    // 4 rows down
    for(int i = 0; i < 4; i++) {
        tmp = itemBelow(newIndex);
        if(checkRange(tmp))
            newIndex = tmp;
    }
    if(columnOf(newIndex) != columnOf(lastSelected()))
        m_shiftedCol = columnOf(lastSelected());
    if(m_rangeSelection)
        addSelectionRange(newIndex);
    else
        select(newIndex);
    scrollToCurrent();
}

void FolderGridView::selectFirst() {
    if(!m_rowCount)
        return;
    m_shiftedCol = -1;
    if(m_rangeSelection)
        addSelectionRange(0);
    else
        select(0);
    scrollToCurrent();
}

void FolderGridView::selectLast() {
    if(!m_rowCount)
        return;
    m_shiftedCol = -1;
    if(m_rangeSelection)
        addSelectionRange(m_rowCount - 1);
    else
        select(m_rowCount - 1);
    scrollToCurrent();
}

void FolderGridView::scrollToCurrent() {
    scrollToItem(lastSelected());
}

// same as scrollToItem minus the animation
void FolderGridView::focusOn(int index) {
    if(!checkRange(index))
        return;
    ensureVisible(cellSceneRect(index), 0, 0);
    loadVisibleThumbnailsDelayed();
}

// The arithmetic layout — the exact math FlowLayout::doLayout ran (spacing
// zero, contents margins 9/6/9/0, wrap at perRow, per-row centering), minus
// the widgets: uniform cells make every position computable from the index,
// which is what lets rows without widgets still have scene geometry.
void FolderGridView::refreshCellSize() const {
    ThumbnailWidget prototype;
    configureWidget(&prototype);
    m_cellSize = prototype.boundingRect().size();
}

qreal FolderGridView::holderWidth() const {
    // the scrollbar dance the old fitSceneToContents ran: the holder's width
    // depends on whether the scrollbar is visible, whose state depends on the
    // scene extent, which depends on the holder width — resolved one state
    // behind, exactly as before
    return m_scrollBar->isVisible() ? width() - m_scrollBar->width() : width();
}

int FolderGridView::perRow() const {
    if(m_cellSize.isEmpty())
        refreshCellSize();
    const qreal maxRowWidth = holderWidth() - 9 - 9; // the layout's contents margins
    return qMax(1, static_cast<int>(maxRowWidth / m_cellSize.width()));
}

int FolderGridView::gridRows() const {
    const int cols = perRow();
    return (m_rowCount + cols - 1) / cols;
}

int FolderGridView::itemAbove(int index) const {
    const int cols = perRow();
    const int indexAbove = index - cols;
    return indexAbove >= 0 ? indexAbove : index;
}

int FolderGridView::itemBelow(int index) const {
    const int cols = perRow();
    if(sameRow(index, m_rowCount - 1))
        return index;
    const int indexBelow = index + cols;
    return indexBelow < m_rowCount ? indexBelow : m_rowCount - 1;
}

int FolderGridView::columnOf(int index) const {
    return index % perRow();
}

bool FolderGridView::sameRow(int one, int two) const {
    return (one / perRow()) == (two / perRow());
}

QRectF FolderGridView::cellSceneRect(int index) const {
    if(m_cellSize.isEmpty())
        refreshCellSize();
    const qreal maxRowWidth = holderWidth() - 18;
    const int cols = perRow();
    const int row = index / cols;
    const int col = index % cols;
    // centering: applied only once the listing fills at least one row, the
    // offset being the leftover width split in two
    qreal centerOffset = 0;
    if(m_rowCount >= cols)
        centerOffset = static_cast<int>(std::fmod(maxRowWidth, m_cellSize.width()) / 2);
    return QRectF(QPointF(9 + col * m_cellSize.width() + centerOffset, 6 + row * m_cellSize.height()), m_cellSize);
}

int FolderGridView::materializeFirst() const {
    if(m_cellSize.isEmpty())
        refreshCellSize();
    // the viewport's top edge, one band back, snapped to the row start
    const qreal bandTop = mapToScene(viewport()->geometry()).boundingRect().top() - m_offscreenPreloadArea;
    const int row = static_cast<int>(bandTop / m_cellSize.height()) - 1;
    return qMax(0, row * perRow());
}

int FolderGridView::materializeLast() const {
    if(m_cellSize.isEmpty())
        refreshCellSize();
    const qreal bandBottom = mapToScene(viewport()->geometry()).boundingRect().bottom() + m_offscreenPreloadArea;
    const int row = static_cast<int>(bandBottom / m_cellSize.height()) + 1;
    return qMin(m_rowCount - 1, (row + 1) * perRow() - 1);
}

void FolderGridView::placeWidget(ThumbnailWidget *widget, int index) {
    widget->setPos(cellSceneRect(index).topLeft());
}

void FolderGridView::updateSceneExtent() {
    refreshCellSize();
    const int rows = gridRows();
    const qreal holderW = holderWidth();
    const qreal height = qMax(6.0 + rows * m_cellSize.height(), static_cast<qreal>(this->height()));
    m_scene.setSceneRect(QRectF(0, 0, qMax(holderW, static_cast<qreal>(this->width())), height));
    // keep the viewport anchored the way the old fitSceneToContents did
    const QPointF center = mapToScene(viewport()->rect().center());
    QGraphicsView::centerOn(0, center.y() + 1);
}

void FolderGridView::syncThumbnailLayout() {
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
        placeWidget(itw.value(), itw.key());
    m_dragHoverIndex = -1;
}

void FolderGridView::configureWidget(ThumbnailWidget *widget) const {
    widget->setPadding(8);
    ThumbnailStyle style = (settings->folderViewMode() == FV_SIMPLE) ? THUMB_SIMPLE : THUMB_NORMAL;
    widget->setThumbStyle(style);
    widget->setThumbnailSize(m_thumbnailSize);
}

void FolderGridView::setThumbnailSize(int newSize) {
    newSize = clamp(newSize, THUMBNAIL_SIZE_MIN, THUMBNAIL_SIZE_MAX);
    m_thumbnailSize = newSize;
    // widgets carry their own cached geometry; a size change reconfigures the
    // window wholesale — destroy and re-materialize is cheaper than a sweep
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
        itw.value()->setThumbnailSize(newSize);
    refreshCellSize();
    updateSceneExtent();
    if(lastSelected() != -1)
        ensureVisible(cellSceneRect(lastSelected()), 0, 40);
    emit thumbnailSizeChanged(m_thumbnailSize);
    loadVisibleThumbnails();
}

void FolderGridView::resizeEvent(QResizeEvent *event) {
    if(this->isVisible()) {
        ThumbnailView::resizeEvent(event);
        updateSceneExtent();
        loadVisibleThumbnailsDelayed();
    }
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
