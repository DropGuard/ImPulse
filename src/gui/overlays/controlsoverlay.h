#pragma once

#include <QHBoxLayout>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QDebug>
#include "gui/customwidgets/floatingwidget.h"
#include "gui/customwidgets/actionbutton.h"

class ControlsOverlay : public FloatingWidget {
    Q_OBJECT
  public:
    explicit ControlsOverlay(FloatingWidgetContainer *parent);

  public slots:
    void show();

  private:
    QHBoxLayout m_layout;
    ActionButton *m_closeButton, *m_settingsButton, *m_folderViewButton;
    QGraphicsOpacityEffect *m_fadeEffect;
    QPropertyAnimation *m_fadeAnimation;
    QSize contentsSize();
    void fitToContents();

  protected:
    void recalculateGeometry() override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
};
