#include "thumbnailstrip.h"

// TODO: move item highlight logic to base class

ThumbnailStrip::ThumbnailStrip(QWidget *parent) : ThumbnailView(Qt::Horizontal, parent) {
    this->setAttribute(Qt::WA_NoMousePropagation, true);
    this->setFocusPolicy(Qt::NoFocus);
    setupLayout();
    readSettings();
}

void ThumbnailStrip::updateScrollbarIndicator() {
    if(!m_thumbnails.count() || lastSelected() == -1)
        return;
    qreal itemCenter = static_cast<qreal>(lastSelected() + 0.5) / itemCount();
    if(m_scrollBar->orientation() == Qt::Horizontal)
        m_indicator =
            QRect(m_scrollBar->width() * itemCenter - m_indicatorSize, 2, m_indicatorSize, m_scrollBar->height() - 4);
    else
        m_indicator =
            QRect(2, m_scrollBar->height() * itemCenter - m_indicatorSize, m_scrollBar->width() - 4, m_indicatorSize);
}

//  no layout; manual item positioning
//  graphical issues otherwise
void ThumbnailStrip::setupLayout() {
    this->setAlignment(Qt::AlignLeft | Qt::AlignTop);
}

ThumbnailWidget *ThumbnailStrip::createThumbnailWidget() {
    ThumbnailWidget *widget = new ThumbnailWidget();
    widget->setPadding(m_thumbPadding);
    widget->setMargins(m_thumbMarginX, m_thumbMarginY);
    widget->setThumbStyle(m_currentStyle);
    widget->setThumbnailSize(m_thumbnailSize);
    return widget;
}

void ThumbnailStrip::addItemToLayout(ThumbnailWidget *widget, int pos) {
    m_scene.addItem(widget);
    updateThumbnailPositions(pos, m_thumbnails.count() - 1);
}

void ThumbnailStrip::removeItemFromLayout(int pos) {
    if(checkRange(pos)) {
        ThumbnailWidget *thumb = m_thumbnails.at(pos);
        m_scene.removeItem(thumb);
        // move items
        ThumbnailWidget *tmp;
        if(orientation() == Qt::Horizontal) {
            for(int i = pos; i < m_thumbnails.count(); i++) {
                tmp = m_thumbnails.at(i);
                tmp->moveBy(-tmp->boundingRect().width(), 0);
            }
        } else {
            for(int i = pos; i < m_thumbnails.count(); i++) {
                tmp = m_thumbnails.at(i);
                tmp->moveBy(0, -tmp->boundingRect().height());
            }
        }
    }
}

void ThumbnailStrip::removeAll() {
    m_scene.clear(); // also calls delete on all items
    m_thumbnails.clear();
}

void ThumbnailStrip::updateThumbnailPositions() {
    updateThumbnailPositions(0, m_thumbnails.count() - 1);
}

void ThumbnailStrip::updateThumbnailPositions(int start, int end) {
    if(start > end || !checkRange(start) || !checkRange(end))
        return;
    // assume all m_thumbnails are the same size
    ThumbnailWidget *tmp;
    if(orientation() == Qt::Horizontal) {
        int thumbWidth = static_cast<int>(m_thumbnails.at(start)->boundingRect().width());
        for(int i = start; i <= end; i++) {
            tmp = m_thumbnails.at(i);
            tmp->setPos(i * thumbWidth, 0);
        }
    } else {
        int thumbHeight = static_cast<int>(m_thumbnails.at(start)->boundingRect().height());
        for(int i = start; i <= end; i++) {
            tmp = m_thumbnails.at(i);
            tmp->setPos(0, i * thumbHeight);
        }
    }
}

void ThumbnailStrip::focusOn(int index) {
    if(!checkRange(index))
        return;
    auto th = m_thumbnails.at(index);
    if(settings->panelCenterSelection()) {
        QGraphicsView::centerOn(th->sceneBoundingRect().center());
    } else {
        // partially show the next thumb if possible
        if(orientation() == Qt::Vertical) {
            if(height() > th->height() * 2)
                ensureVisible(th, 0, th->height() / 2);
            else
                ensureVisible(th, 0, 0);
        } else {
            if(width() > th->width() * 2)
                ensureVisible(th, th->width() / 2, 0);
            else
                ensureVisible(th, 0, 0);
        }
    }
    loadVisibleThumbnails();
}

void ThumbnailStrip::focusOnSelection() {
    if(selection().isEmpty())
        return;
    focusOn(selection().constLast());
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

    // apply style, size & reposition
    for(int i = 0; i < m_thumbnails.count(); i++) {
        m_thumbnails.at(i)->setPadding(m_thumbPadding);
        m_thumbnails.at(i)->setMargins(m_thumbMarginX, m_thumbMarginY);
        m_thumbnails.at(i)->setThumbStyle(m_currentStyle);
        m_thumbnails.at(i)->setThumbnailSize(m_thumbnailSize);
    }
    updateThumbnailPositions(0, m_thumbnails.count() - 1);
    fitSceneToContents();
    setCropThumbnails(settings->squareThumbnails());
    focusOn(lastSelected());
}

QSize ThumbnailStrip::itemSize() {
    if(!m_thumbnails.count()) {
        ThumbnailWidget w;
        w.setPadding(m_thumbPadding);
        w.setMargins(m_thumbMarginX, m_thumbMarginY);
        w.setThumbStyle(m_currentStyle);
        w.setThumbnailSize(m_thumbnailSize);
        return w.boundingRect().size().toSize();
    } else {
        return m_thumbnails.at(0)->boundingRect().size().toSize();
    }
}

void ThumbnailStrip::resizeEvent(QResizeEvent *event) {
    ThumbnailView::resizeEvent(event);
    fitSceneToContents();
    if(event->oldSize().width() < width())
        loadVisibleThumbnailsDelayed();
}
