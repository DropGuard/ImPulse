#pragma once

#include <QApplication>
#include <QLabel>
#include <QBoxLayout>
#include <QScrollArea>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QPainter>
#include <QResizeEvent>
#include <cmath>

#include "gui/customwidgets/thumbnailview.h"
#include "gui/customwidgets/thumbnailwidget.h"
#include "sourcecontainers/thumbnail.h"

class ThumbnailStrip : public ThumbnailView {
    Q_OBJECT
  public:
    explicit ThumbnailStrip(QWidget *parent = nullptr);
    QSize itemSize();
    void readSettings();

  private:
    const int m_thumbPadding = 9;
    int m_thumbMarginX = 2, m_thumbMarginY = 4;
    void updateThumbnailPositions(int start, int end);
    void updateThumbnailPositions();
    void setupLayout();
    ThumbnailStyle m_currentStyle;

  public slots:
    void focusOn(int index) override;
    void focusOnSelection() override;

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void updateScrollbarIndicator() override;
    void syncThumbnailLayout() override;
    ThumbnailWidget *createThumbnailWidget() override;
};
