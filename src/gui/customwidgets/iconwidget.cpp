#include "iconwidget.h"

IconWidget::IconWidget(QWidget *parent) : QWidget(parent), m_hiResPixmap(false), m_pixmap(nullptr) {
    m_dpr = this->devicePixelRatioF();
    m_color = settings->colorScheme().icons;
    connect(settings, &Settings::settingsChanged, this, &IconWidget::onSettingsChanged);
}

IconWidget::~IconWidget() {
    if(m_pixmap)
        delete m_pixmap;
}

void IconWidget::onSettingsChanged() {
    if(m_colorMode == ICON_COLOR_THEME && m_color != settings->colorScheme().icons) {
        m_color = settings->colorScheme().icons;
        applyColor();
    }
}

void IconWidget::setIconPath(const QString &path) {
    if(m_iconPath == path)
        return;
    m_iconPath = path;
    loadIcon();
}

void IconWidget::loadIcon() {
    auto path = m_iconPath;
    if(m_pixmap)
        delete m_pixmap;
    if(m_dpr >= (1.0 + 0.001)) {
        path.replace(".", "@2x.");
        m_hiResPixmap = true;
        m_pixmap = new QPixmap(path);
        if(m_dpr >= (2.0 - 0.001))
            m_pixmapDrawScale = m_dpr;
        else
            m_pixmapDrawScale = 2.0;
        m_pixmap->setDevicePixelRatio(m_pixmapDrawScale);
    } else {
        m_hiResPixmap = false;
        m_pixmap = new QPixmap(path);
        m_pixmapDrawScale = m_dpr;
    }
    applyColor();
    if(m_pixmap->isNull()) {
        delete m_pixmap;
        m_pixmap = nullptr;
    }
    update();
}

QSize IconWidget::minimumSizeHint() const {
    if(m_pixmap && !m_pixmap->isNull())
        return m_pixmap->size() / m_dpr;
    else
        return QWidget::minimumSizeHint();
}

void IconWidget::setIconOffset(int x, int y) {
    m_iconOffset.setX(x);
    m_iconOffset.setY(y);
    update();
}

void IconWidget::setColorMode(IconColorMode _mode) {
    if(m_colorMode != _mode && _mode == ICON_COLOR_SOURCE) {
        m_colorMode = _mode;
        // reload uncolored
        loadIcon();
    } else {
        m_colorMode = _mode;
        applyColor();
    }
}

void IconWidget::setColor(QColor _color) {
    m_colorMode = ICON_COLOR_CUSTOM;
    m_color = _color;
    applyColor();
}

void IconWidget::applyColor() {
    if(!m_pixmap || m_pixmap->isNull() || m_colorMode == ICON_COLOR_SOURCE)
        return;
    ImageLib::recolor(*m_pixmap, m_color);
}

void IconWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QPainter p(this);
    if(!this->isEnabled())
        p.setOpacity(0.5f);
    QStyleOption opt;
    opt.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
    if(m_pixmap) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPointF pos;
        if(m_hiResPixmap) {
            pos = QPointF(width() / 2 - m_pixmap->width() / (2 * m_pixmapDrawScale),
                          height() / 2 - m_pixmap->height() / (2 * m_pixmapDrawScale));
        } else {
            pos = QPointF(width() / 2 - m_pixmap->width() / 2, height() / 2 - m_pixmap->height() / 2);
        }
        p.drawPixmap(pos + m_iconOffset, *m_pixmap);
    }
}
