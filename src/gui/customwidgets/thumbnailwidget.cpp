#include "thumbnailwidget.h"

ThumbnailWidget::ThumbnailWidget(QGraphicsItem *parent)
    : QGraphicsWidget(parent), isLoaded(false), m_thumbnail(nullptr), m_highlighted(false), m_hovered(false),
      m_dropHovered(false), m_thumbnailSize(100), m_padding(5), m_marginX(2), m_marginY(2), m_labelSpacing(9),
      m_textHeight(5), m_thumbStyle(THUMB_SIMPLE) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    m_dpr = qApp->devicePixelRatio();
    if(trunc(m_dpr) == m_dpr) // don't enable for fractional scaling
        setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    setAcceptHoverEvents(true);
    m_font.setBold(false);
    QFontMetrics fm(m_font);
    m_textHeight = fm.height();
}

void ThumbnailWidget::updateDpr(qreal newDpr) {
    if(m_dpr != newDpr) {
        m_dpr = newDpr;
        refreshCacheMode();
        updateThumbnailDrawPosition();
        updateBackgroundRect();
    }
}

/// Re-applies the device coordinate cache once the current paint pass is over.
/// setCacheMode() purges the item cache, and calling it from inside paint()
/// frees the device cache entry the scene is drawing into, which crashes.
void ThumbnailWidget::refreshCacheMode() {
    if(trunc(m_dpr) != m_dpr) // don't enable for fractional scaling
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            if(trunc(m_dpr) != m_dpr)
                return;
            setCacheMode(QGraphicsItem::DeviceCoordinateCache);
            // re-setting the mode only purges the cache, it does not repaint
            update();
        },
        Qt::QueuedConnection);
}

void ThumbnailWidget::setThumbnailSize(int size) {
    if(m_thumbnailSize != size && size > 0) {
        isLoaded = false;
        m_thumbnailSize = size;
        updateBoundingRect();
        updateGeometry();
        updateThumbnailDrawPosition();
        updateBackgroundRect();
        setupTextLayout();
        update();
    }
}

void ThumbnailWidget::setPadding(int _padding) {
    m_padding = _padding;
    updateBoundingRect();
}

void ThumbnailWidget::setMargins(int _marginX, int _marginY) {
    m_marginX = _marginX;
    m_marginY = _marginY;
    updateBoundingRect();
}

int ThumbnailWidget::thumbnailSize() {
    return m_thumbnailSize;
}

void ThumbnailWidget::setThumbStyle(ThumbnailStyle _style) {
    if(m_thumbStyle != _style) {
        m_thumbStyle = _style;
        updateBoundingRect();
        updateThumbnailDrawPosition();
        setupTextLayout();
        updateBackgroundRect();
        updateGeometry();
        update();
    }
}

void ThumbnailWidget::updateGeometry() {
    QGraphicsWidget::updateGeometry();
}

void ThumbnailWidget::setGeometry(const QRectF &rect) {
    QGraphicsWidget::setGeometry(QRectF(rect.topLeft(), boundingRect().size()));
}

QRectF ThumbnailWidget::geometry() const {
    return QRectF(QGraphicsWidget::geometry().topLeft(), boundingRect().size());
}

QSizeF ThumbnailWidget::effectiveSizeHint(Qt::SizeHint which, const QSizeF &constraint) const {
    return sizeHint(which, constraint);
}

void ThumbnailWidget::setThumbnail(std::shared_ptr<Thumbnail> _thumbnail) {
    m_needsFreshThumbnail = false;
    if(_thumbnail) {
        m_thumbnail = _thumbnail;
        if(m_thumbnail && m_thumbnail->image())
            m_displayPixmap = QPixmap::fromImage(*m_thumbnail->image());
        else
            m_displayPixmap = QPixmap();
        isLoaded = true;
        updateThumbnailDrawPosition();
        setupTextLayout();
        updateBackgroundRect();
        update();
    }
}

