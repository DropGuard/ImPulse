#include "thumbnailstrip.h"

ThumbnailStrip::ThumbnailStrip(QWidget *parent) : ThumbnailView(Qt::Horizontal, parent) {
    this->setAttribute(Qt::WA_NoMousePropagation, true);
    this->setFocusPolicy(Qt::NoFocus);
    readSettings();
}

// index arithmetic, the old m_thumbnails.at(start)->boundingRect() read made
// config-free: the cell size is uniform and computable from the prototype
void ThumbnailStrip::updateScrollbarIndicator() {
    if(!m_rowCount || lastSelected() == -1)
        return;
    qreal itemCenter = static_cast<qreal>(lastSelected() + 0.5) / itemCount();
    if(m_scrollBar->orientation() == Qt::Horizontal)
        m_indicator =
            QRect(m_scrollBar->width() * itemCenter - m_indicatorSize, 2, m_indicatorSize, m_scrollBar->height() - 4);
    else
        m_indicator =
            QRect(2, m_scrollBar->height() * itemCenter - m_indicatorSize, m_scrollBar->width() - 4, m_indicatorSize);
}

ThumbnailWidget *ThumbnailStrip::createThumbnailWidget() {
    auto *widget = new ThumbnailWidget();
    configureWidget(widget);
    return widget;
}

void ThumbnailStrip::configureWidget(ThumbnailWidget *widget) const {
    widget->setPadding(m_thumbPadding);
    widget->setMargins(m_thumbMarginX, m_thumbMarginY);
    widget->setThumbStyle(m_currentStyle);
    widget->setThumbnailSize(m_thumbnailSize);
}

void ThumbnailStrip::syncThumbnailLayout() {
    for(auto itw = m_visible.constBegin(); itw != m_visible.constEnd(); ++itw)
        placeWidget(itw.value(), itw.key());
}

QRectF ThumbnailStrip::cellSceneRect(int index) const {
    const QSizeF cell = [this] {
        ThumbnailWidget prototype;
        configureWidget(&prototype);
        return prototype.boundingRect().size();
    }();
    if(orientation() == Qt::Horizontal)
        return QRectF(QPointF(index * cell.width(), 0), cell);
    return QRectF(QPointF(0, index * cell.height()), cell);
}

int ThumbnailStrip::materializeFirst() const {
    const qreal bandLeft = mapToScene(viewport()->geometry()).boundingRect().left() - m_offscreenPreloadArea;
    const QSizeF cell = cellSceneRect(0).size();
    const qreal step = orientation() == Qt::Horizontal ? cell.width() : cell.height();
    if(step <= 0)
        return 0;
    return qMax(0, static_cast<int>(bandLeft / step) - 1);
}

int ThumbnailStrip::materializeLast() const {
    const qreal bandRight = mapToScene(viewport()->geometry()).boundingRect().right() + m_offscreenPreloadArea;
    const QSizeF cell = cellSceneRect(0).size();
    const qreal step = orientation() == Qt::Horizontal ? cell.width() : cell.height();
    if(step <= 0)
        return m_rowCount - 1;
    return qMin(m_rowCount - 1, static_cast<int>(bandRight / step) + 1);
}

void ThumbnailStrip::placeWidget(ThumbnailWidget *widget, int index) {
    widget->setPos(cellSceneRect(index).topLeft());
}

// the scene extent is the whole listing's arithmetic span, not the
// materialized widgets'
void ThumbnailStrip::updateSceneExtent() {
    const QSizeF cell = cellSceneRect(0).size();
    if(orientation() == Qt::Horizontal)
        m_scene.setSceneRect(
            QRectF(0, 0, m_rowCount * cell.width(), qMax(cell.height(), static_cast<qreal>(height()))));
    else
        m_scene.setSceneRect(QRectF(0, 0, qMax(cell.width(), static_cast<qreal>(width())), m_rowCount * cell.height()));
}

void ThumbnailStrip::focusOn(int index) {
    if(!checkRange(index))
        return;
    const QRectF rect = cellSceneRect(index);
    if(settings->panelCenterSelection()) {
        QGraphicsView::centerOn(rect.center());
    } else {
        // partially show the next thumb if possible
        if(orientation() == Qt::Vertical) {
            if(height() > rect.height() * 2)
                ensureVisible(rect, 0, static_cast<int>(rect.height() / 2));
            else
                ensureVisible(rect, 0, 0);
        } else {
            if(width() > rect.width() * 2)
                ensureVisible(rect, static_cast<int>(rect.width() / 2), 0);
            else
                ensureVisible(rect, 0, 0);
        }
    }
    loadVisibleThumbnails();
}

void ThumbnailStrip::focusOnSelection() {
    if(selection().isEmpty())
        return;
    focusOn(selection().constLast());
}

// a config change reconfigures every materialized widget wholesale: destroy
// the window and let it re-materialize with the new padding/margins/style
void ThumbnailStrip::rebuildWindow() {
    m_visible.clear();
    updateSceneExtent();
    loadVisibleThumbnails();
}

void ThumbnailStrip::readSettings() {
    if(settings->thumbPanelStyle() == TH_PANEL_SIMPLE)
        m_currentStyle = THUMB_SIMPLE;
    else
        m_currentStyle = THUMB_NORMAL_CENTERED;

    m_thumbnailSize = qBound(20, settings->panelPreviewsSize(), 300);

    if(settings->panelPosition() == PANEL_TOP || settings->panelPosition() == PANEL_BOTTOM) {
        ThumbnailView::setOrientation(Qt::Horizontal);
        m_thumbMarginX = 2;
        m_thumbMarginY = 4;
    } else {
        ThumbnailView::setOrientation(Qt::Vertical);
        m_thumbMarginX = 12;
        m_thumbMarginY = 2;
    }

    updateSceneExtent();
    rebuildWindow();
    setCropThumbnails(settings->squareThumbnails());
    focusOn(lastSelected());
}

QSize ThumbnailStrip::itemSize() {
    ThumbnailWidget prototype;
    configureWidget(&prototype);
    return prototype.boundingRect().size().toSize();
}

void ThumbnailStrip::resizeEvent(QResizeEvent *event) {
    ThumbnailView::resizeEvent(event);
    updateSceneExtent();
    if(event->oldSize().width() < width())
        loadVisibleThumbnailsDelayed();
}
