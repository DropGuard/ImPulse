#include "thumbnailwindow.h"

#include <QGraphicsScene>
#include <utility>

#include "gui/customwidgets/thumbnailwidget.h"

namespace {
// out of the scene, then dead. removeItem hands ownership back to the caller,
// so this delete is the only one the widget will ever see; the scene check
// keeps a widget some caller already removed from being "removed" twice.
void dispose(QGraphicsScene &scene, ThumbnailWidget *widget) {
    if(widget->scene() == &scene)
        scene.removeItem(widget);
    delete widget;
}
} // namespace

ThumbnailWindow::ThumbnailWindow(QGraphicsScene &scene) : m_scene(scene) {
}

ThumbnailWindow::~ThumbnailWindow() {
    clear();
}

void ThumbnailWindow::adopt(int index, ThumbnailWidget *widget) {
    destroy(index);
    widget->setRowIndex(index);
    m_scene.addItem(widget);
    m_widgets.insert(index, widget);
}

void ThumbnailWindow::destroy(int index) {
    if(auto *widget = m_widgets.take(index))
        dispose(m_scene, widget);
}

// entries leave the hash before any widget dies: a destructor that re-enters
// the view finds a consistent, smaller window
void ThumbnailWindow::destroyOutside(int first, int last) {
    QList<ThumbnailWidget *> leaving;
    for(auto it = m_widgets.begin(); it != m_widgets.end();) {
        if(it.key() < first || it.key() > last) {
            leaving.append(it.value());
            it = m_widgets.erase(it);
        } else {
            ++it;
        }
    }
    for(ThumbnailWidget *widget : std::as_const(leaving))
        dispose(m_scene, widget);
}

void ThumbnailWindow::clear() {
    const QHash<int, ThumbnailWidget *> leaving = std::exchange(m_widgets, {});
    for(ThumbnailWidget *widget : leaving)
        dispose(m_scene, widget);
}

void ThumbnailWindow::rekey(const QList<int> &newIndices, int rowCount) {
    QHash<int, ThumbnailWidget *> rekeyed;
    QList<ThumbnailWidget *> leaving;
    for(auto it = m_widgets.constBegin(); it != m_widgets.constEnd(); ++it) {
        const int newIndex = it.key() < newIndices.size() ? newIndices[it.key()] : -1;
        if(newIndex < 0 || newIndex >= rowCount) {
            leaving.append(it.value());
            continue;
        }
        if(ThumbnailWidget *previous = rekeyed.take(newIndex))
            leaving.append(previous);
        it.value()->setRowIndex(newIndex);
        rekeyed.insert(newIndex, it.value());
    }
    m_widgets = std::move(rekeyed);
    for(ThumbnailWidget *widget : std::as_const(leaving))
        dispose(m_scene, widget);
}

ThumbnailWidget *ThumbnailWindow::value(int index) const {
    return m_widgets.value(index, nullptr);
}

bool ThumbnailWindow::contains(int index) const {
    return m_widgets.contains(index);
}

bool ThumbnailWindow::isEmpty() const {
    return m_widgets.isEmpty();
}

qsizetype ThumbnailWindow::count() const {
    return m_widgets.count();
}

ThumbnailWindow::const_iterator ThumbnailWindow::constBegin() const {
    return m_widgets.constBegin();
}

ThumbnailWindow::const_iterator ThumbnailWindow::constEnd() const {
    return m_widgets.constEnd();
}