void ThumbnailWidget::invalidateThumbnail() {
    unsetThumbnail();
    m_needsFreshThumbnail = true;
    update();
}

void ThumbnailWidget::unsetThumbnail() {
    if(m_thumbnail)
        m_thumbnail.reset();
    isLoaded = false;
}

// Thumbnail is immutable — a worker can still hold the one this widget was
// given — so a rename builds a second description of the same pixels rather
// than mutating the first. The QPixmap is refcounted, so nothing is decoded.
void ThumbnailWidget::renameThumbnail(const QString &newName) {
    if(!m_thumbnail)
        return;
    setThumbnail(std::make_shared<Thumbnail>(newName, m_thumbnail->info(), m_thumbnail->size(), m_thumbnail->image()));
}

void ThumbnailWidget::setupTextLayout() {
    if(m_thumbStyle != THUMB_SIMPLE) {
        m_nameRect = QRect(m_padding + m_marginX, m_padding + m_marginY + m_thumbnailSize + m_labelSpacing,
                           m_thumbnailSize, m_textHeight);
        m_infoRect = m_nameRect.adjusted(0, m_textHeight + 2, 0, m_textHeight + 2);
    }
}

void ThumbnailWidget::updateBackgroundRect() {
    bool verticalFit = (m_drawRectCentered.height() >= m_drawRectCentered.width());
    if(m_thumbStyle == THUMB_NORMAL && !verticalFit) {
        m_bgRect.setBottom(height() - m_marginY);
        m_bgRect.setLeft(m_marginX);
        m_bgRect.setRight(width() - m_marginX);
        if(!m_thumbnail || !m_displayPixmap)
            m_bgRect.setTop(m_drawRectCentered.top() - m_padding);
        else // ensure we get equal padding on the top & sides
            m_bgRect.setTop(qMax(m_drawRectCentered.top() - m_drawRectCentered.left() + m_marginX, m_marginY));
    } else {
        m_bgRect = boundingRect().adjusted(m_marginX, m_marginY, -m_marginX, -m_marginY);
    }
}

void ThumbnailWidget::setHighlighted(bool mode) {
    if(m_highlighted != mode) {
        m_highlighted = mode;
        update();
    }
}

bool ThumbnailWidget::isHighlighted() {
    return m_highlighted;
}

void ThumbnailWidget::setDropHovered(bool mode) {
    if(m_dropHovered != mode) {
        m_dropHovered = mode;
        update();
    }
}

bool ThumbnailWidget::isDropHovered() {
    return m_dropHovered;
}

QRectF ThumbnailWidget::boundingRect() const {
    return m_boundingRect;
}

void ThumbnailWidget::updateBoundingRect() {
    m_boundingRect =
        QRectF(0, 0, m_thumbnailSize + (m_padding + m_marginX) * 2, m_thumbnailSize + (m_padding + m_marginY) * 2);
    if(m_thumbStyle != THUMB_SIMPLE)
        m_boundingRect.adjust(0, 0, 0, m_labelSpacing + m_textHeight * 2);
}

qreal ThumbnailWidget::width() {
    return boundingRect().width();
}

qreal ThumbnailWidget::height() {
    return boundingRect().height();
}

