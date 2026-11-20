#pragma once

#include "gui/customwidgets/overlaywidget.h"
#include <QApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QTimer>
#include <QHBoxLayout>

class ZoomIndicatorOverlay : public OverlayWidget {
    Q_OBJECT
  public:
    explicit ZoomIndicatorOverlay(FloatingWidgetContainer *parent = nullptr);

    void setScale(qreal scale);
    void show();
    void show(int duration);

  protected:
    void recalculateGeometry() override;

  private:
    QFontMetrics m_fm;
    QHBoxLayout m_layout;
    QLabel m_label;
    QTimer m_visibilityTimer;
    int m_hideDelay;
};
