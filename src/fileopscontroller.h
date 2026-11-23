#pragma once

// Core's file operations, as an object: delete and trash, rename, reload,
// copy and move (quick, current-file and interactive), reveal in the file
// manager, the open dialog. See the .cpp header for what it owns and what it
// only borrows.

#include <QList>
#include <QObject>
#include <QString>

#include "gui/dialogs/dialogresult.h"
#include "utils/fileoperations.h" // FileOpResult

struct State;
class DirectoryModel;
class SelectionQuery;
class INavigationHost;
class ICoreView;
class IOverlayHost;
class IUserInteractions;
class DialogHost;

class FileOpsController : public QObject {
    Q_OBJECT
  public:
    // Every seam is borrowed. ui and overlays are the two a caller may replace
    // after construction (Core::setUserInteractions / setOverlayHost), so the
    // controller takes them by setter too and Core forwards each replacement:
    // a copy taken once at construction would keep answering to the old one.
    FileOpsController(const State &state, const SelectionQuery &selection, INavigationHost *host, ICoreView *view,
                      DialogHost *dialogs, IUserInteractions *ui, IOverlayHost *overlays, QObject *parent = nullptr);
    // the listing materializes on first load — the model arrives then
    void attachModel(DirectoryModel *model);
    void setUserInteractions(IUserInteractions *ui);
    void setOverlayHost(IOverlayHost *overlays);

  public slots:
    // what a keystroke, an overlay or a drop ends up invoking — the
    // ActionManager and the signal connects in core.cpp land here directly
    void removePermanent();
    void moveToTrash();
    void reloadImage();
    void reloadImage(const QString &filePath);
    void renameCurrentSelection(const QString &newName);
    void showRenameDialog();
    void showOpenDialog();
    void showInDirectory();
    void quickCopyToFixedDirectory();
    void copyCurrentFile(const QString &destDirectory);
    void moveCurrentFile(const QString &destDirectory);
    void interactiveCopy(const QList<QString> &paths, const QString &destDirectory);
    void interactiveMove(const QList<QString> &paths, const QString &destDirectory);

  private:
    FileOpResult removeFile(const QString &filePath, bool trash);
    void outputError(const FileOpResult &error) const;
    void reportBatchRemoval(const QString &doneVerb, int total, int successCount) const;
    void doInteractiveCopy(const QString &path, const QString &destDirectory, DialogResult &overwriteFiles);
    void doInteractiveMove(const QString &path, const QString &destDirectory, DialogResult &overwriteFiles);

    const State &m_state;
    const SelectionQuery &m_selection;
    INavigationHost *m_host;
    ICoreView *m_view;
    DialogHost *m_dialogs;
    IUserInteractions *m_ui;
    IOverlayHost *m_overlays;
    DirectoryModel *m_model = nullptr; // arrives via attachModel
};
