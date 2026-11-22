#pragma once

#include <QGraphicsWidget>

#include "gui/customwidgets/thumbnailview.h"
#include "gui/customwidgets/thumbnailwidget.h"
#include "utils/stuff.h"
#include "components/actionmanager/actionmanager.h"

class FolderGridView : public ThumbnailView {
    Q_OBJECT
  public:
    explicit FolderGridView(QWidget *parent = nullptr);
    ~FolderGridView() override;

    const int THUMBNAIL_SIZE_MIN = 80;  // px
    const int THUMBNAIL_SIZE_MAX = 400; // these should be divisible by ZOOM_STEP
    const int ZOOM_STEP = 20;
    void selectAll();

  public slots:
    void show();
    void hide();

    void selectFirst();
    void selectLast();
    void pageUp();
    void pageDown();
    void selectAbove();
    void selectBelow();
    void selectNext();
    void selectPrev();
    // the gridOpen action's landing: activates the current selection the way
    // the Enter key always has
    void activateCurrent();

    void zoomIn();
    void zoomOut();
    void setThumbnailSize(int newSize);
    void setShowLabels(bool mode);
    void focusOn(int index) override;
    void focusOnSelection() override;

  private:
    int m_shiftedCol;
    void scrollToCurrent();

    // the arithmetic layout that replaced FlowLayout: uniform cells wrapped
    // at perRow columns, centered per row — one prototype widget supplies
    // the cell size, everything else is derived
    mutable QSizeF m_cellSize;
    void refreshCellSize() const;
    qreal holderWidth() const;
    int perRow() const;
    int gridRows() const;

  private slots:
    void onitemSelected();

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void updateScrollbarIndicator() override;
    void syncThumbnailLayout() override;
    void configureWidget(ThumbnailWidget *widget) const override;
    void updateSceneExtent() override;

    QRectF cellSceneRect(int index) const override;
    int materializeFirst() const override;
    int materializeLast() const override;
    void placeWidget(ThumbnailWidget *widget, int index) override;

    // the FlowLayout navigation helpers, as pure index arithmetic over perRow
    int itemAbove(int index) const;
    int itemBelow(int index) const;
    int columnOf(int index) const;
    bool sameRow(int one, int two) const;

    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    // the grid scope is armed exactly while the grid holds keyboard focus:
    // focusIn/focusOut drive the projection's de-claim of the grid's keys
    // from the global table
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    bool focusNextPrevChild(bool) override;

  signals:
    void thumbnailSizeChanged(int);
};