void ThumbnailWidget::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    Q_UNUSED(widget)
    Q_UNUSED(option)
    painter->setRenderHints(QPainter::Antialiasing);

    // update dpr since on wayland we only get correct values on the first paint event
    // actual thumbnail still has incorrect dpr but it is still drawn "correctly" so whatever
    updateDpr(painter->paintEngine()->paintDevice()->devicePixelRatioF());

    if(isHovered() && !isHighlighted())
        drawHoverBg(painter);
    if(isHighlighted())
        drawHighlight(painter);

    if(!m_thumbnail) { // not loaded
        // todo: recolor once in shrRes
        QPixmap loadingIcon(*shrRes->getPixmap(ShrIcon::SHR_ICON_LOADING, m_dpr));
        if(isHighlighted())
            ImageLib::recolor(loadingIcon, settings->colorScheme().accent);
        else
            ImageLib::recolor(loadingIcon, settings->colorScheme().folderview_hc2);
        drawIcon(painter, &loadingIcon);
    } else {
        if(m_displayPixmap.isNull() || m_displayPixmap.width() == 0) { // invalid thumb
            QPixmap errorIcon(*shrRes->getPixmap(ShrIcon::SHR_ICON_ERROR, m_dpr));
            if(isHighlighted())
                ImageLib::recolor(errorIcon, settings->colorScheme().accent);
            else
                ImageLib::recolor(errorIcon, settings->colorScheme().folderview_hc2);
            drawIcon(painter, &errorIcon);
        } else {
            drawThumbnail(painter, &m_displayPixmap);
            if(isHovered())
                drawHoverHighlight(painter);
        }
        if(m_thumbStyle != THUMB_SIMPLE)
            drawLabel(painter);
    }
    if(isDropHovered())
        drawDropHover(painter);
}

void ThumbnailWidget::drawHighlight(QPainter *painter) {
    if(isHighlighted()) {
        auto hints = painter->renderHints();
        auto op = painter->opacity();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setOpacity(0.40f * op);
        painter->fillRect(m_bgRect, settings->colorScheme().accent);
        painter->setOpacity(0.70f * op);
        QPen pen(settings->colorScheme().accent, 2);
        painter->setPen(pen);
        painter->drawRect(m_bgRect.adjusted(1, 1, -1, -1)); // 2px pen
        //painter->drawRect(highlightRect.adjusted(0.5,0.5,-0.5,-0.5)); // 1px pen
        painter->setOpacity(op);
        painter->setRenderHints(hints);
    }
}

void ThumbnailWidget::drawHoverBg(QPainter *painter) {
    auto op = painter->opacity();
    painter->fillRect(m_bgRect, settings->colorScheme().folderview_hc);
    painter->setOpacity(op);
}

void ThumbnailWidget::drawHoverHighlight(QPainter *painter) {
    auto op = painter->opacity();
    auto mode = painter->compositionMode();
    painter->setCompositionMode(QPainter::CompositionMode_Plus);
    painter->setOpacity(0.2f);
    painter->drawPixmap(m_drawRectCentered, m_displayPixmap);
    painter->setOpacity(op);
    painter->setCompositionMode(mode);
}

void ThumbnailWidget::drawLabel(QPainter *painter) {
    if(m_thumbnail) {
        drawSingleLineText(painter, m_nameRect, m_thumbnail->name(), settings->colorScheme().text_hc2);
        auto op = painter->opacity();
        painter->setOpacity(op * 0.62f);
        drawSingleLineText(painter, m_infoRect, m_thumbnail->info(), settings->colorScheme().text_hc2);
        painter->setOpacity(op);
    }
}

void ThumbnailWidget::drawSingleLineText(QPainter *painter, QRect rect, QString text, const QColor &color) {
    QFontMetrics fm(m_font);
    bool fits = !(fm.horizontalAdvance(text) > rect.width());
    // filename
    Qt::TextInteractionFlags flags;
    painter->setFont(m_font);
    if(fits) {
        flags = Qt::TextInteractionFlags(Qt::TextSingleLine) | Qt::TextInteractionFlags(Qt::AlignVCenter) |
                Qt::TextInteractionFlags(Qt::AlignHCenter);
        painter->setPen(color);
        painter->drawText(rect, flags, text);
    } else {
        // fancy variant with text fade effect - uses temporary surface to paint; slow
        QPixmap textLayer(rect.width() * m_dpr, rect.height() * m_dpr);
        textLayer.fill(Qt::transparent);
        textLayer.setDevicePixelRatio(m_dpr);
        QPainter textPainter(&textLayer);
        textPainter.setFont(m_font);
        // paint text onto tmp layer
        flags = Qt::TextInteractionFlags(Qt::TextSingleLine) | Qt::TextInteractionFlags(Qt::AlignVCenter);
        textPainter.setPen(color);
        QRect textRect = QRect(0, 0, rect.width(), rect.height());
        textPainter.drawText(textRect, flags, text);
        QRectF fadeRect = textRect.adjusted(textRect.width() - 6, 0, 0, 0);
        // fade effect
        QLinearGradient gradient(fadeRect.topLeft(), fadeRect.topRight());
        gradient.setColorAt(0, Qt::transparent);
        gradient.setColorAt(1, Qt::red); // any color, this is just a transparency mask
        textPainter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        textPainter.fillRect(fadeRect, gradient);
        // write text layer into graphicsitem
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter->drawPixmap(rect.topLeft(), textLayer);
    }
}

