#pragma once

// Core's editing, as an object: the in-memory transforms (rotate, flip,
// resize, crop), the crop panel toggle, the save contract that commits an
// edit, discard, and edit recovery. See the .cpp header for what it owns and
// what it only borrows.

#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <functional>
#include <memory>

class QImage;
struct State;
class DirectoryModel;
class SelectionQuery;
class NavigationController;
class INavigationHost;
class DialogHost;
class IOverlayHost;
class ICropSurface;

class EditController : public QObject {
    Q_OBJECT
  public:
    // Every seam is borrowed; crop is optional (null headless without a crop
    // fake). overlays is replaceable after construction (Core::setOverlayHost),
    // so it has a setter Core forwards to. `windowed` says whether Core built
    // a window: the save-as dialog and the recovery picker exist only there,
    // and the dialog host a headless Core owns has nothing to parent them to.
    // It is fixed for Core's lifetime, so it is a value, not a query.
    EditController(const State &state, const SelectionQuery &selection, NavigationController &nav,
                   INavigationHost *host, DialogHost *dialogs, IOverlayHost *overlays, ICropSurface *crop,
                   bool windowed, QObject *parent = nullptr);
    // the listing materializes on first load — the model arrives then
    void attachModel(DirectoryModel *model);
    void setOverlayHost(IOverlayHost *overlays);

  public slots:
    // what a keystroke, an overlay, the crop surface or the resize dialog ends
    // up invoking — the connects in core.cpp land here directly
    void rotateLeft();
    void rotateRight();
    void flipH();
    void flipV();
    void resize(QSize size);
    void crop(QRect rect);
    void cropAndSave(QRect rect);
    void toggleCropPanel();
    void showResizeDialog();
    void discardEdits();
    void saveCurrentFile();
    void saveCurrentFileAs(const QString &destPath);
    void requestSavePath();
    void recoverEdits();

  private:
    void rotateByDegrees(int degrees);
    bool saveFile(const QString &filePath);
    bool saveFile(const QString &filePath, const QString &newPath);
    template <typename... Args>
    void edit_template(const std::function<std::unique_ptr<const QImage>(std::shared_ptr<const QImage>, Args...)> &func,
                       Args &&...as);

    const State &m_state;
    const SelectionQuery &m_selection;
    NavigationController &m_nav;
    INavigationHost *m_host;
    DialogHost *m_dialogs;
    IOverlayHost *m_overlays;
    ICropSurface *m_crop;
    const bool m_windowed;
    DirectoryModel *m_model = nullptr; // arrives via attachModel
};
