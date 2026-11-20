#pragma once

#include "gui/customwidgets/iconwidget.h"
#include <QMouseEvent>

class IconButton : public IconWidget {
    Q_OBJECT
  public:
    explicit IconButton(QWidget *parent = nullptr);
    void setCheckable(bool mode);
    bool isChecked();

  public slots:
    void setChecked(bool mode);

  signals:
    void clicked();
    void toggled(bool);

  protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool m_checkable, m_checked, m_pressed;
    void mouseMoveEvent(QMouseEvent *event) override;
};
