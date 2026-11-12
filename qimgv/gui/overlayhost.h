#pragma once

// Transient surfaces, extracted from MW (the strangler rewrite of
// mainwindow.cpp): the floating message, the copy/move overlay, the save
// confirmation overlay, the rename overlay, the image info overlay, and the
// controls overlay, plus the lazy setup dance each of them used to get in MW.
// Domain requests the overlays emit are re-emitted here; Core connects to
// this host directly.

#include <QObject>

#include "gui/ioverlayhost.h"

#include "gui/overlays/floatingmessageproxy.h"
#include "gui/overlays/imageinfooverlayproxy.h"

class MW;
class ViewerWidget;
class DocumentWidget;
class CopyOverlay;
class SaveConfirmOverlay;
class RenameOverlay;
class ControlsOverlay;

class OverlayHost : public IOverlayHost {
    Q_OBJECT
  public:
    explicit OverlayHost(MW *container, ViewerWidget *viewer, DocumentWidget *docWidget);
    ~OverlayHost() override;

    void triggerCopyOverlay() override;
    void triggerMoveOverlay() override;
    void showSaveOverlay() override;
    void hideSaveOverlay() override;
    void toggleRenameOverlay(const QString &currentName) override;
    void toggleImageInfoOverlay() override;
    void setExifInfo(const QMap<QString, QString> &exifInfo) override;
    void showMessageDirectory(const QString &dirName) override;
    void showMessage(const QString &text, int duration = 1500) override;
    void showMessageSuccess(const QString &text) override;
    void showWarning(const QString &text) override;
    void showError(const QString &text) override;
    // settings-driven visibility of the fullscreen controls overlay
    void updateControlsOverlay();
    void hideControlsOverlay();
    // view switches hide every transient surface
    void hideTransientOverlays();
    // the projection re-derives the rename target on every state write
    void setRenameOverlayName(const QString &name);
    void saveCopySettings();

  signals:

  private:
    void setupCopyOverlay();
    void setupSaveOverlay();
    void setupRenameOverlay();

    MW *container;
    ViewerWidget *viewer;
    DocumentWidget *docWidget;
    // non-QObject proxies: this host is their owner
    FloatingMessageProxy *floatingMessage = nullptr;
    ImageInfoOverlayProxy *imageInfoOverlay = nullptr;
    // QObject widgets: the parent tree owns them, these pointers observe
    CopyOverlay *copyOverlay = nullptr;
    SaveConfirmOverlay *saveOverlay = nullptr;
    RenameOverlay *renameOverlay = nullptr;
    ControlsOverlay *controlsOverlay = nullptr;
};
