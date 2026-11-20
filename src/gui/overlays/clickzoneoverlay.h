#pragma once

#include <QHBoxLayout>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QDebug>
#include "gui/customwidgets/floatingwidget.h"
#include "utils/imagelib.h"

enum ActiveHighlightZone {
    HIGHLIGHT_NONE,
    HIGHLIGHT_LEFT,
    HIGHLIGHT_RIGHT
};

class ClickZoneOverlay : public FloatingWidget {
    Q_OBJECT
  public:
    explicit ClickZoneOverlay(FloatingWidgetContainer *parent);
    QRect leftZone();
    QRect rightZone();
    void highlightLeft();
    void highlightRight();
    void disableHighlight();
    void setHighlightedZone(ActiveHighlightZone zone);
    bool isHighlighted();
    void setPressed(bool mode);

  public slots:
    void readSettings();

  private:
    QPixmap loadPixmap(QString path);
    QPixmap m_pixmapLeft, m_pixmapRight;
    QRect m_leftZone, m_rightZone;
    qreal m_dpr, m_pixmapDrawScale;
    bool m_hiResPixmaps = false;
    const int m_zoneSize = 110;
    bool m_isPressed = false;
    bool m_leftHovered = false, m_rightHovered = false;
    bool m_drawZones = true;
    ActiveHighlightZone m_activeZone = HIGHLIGHT_NONE;
    void drawPixmap(QPainter &p, const QPixmap &pixmap, QRect rect);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void recalculateGeometry() override;
};
