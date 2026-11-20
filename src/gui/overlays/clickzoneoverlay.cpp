#include "clickzoneoverlay.h"

ClickZoneOverlay::ClickZoneOverlay(FloatingWidgetContainer *parent) : FloatingWidget(parent) {
    // this is just for painting, we are handling mouse events elsewhere
    setAttribute(Qt::WA_TransparentForMouseEvents);
    if(parent)
        setContainerSize(parent->size());

    m_dpr = this->devicePixelRatioF();
    m_pixmapLeft = loadPixmap(":/res/icons/common/overlay/arrow_left_50.png");
    m_pixmapRight = loadPixmap(":/res/icons/common/overlay/arrow_right_50.png");

    connect(settings, &Settings::settingsChanged, this, &ClickZoneOverlay::readSettings);
    readSettings();

    this->show();
}

void ClickZoneOverlay::readSettings() {
    if(settings->clickableEdgesVisible() == m_drawZones)
        return;
    m_drawZones = settings->clickableEdgesVisible();
    update();
}

QPixmap ClickZoneOverlay::loadPixmap(QString path) {
    QPixmap pixmap;
    if(m_dpr >= (1.0 + 0.001)) {
        path.replace(".", "@2x.");
        m_hiResPixmaps = true;
        pixmap = QPixmap(path);
        if(m_dpr >= (2.0 - 0.001))
            m_pixmapDrawScale = m_dpr;
        else
            m_pixmapDrawScale = 2.0;
        pixmap.setDevicePixelRatio(m_pixmapDrawScale);
    } else {
        m_hiResPixmaps = false;
        pixmap = QPixmap(path);
        m_pixmapDrawScale = m_dpr;
    }
    ImageLib::recolor(pixmap, QColor(255, 255, 255));
    if(pixmap.isNull())
        return QPixmap();
    return pixmap;
}

QRect ClickZoneOverlay::leftZone() {
    return m_leftZone;
}

QRect ClickZoneOverlay::rightZone() {
    return m_rightZone;
}

void ClickZoneOverlay::highlightLeft() {
    setHighlightedZone(HIGHLIGHT_LEFT);
}

void ClickZoneOverlay::highlightRight() {
    setHighlightedZone(HIGHLIGHT_RIGHT);
}

void ClickZoneOverlay::disableHighlight() {
    setHighlightedZone(HIGHLIGHT_NONE);
}

bool ClickZoneOverlay::isHighlighted() {
    return (m_activeZone != HIGHLIGHT_NONE);
}

void ClickZoneOverlay::setPressed(bool mode) {
    if(m_isPressed == mode)
        return;
    m_isPressed = mode;
    if(isHighlighted())
        update();
}

void ClickZoneOverlay::setHighlightedZone(ActiveHighlightZone zone) {
    m_activeZone = zone;
    update();
}

void ClickZoneOverlay::recalculateGeometry() {
    setGeometry(0, 0, containerSize().width(), containerSize().height());
}

void ClickZoneOverlay::resizeEvent(QResizeEvent *event) {
    Q_UNUSED(event)
    m_leftZone = QRect(0, 0, m_zoneSize, height());
    m_rightZone = QRect(width() - m_zoneSize, 0, m_zoneSize, height());
}

void ClickZoneOverlay::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    if(m_activeZone == HIGHLIGHT_NONE || !m_drawZones || width() <= 250)
        return;
    QPainter p(this);
    if(m_isPressed)
        p.setOpacity(0.06f);
    else
        p.setOpacity(0.10f);
    QBrush brush;
    brush.setColor(QColor(200, 200, 200));
    brush.setStyle(Qt::SolidPattern);

    if(m_activeZone == HIGHLIGHT_LEFT) {
        p.fillRect(m_leftZone, brush);
        drawPixmap(p, m_pixmapLeft, m_leftZone);
    }
    if(m_activeZone == HIGHLIGHT_RIGHT) {
        p.fillRect(m_rightZone, brush);
        drawPixmap(p, m_pixmapRight, m_rightZone);
    }
}

// draws pixmap centered inside rect
void ClickZoneOverlay::drawPixmap(QPainter &p, const QPixmap &pixmap, QRect rect) {
    if(m_isPressed)
        p.setOpacity(0.37f);
    else
        p.setOpacity(0.5f);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPointF pos;
    if(m_hiResPixmaps) {
        pos = QPointF(rect.left() + rect.width() / 2 - pixmap.width() / (2 * m_pixmapDrawScale),
                      rect.top() + rect.height() / 2 - pixmap.height() / (2 * m_pixmapDrawScale));
    } else {
        pos = QPointF(rect.left() + rect.width() / 2 - pixmap.width() / 2,
                      rect.top() + rect.height() / 2 - pixmap.height() / 2);
    }
    p.drawPixmap(pos, pixmap);
}
