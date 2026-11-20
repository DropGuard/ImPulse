#include "styledcombobox.h"

#include "components/actionmanager/actionmanager.h"

StyledComboBox::StyledComboBox(QWidget *parent) : QComboBox(parent), m_hiResPixmap(false) {
    m_dpr = this->devicePixelRatioF();
    // `settings` outlives every widget, so the connection has to be bound to
    // this widget: the four-argument form disconnects it when `this` dies.
    connect(settings, &Settings::settingsChanged, this, [this]() {
        ImageLib::recolor(this->m_downArrow, settings->colorScheme().icons);
    });
}

void StyledComboBox::setIconPath(QString path) {
    if(m_dpr >= (1.0 + 0.001)) {
        path.replace(".", "@2x.");
        m_hiResPixmap = true;
        m_downArrow.load(path);
        if(m_dpr >= (2.0 - 0.001))
            m_pixmapDrawScale = m_dpr;
        else
            m_pixmapDrawScale = 2.0;
        m_downArrow.setDevicePixelRatio(m_pixmapDrawScale);
    } else {
        m_hiResPixmap = false;
        m_downArrow.load(path);
        m_pixmapDrawScale = m_dpr;
    }
    ImageLib::recolor(m_downArrow, settings->colorScheme().icons);
    update();
}

void StyledComboBox::paintEvent(QPaintEvent *e) {
    QComboBox::paintEvent(e);
    QPainter p(this);
    QPointF pos(0, 0);

    if(m_hiResPixmap) {
        pos = QPointF(width() - 8 - m_downArrow.width() / m_pixmapDrawScale,
                      height() / 2 - m_downArrow.height() / (2 * m_pixmapDrawScale));
    } else {
        pos = QPointF(width() - m_downArrow.width() - 8, (height() - m_downArrow.height()) / 2);
    }
    p.drawPixmap(pos, m_downArrow);
}

// The input-routing combo gives up every key with ignore() and lets the
// parent chain answer (in the crop panel that ends at the crop workflow).
// For that contract to survive QShortcutMap, every override must be claimed —
// an unclaimed key would fire its global action and never reach the chain.
bool StyledComboBox::event(QEvent *event) {
    if(event->type() == QEvent::ShortcutOverride)
        event->accept();
    return QComboBox::event(event);
}

void StyledComboBox::keyPressEvent(QKeyEvent *event) {
    event->ignore();
}
