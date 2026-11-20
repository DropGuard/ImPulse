#pragma once

#include <QtGlobal>
#include <QTimeLine>
#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QBoxLayout>
#include <QTimer>
#include <QTimeLine>
#include "floatingwidget.h"
#include "settings.h"
#include <memory>
#include <QDebug>
#include <ctime>

class SlidePanel : public FloatingWidget {
    Q_OBJECT
  public:
    explicit SlidePanel(FloatingWidgetContainer *parent);
    ~SlidePanel() override;
    bool hasWidget();
    void setWidget(QWidget *w);
    // Use visibleGeometry instead of geometry() here.
    // If this is called mid-animation then geometry() will be all wrong.
    QRect triggerRect();
    // when this is set, the widget will not change geometry by itself
    // no pos() animations & no recalculateGeometry()
    bool layoutManaged();
    void setLayoutManaged(bool mode);

    void setPosition(PanelPosition);
    PanelPosition position();
    void hideAnimated();

  public slots:
    void show();
    void hide();

  private slots:
    void onAnimationFinish();
    void animationUpdate(int frame);

  protected:
    QHBoxLayout m_layout;
    QGraphicsOpacityEffect *m_fadeEffect;
    int m_panelSize, m_slideAmount;
    QWidget *m_widget = nullptr;
    QRect m_triggerRect;
    void setAnimationRange(QPoint start, QPoint end);
    void saveStaticGeometry(QRect geometry);
    QRect staticGeometry();
    QTimer m_timer;
    QTimeLine m_timeline;
    QEasingCurve m_outCurve;
    const int m_animationDuration = 230;
    PanelPosition m_position;
    void recalculateGeometry() override;
    virtual void updateTriggerRect();

  private:
    void setOrientation();
    Qt::Orientation m_orientation;
    QRect m_staticGeometry;
    qreal m_panelVisibleOpacity = 1.0;
    QPoint m_startPosition, m_endPosition;
    bool m_layoutManaged = false;
};
