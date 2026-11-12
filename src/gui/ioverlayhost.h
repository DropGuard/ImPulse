#pragma once

#include <QMap>
#include <QObject>
#include <QString>

// What Core needs from the overlay layer, and nothing more.
//
// Why this is an interface rather than a direct OverlayHost member:
//
// Core reached the overlays through `mw->overlays()`, 59 times, plus ten signal
// connections. That made MainWindow a hard dependency of every Core path --
// including the file operations and navigation decisions that have nothing to
// do with overlays but could not be constructed without a window. This is the
// seam that lets those tests supply a scripted host instead.
//
// It is a QObject because the contract includes signals: the overlays ask Core
// to copy, move, rename, save and discard, and those requests are part of the
// surface Core depends on, not an implementation detail of the widget.
class IOverlayHost : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;

    // The nine calls Core makes. A method appears here because Core calls it,
    // not because OverlayHost has it -- the interface is Core's needs, and
    // OverlayHost happens to be one implementation.
    virtual void showMessage(const QString &text, int duration = 1500) = 0;
    virtual void showMessageSuccess(const QString &text) = 0;
    virtual void showWarning(const QString &text) = 0;
    virtual void showError(const QString &text) = 0;
    virtual void showMessageDirectory(const QString &dirName) = 0;
    virtual void setExifInfo(const QMap<QString, QString> &exifInfo) = 0;
    virtual void toggleRenameOverlay(const QString &currentName) = 0;
    virtual void showSaveOverlay() = 0;
    virtual void hideSaveOverlay() = 0;
    // action-driven surfaces: the copy and move menus, and the image info
    virtual void triggerCopyOverlay() = 0;
    virtual void triggerMoveOverlay() = 0;
    virtual void toggleImageInfoOverlay() = 0;

  signals:
    void copyRequested(QString path);
    void moveRequested(QString path);
    void renameRequested(QString path);
    void saveRequested();
    void saveAsClicked();
    void discardEditsRequested();
};
