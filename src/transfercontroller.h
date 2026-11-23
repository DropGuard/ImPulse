#pragma once

// Core's clipboard and drag-and-drop, as an object: the app as a mime source
// (clipboard writes, drags out) and a mime sink (drops in, pastes). See the
// .cpp header for what it owns and what it only borrows.

#include <QList>
#include <QObject>
#include <QString>
#include <memory>

class QMimeData;
class Image;
class DirectoryModel;
class SelectionQuery;
class NavigationController;
class FileOpsController;
class IOverlayHost;
class DialogHost;

// what a mime payload is for: the clipboard carries the pixels, a drag must
// not (setImageData during a drag hangs Xorg)
enum MimeDataTarget {
    TARGET_CLIPBOARD,
    TARGET_DROP
};

class TransferController : public QObject {
    Q_OBJECT
  public:
    // Every seam is borrowed. overlays is the one a caller may replace after
    // construction (Core::setOverlayHost), so it has a setter Core forwards to.
    // A paste places files through the file operations and a drop opens one
    // through navigation: this object adds no file or navigation machinery.
    TransferController(const SelectionQuery &selection, NavigationController &nav, FileOpsController &fileOps,
                       DialogHost *dialogs, IOverlayHost *overlays, QObject *parent = nullptr);
    // the listing materializes on first load — the model arrives then
    void attachModel(DirectoryModel *model);
    void setOverlayHost(IOverlayHost *overlays);

    // the payload for a drag of these paths
    QMimeData *mimeDataForPaths(const QList<QString> &paths);
    // what a drag or clipboard entry carries for one image
    QMimeData *getMimeDataForImage(std::shared_ptr<Image> img, MimeDataTarget target);

  public slots:
    void copyFileClipboard();
    void copyPathClipboard();
    void openFromClipboard();
    // The folder view's Ctrl+V: the clipboard holds file paths, so this copies
    // them into the directory being browsed. It is deliberately NOT the same
    // command as openFromClipboard — that one shows the clipboard's image,
    // which needs a document view; this one places files, which needs a
    // directory. Same key, two scopes, because the subject differs.
    void pasteFilesToCurrentDirectory();
    // The drag-and-drop entry points a real gesture uses: a drop from outside
    // the app, and a drag out of it (the action's form drags the selection).
    // A drag out carries this object as its source, which is how a drop
    // recognizes one of the app's own and ignores it.
    void onDropIn(const QMimeData *mimeData, QObject *source);
    void onDraggedOut();
    void onDraggedOut(const QList<QString> &paths);

  private:
    const SelectionQuery &m_selection;
    NavigationController &m_nav;
    FileOpsController &m_fileOps;
    DialogHost *m_dialogs;
    IOverlayHost *m_overlays;
    DirectoryModel *m_model = nullptr; // arrives via attachModel
};