void ThumbnailWidget::drawDropHover(QPainter *painter) {
    // save
    auto hints = painter->renderHints();
    auto op = painter->opacity();

    painter->setRenderHint(QPainter::Antialiasing);
    QColor clr(190, 60, 25);
    painter->setOpacity(0.1f * op);
    painter->fillRect(m_bgRect, clr);
    painter->setOpacity(op);
    QPen pen(clr, 2);
    painter->setPen(pen);
    painter->drawRect(m_bgRect.adjusted(1, 1, -1, -1));
    painter->setRenderHints(hints);
}

void ThumbnailWidget::drawThumbnail(QPainter *painter, const QPixmap *pixmap) {
    if(!m_thumbnail->hasAlphaChannel())
        painter->fillRect(m_drawRectCentered.adjusted(3, 3, 3, 3), QColor(0, 0, 0, 60));
    painter->drawPixmap(m_drawRectCentered, *pixmap);
}

void ThumbnailWidget::drawIcon(QPainter *painter, const QPixmap *pixmap) {
    QPointF drawPosCentered(width() / 2 - pixmap->width() / (2 * pixmap->devicePixelRatioF()),
                            height() / 2 - pixmap->height() / (2 * pixmap->devicePixelRatioF()));
    painter->drawPixmap(drawPosCentered, *pixmap, QRectF(QPoint(0, 0), pixmap->size()));
}

QSizeF ThumbnailWidget::sizeHint(Qt::SizeHint which, const QSizeF &constraint) const {
    Q_UNUSED(which);
    Q_UNUSED(constraint);

    return boundingRect().size();
}

void ThumbnailWidget::hoverEnterEvent(QGraphicsSceneHoverEvent *event) {
    event->ignore();
    setHovered(true);
}

void ThumbnailWidget::hoverLeaveEvent(QGraphicsSceneHoverEvent *event) {
    event->ignore();
    setHovered(false);
}

void ThumbnailWidget::setHovered(bool mode) {
    if(m_hovered != mode) {
        m_hovered = mode;
        update();
    }
}

bool ThumbnailWidget::isHovered() {
    return m_hovered;
}

void ThumbnailWidget::updateThumbnailDrawPosition() {
    if(m_thumbnail && !m_displayPixmap.isNull()) {
        QPoint topLeft;
        QSize pixmapSize; // dpr-adjusted size
        if(isLoaded)
            pixmapSize = m_thumbnail->image()->size() / qApp->devicePixelRatio();
        else
            pixmapSize = m_thumbnail->image()->size().scaled(m_thumbnailSize, m_thumbnailSize, Qt::KeepAspectRatio);
        bool verticalFit = (pixmapSize.height() >= pixmapSize.width());
        topLeft.setX((width() - pixmapSize.width()) / 2.0);
        if(m_thumbStyle == THUMB_SIMPLE)
            topLeft.setY((height() - pixmapSize.height()) / 2.0);
        else if(m_thumbStyle == THUMB_NORMAL_CENTERED && !verticalFit)
            topLeft.setY((height() - pixmapSize.height()) / 2.0 - m_textHeight);
        else // THUMB_NORMAL - snap thumbnail to the filename label
            topLeft.setY(m_padding + m_marginY + m_thumbnailSize - pixmapSize.height());
        m_drawRectCentered = QRect(topLeft, pixmapSize);
    }
}
