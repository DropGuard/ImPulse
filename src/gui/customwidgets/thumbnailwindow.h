#pragma once

/* The materialized window's one owner: the ThumbnailWidgets that exist for
 * the rows near a ThumbnailView's viewport, keyed by row index.
 *
 * A QGraphicsWidget is not a QObject-parent-tree citizen. Its lifetime belongs
 * to the item tree and the scene: addItem hands it to the scene, the scene's
 * destructor deletes what is left, and an item's own destructor deregisters it
 * from its scene. The view used to keep a bare QHash beside the scene and
 * delete from it at seven sites in three files, each written against the
 * same unwritten rule ("only the hash deletes, nobody touches one after").
 * This type is that rule as code: it is the only place a ThumbnailWidget is
 * put into the scene for the window, re-keyed, or deleted, and R15 in
 * src/tests/architecture_rules.py holds every other file to that.
 *
 * Two invariants hold at every point a caller can observe:
 *  - every widget in the hash is in the scene and alive, and its rowIndex()
 *    is its key;
 *  - the hash never holds a pointer to a dead widget, not even while a
 *    delete is running (entries leave the hash before their widget dies, so
 *    a re-entrant reader sees a smaller window, never a dangling one).
 *
 * The scene stays the backstop it always was: declared before the window in
 * ThumbnailView, it outlives it, so the window's destructor deletes its
 * widgets while their scene is still there.
 */

#include <QHash>
#include <QList>

class QGraphicsScene;
class ThumbnailWidget;

class ThumbnailWindow {
  public:
    using const_iterator = QHash<int, ThumbnailWidget *>::const_iterator;

    explicit ThumbnailWindow(QGraphicsScene &scene);
    ~ThumbnailWindow();
    ThumbnailWindow(const ThumbnailWindow &) = delete;
    ThumbnailWindow &operator=(const ThumbnailWindow &) = delete;

    // takes ownership: the widget goes into the scene under `index`. A widget
    // already held for that row is destroyed first — two widgets for one row
    // would leave the first in the scene as a ghost the window never tracks.
    void adopt(int index, ThumbnailWidget *widget);
    void destroy(int index);
    // every widget whose row is outside [first, last]
    void destroyOutside(int first, int last);
    void clear();
    // the listing diff: newIndices[old] is the row a widget follows to, -1 (or
    // past the list, or at/after rowCount) when its row left the listing.
    // Surviving widgets get their new rowIndex; placement is the caller's.
    // Two old rows landing on one new row is a malformed plan: the later one
    // keeps the row and the earlier is destroyed, never leaked.
    void rekey(const QList<int> &newIndices, int rowCount);

    ThumbnailWidget *value(int index) const;
    bool contains(int index) const;
    bool isEmpty() const;
    qsizetype count() const;
    // read-only iteration; key() is the row, value() the widget
    const_iterator constBegin() const;
    const_iterator constEnd() const;

  private:
    QGraphicsScene &m_scene;
    QHash<int, ThumbnailWidget *> m_widgets;
};
