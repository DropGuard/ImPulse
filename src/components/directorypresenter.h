#pragma once

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>
#include "gui/idirectoryview.h"
#include "components/thumbnailer/thumbnailer.h"
#include "directorymodel.h"
#include "sharedresources.h"
#include <QMimeData>

//tmp
#include <QtSvg/QSvgRenderer>

class DirectoryPresenter : public QObject {
    Q_OBJECT
  public:
    explicit DirectoryPresenter(Thumbnailer &_thumbnailer, QObject *parent = nullptr);

    // The view is an IDirectoryView, which is not a QObject, so a
    // pointer-to-member connect cannot be written against the interface (measured:
    // &IDirectoryView::itemActivated does not compile). It is written against the
    // concrete type instead, which every caller already has: MW hands out
    // FolderView* and ThumbnailStrip*, and both declare these five
    // signals itself. That makes the compiler check every signal/slot pair, and
    // it retires the SIGNAL()/SLOT() strings this used to be - strings that also
    // forced disconnectView to mirror all five signatures by hand.
    template <typename View> void setView(View *newView) {
        if(m_view)
            return; // the binding is already live
        m_view = newView;
        m_viewSender = newView; // a QPointer: it goes null with the widget
        connect(newView, &View::itemActivated, this, &DirectoryPresenter::onItemActivated);
        connect(newView, &View::thumbnailsRequested, this, &DirectoryPresenter::generateThumbnails);
        connect(newView, &View::draggedOut, this, &DirectoryPresenter::onDraggedOut);
        connect(newView, &View::draggedOver, this, &DirectoryPresenter::onDraggedOver);
        connect(newView, &View::droppedInto, this, &DirectoryPresenter::onDroppedInto);
        populateView();
        // these five connects are the only view -> presenter wiring there is,
        // so disconnectView can sever them all by sender+receiver without a
        // second list of signatures to keep in agreement
    }
    void setModel(std::shared_ptr<DirectoryModel> newModel);
    void unsetModel();

    void selectAndFocus(int index);
    void selectAndFocus(const QString &path);

    bool showDirs();
    void setShowDirs(bool mode);

    QList<QString> selectedPaths() const;

  signals:
    void dirActivated(const QString &dirPath);
    void fileActivated(const QString &filePath);
    void draggedOut(QList<QString>);
    void droppedInto(QList<QString>, QString);

  public slots:
    void disconnectView();

  private slots:
    void generateThumbnails(const QList<int> &, int, bool, bool);
    void onThumbnailReady(std::shared_ptr<Thumbnail> thumb, const QString &filePath);
    void populateView();
    void syncRows();
    void onItemActivated(int absoluteIndex);
    void onDraggedOut();
    void onDraggedOver(int index);

    void onDroppedInto(const QMimeData *data, QObject *source, int targetIndex);

  private:
    // observes; the Qt parent tree owns the view widget
    IDirectoryView *m_view = nullptr;
    // the same widget, seen as the QObject it also is: the disconnect needs to
    // ask whether it is still there, and QPointer answers that truthfully
    QPointer<QObject> m_viewSender;
    std::shared_ptr<DirectoryModel> m_model = nullptr;
    // shared with every other presenter, owned by Core (architecture rule R9):
    // one executor per engine event. Two presenters each holding their own
    // made the broadcast ThumbNext run the same decode twice and hand two mpv
    // writers one frame file
    Thumbnailer &m_thumbnailer;
    struct ViewEntry {
        quint64 id;
        QString name, path;
        quint64 contentRevision;
    };
    QList<ViewEntry> m_rows;
    bool m_showDirs;
};
