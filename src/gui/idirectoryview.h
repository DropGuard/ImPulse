#pragma once

#include <QtPlugin>
#include <QList>
#include <QString>
#include <memory>

class Thumbnail;
class QMimeData;

struct DirectoryViewRow {
    int previousIndex = -1;
    QString newName = {};
    bool contentChanged = false;
};

class IDirectoryView {
  public:
    virtual ~IDirectoryView() {
    }

    virtual void populate(int) = 0;
    virtual void applyRows(const QList<DirectoryViewRow> &rows) = 0;
    virtual void setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) = 0;
    virtual void select(QList<int>) = 0;
    virtual void select(int) = 0;
    virtual void focusOn(int) = 0;
    virtual void focusOnSelection() = 0;
    virtual QList<int> selection() = 0;
    virtual void setDirectoryPath(QString path) = 0;
    virtual void setDragHover(int index) = 0;

    // No signal declarations here: the interface is not a QObject, so these
    // members could never be connected through the base — the concrete view
    // classes declare the five events as real signals, and the presenter
    // binds against the concrete type it is handed.
};

Q_DECLARE_INTERFACE(IDirectoryView, "IDirectoryView")
