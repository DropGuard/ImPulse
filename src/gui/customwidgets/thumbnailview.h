#pragma once

/* This class manages QGraphicsScene, ThumbnailWidget list,
 * scrolling, requesting and setting thumbnails.
 * It doesn't do actual positioning of thumbnails within the scene.
 * (But maybe it should?)
 *
 * Usage: subclass, implement layout-related stuff
 */

#include <QGraphicsView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QWheelEvent>
#include <QTimeLine>
#include <QTimer>
#include <QElapsedTimer>
#include <QScreen>

#include "gui/customwidgets/thumbnailwidget.h"
#include "gui/idirectoryview.h"
#include "shortcutbuilder.h"

enum ThumbnailSelectMode {
    ACTIVATE_BY_PRESS,
    ACTIVATE_BY_DOUBLECLICK
};

enum ScrollDirection {
    SCROLL_FORWARDS,
    SCROLL_BACKWARDS
};

class ThumbnailView : public QGraphicsView, public IDirectoryView {
    Q_OBJECT
    Q_INTERFACES(IDirectoryView)
  public:
    ThumbnailView(Qt::Orientation orient, QWidget *parent = nullptr);
    void setDirectoryPath(QString path) override;
    void select(QList<int>) override;
    void select(int) override;
    QList<int> selection() override;
    int itemCount();

    void setSelectMode(ThumbnailSelectMode mode);
    int lastSelected();
    void clearSelection();
    void deselect(int index);

  public slots:
    void show();
    void showEvent(QShowEvent *event) override;
    void resetViewport();
    int thumbnailSize();
    void loadVisibleThumbnails();
    void loadVisibleThumbnailsDelayed();

    void focusOnSelection() override = 0;
    void populate(int count) override;
    void applyRows(const QList<DirectoryViewRow> &rows) override;
    void setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) override;
    void setDragHover(int index) override;

  signals:
    void itemActivated(int);
    void thumbnailsRequested(QList<int>, int, bool, bool);
    void draggedOut();
    void draggedOver(int);
    void droppedInto(const QMimeData *, QObject *, int);

  protected:
    QTimer m_loadTimer;
    bool m_blockThumbnailLoading;

    bool m_cropThumbnails, m_mouseReleaseSelect;
    int m_drawScrollbarIndicator;
    int m_thumbnailSize;
    bool m_rangeSelection;
    ThumbnailSelectMode m_selectMode;
    int m_lastScrollFrameTime;
    QList<int> m_selection;
    QPoint m_dragStartPos;
    ThumbnailWidget *m_dragTarget;

    void createScrollTimeLine();
    QElapsedTimer m_scrollFrameTimer;
    std::function<void(int)> m_centerOn;
    QElapsedTimer m_lastTouchpadScroll;
    Qt::Orientation m_orientation = Qt::Horizontal;

  protected:
    QGraphicsScene m_scene;
    QList<ThumbnailWidget *> m_thumbnails;
    QScrollBar *m_scrollBar;
    QTimeLine *m_scrollTimeLine;
    QPointF m_viewportCenter;

    QList<int> m_rangeSelectionSnapshot;
    bool m_wayland = false;

    int m_offscreenPreloadArea = 3000;

    QRect m_indicator;
    const int m_indicatorSize = 2;

    int m_scrollRefreshRate = 16;
    const int m_scrollDuration = 120;
    const float m_wheelScrollMultiplier = 2.5f;
    const float m_scrollAcceleration = 1.4f;
    const int m_scrollAccelerationThreshold = 50;

    const uint m_loadDelay = 150;
    ScrollDirection m_lastScrollDirection = SCROLL_FORWARDS;

    bool atSceneStart();
    bool atSceneEnd();

    bool checkRange(int pos);

    virtual ThumbnailWidget *createThumbnailWidget() = 0;
    virtual void syncThumbnailLayout() = 0;
    virtual void updateLayout();
    virtual void fitSceneToContents();
    virtual void updateScrollbarIndicator() = 0;

    void setOrientation(Qt::Orientation _orientation);
    Qt::Orientation orientation();

    void setCropThumbnails(bool);
    void setDrawScrollbarIndicator(bool mode);

    void addSelectionRange(int indexTo);

    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;

    bool eventFilter(QObject *o, QEvent *ev) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollToItem(int index);
    void scrollPrecise(int delta);
    void scrollByItem(int delta);
    void scrollSmooth(int delta);
    void scrollSmooth(int angleDelta, qreal multiplier, qreal acceleration);
    void scrollSmooth(int angleDelta, qreal multiplier, qreal acceleration, bool additive);
    void unloadAllThumbnails();
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;
};
