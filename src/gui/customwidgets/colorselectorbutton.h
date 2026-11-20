#ifndef COLORSELECTORBUTTON_H
#define COLORSELECTORBUTTON_H

#include <QPainter>
#include <QColorDialog>
#include "gui/customwidgets/clickablelabel.h"

class ColorSelectorButton : public ClickableLabel {
    Q_OBJECT
  public:
    explicit ColorSelectorButton(QWidget *parent = nullptr);

    void setColor(QColor &newColor);
    QColor color();
    void setDescription(QString text);

  protected:
    void paintEvent(QPaintEvent *e) override;

  private slots:
    void showColorSelector();

  private:
    QColor m_color;
    QString m_description;
};

#endif // COLORSELECTORBUTTON_H
