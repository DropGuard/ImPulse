#ifndef STYLEDCOMBOBOX_H
#define STYLEDCOMBOBOX_H

#include <QComboBox>
#include <QPainter>
#include <QKeyEvent>
#include "settings.h"
#include "utils/imagelib.h"

class StyledComboBox : public QComboBox {
  public:
    StyledComboBox(QWidget *parent = nullptr);
    void setIconPath(QString path);

  protected:
    void paintEvent(QPaintEvent *e) override;
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    bool m_hiResPixmap;
    QPixmap m_downArrow;
    qreal m_dpr, m_pixmapDrawScale;
};

#endif // STYLEDCOMBOBOX_H
