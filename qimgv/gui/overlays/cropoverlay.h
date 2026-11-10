#pragma once

#include "gui/customwidgets/floatingwidget.h"
#include <QDebug>
#include <QPainter>
#include <QPaintEvent>
#include <QColor>

enum CursorAction {
    NO_DRAG,         // 0
    SELECTION_START, // 1
    DRAG_SELECT,     // 2
    DRAG_MOVE,       // 3
    DRAG_LEFT,       // 4
    DRAG_RIGHT,      // 5
    DRAG_TOP,        // 6
    DRAG_BOTTOM,     // 7
    DRAG_TOPLEFT,    // 8
    DRAG_TOPRIGHT,   // 9
    DRAG_BOTTOMLEFT, // 10
    DRAG_BOTTOMRIGHT // 11
};

class ViewerWidget;

class CropOverlay : public FloatingWidget {
    Q_OBJECT
  public:
    // the overlay pulls its geometry live from the viewer (image rect,
    // scale, source size) — it stores nothing that the viewer owns, so a
    // zoom or image change mid-crop can never leave it stale
    explicit CropOverlay(ViewerWidget *viewer = nullptr);

    // observability: the scale the overlay draws with (live from the viewer)
    float imageScale() const;
    // a new image invalidates the selection in image coordinates
    void onImageChanged();
    void clearSelection();

  signals:
    void selectionChanged(QRect);
    void escPressed();
    void cropDefault();
    void cropSave();

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    // selection state is the overlay's own; image geometry is pulled live
    ViewerWidget *viewer;
    QPoint startPos, endPos, moveStartPos, resizeAnchor;
    QRect selectionRect, selectionDrawRect, selectionDrawRectDpi, handles[8];
    bool lockAspectRatio;
    QBrush brushInactiveTint, brushDarkGray, brushGray, brushLightGray;
    QRectF handlesDpi[8];
    CursorAction cursorAction;
    QPen selectionOutlinePen;
    // both queried live: a window dragged across screens with different DPRs
    // must not keep converting coordinates with the construction-time ratio
    qreal dpr() const {
        return devicePixelRatioF();
    }
    int handleSize() const {
        return static_cast<int>(8 * dpr());
    }
    QPointF ar;

    QRect placeInside(QRect what, QRect where);
    void drawSelection(QPainter *);
    void drawHandles(QBrush &, QPainter *);
    void updateHandlePositions();
    void prepareDrawElements();
    CursorAction hoverTarget(QPoint pos);
    QRect imageRectLive() const;
    QRect imageDrawRectDevice() const;
    QRect imageDrawRectLogical() const;
    void resizeSelection(QPoint d);
    void resizeSelectionAR(QPoint d);
    void resizeSelectionFree(QPoint d);
    void recalculateGeometry() override;
    QPoint mapPointToImage(QPoint p);
    void updateSelectionDrawRect();
    void setCursorAction(CursorAction action);
    void setResizeAnchor(CursorAction action);
    bool hasSelection();
  public slots:
    void hide();
    void onSelectionOutsideChange(QRect selection);
    void selectAll();
    void setAspectRatio(QPointF);
    void setLockAspectRatio(bool mode);
};
