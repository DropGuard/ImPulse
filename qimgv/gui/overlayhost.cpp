#include "gui/overlayhost.h"

#include "gui/mainwindow.h"
#include "gui/overlays/controlsoverlay.h"
#include "gui/overlays/copyoverlay.h"
#include "gui/overlays/renameoverlay.h"
#include "gui/overlays/saveconfirmoverlay.h"
#include "gui/viewers/viewerwidget.h"
#include "gui/viewers/documentwidget.h"
#include "settings.h"

OverlayHost::OverlayHost(MW *containerArg, ViewerWidget *viewerArg, DocumentWidget *docWidgetArg)
    : IOverlayHost(containerArg), container(containerArg), viewer(viewerArg), docWidget(docWidgetArg) {
    floatingMessage = new FloatingMessageProxy(viewer); // todo: use additional one for folderview?
    imageInfoOverlay = new ImageInfoOverlayProxy(viewer);
    controlsOverlay = new ControlsOverlay(docWidget);
}

OverlayHost::~OverlayHost() {
    delete floatingMessage;
    delete imageInfoOverlay;
}

void OverlayHost::setupCopyOverlay() {
    copyOverlay = new CopyOverlay(viewer);
    connect(copyOverlay, &CopyOverlay::copyRequested, this, &OverlayHost::copyRequested);
    connect(copyOverlay, &CopyOverlay::moveRequested, this, &OverlayHost::moveRequested);
}

void OverlayHost::setupSaveOverlay() {
    saveOverlay = new SaveConfirmOverlay(viewer);
    connect(saveOverlay, &SaveConfirmOverlay::saveClicked, this, &OverlayHost::saveRequested);
    connect(saveOverlay, &SaveConfirmOverlay::saveAsClicked, this, &OverlayHost::saveAsClicked);
    connect(saveOverlay, &SaveConfirmOverlay::discardClicked, this, &OverlayHost::discardEditsRequested);
}

void OverlayHost::setupRenameOverlay() {
    renameOverlay = new RenameOverlay(container);
    renameOverlay->setName(QString());
    connect(renameOverlay, &RenameOverlay::renameRequested, this, &OverlayHost::renameRequested);
}

void OverlayHost::triggerCopyOverlay() {
    if(!viewer->isDisplaying())
        return;
    if(!copyOverlay)
        setupCopyOverlay();

    if(container->currentViewMode() == MODE_FOLDERVIEW)
        return;
    if(copyOverlay->operationMode() == OVERLAY_COPY) {
        copyOverlay->isHidden() ? copyOverlay->show() : copyOverlay->hide();
    } else {
        copyOverlay->setDialogMode(OVERLAY_COPY);
        copyOverlay->show();
    }
}

void OverlayHost::triggerMoveOverlay() {
    if(!viewer->isDisplaying())
        return;
    if(!copyOverlay)
        setupCopyOverlay();

    if(container->currentViewMode() == MODE_FOLDERVIEW)
        return;
    if(copyOverlay->operationMode() == OVERLAY_MOVE) {
        copyOverlay->isHidden() ? copyOverlay->show() : copyOverlay->hide();
    } else {
        copyOverlay->setDialogMode(OVERLAY_MOVE);
        copyOverlay->show();
    }
}

void OverlayHost::showSaveOverlay() {
    if(!settings->showSaveOverlay())
        return;
    if(!saveOverlay)
        setupSaveOverlay();
    saveOverlay->show();
    saveOverlay->setFocus();
}

void OverlayHost::hideSaveOverlay() {
    if(!saveOverlay)
        return;
    saveOverlay->hide();
}

void OverlayHost::toggleRenameOverlay(const QString &currentName) {
    if(!renameOverlay)
        setupRenameOverlay();
    if(renameOverlay->isHidden()) {
        renameOverlay->setBackdropEnabled((container->currentViewMode() == MODE_FOLDERVIEW));
        renameOverlay->setName(currentName);
        renameOverlay->show();
    } else {
        renameOverlay->hide();
    }
}

void OverlayHost::toggleImageInfoOverlay() {
    if(container->currentViewMode() == MODE_FOLDERVIEW)
        return;
    if(imageInfoOverlay->isHidden())
        imageInfoOverlay->show();
    else
        imageInfoOverlay->hide();
}

void OverlayHost::setExifInfo(const QMap<QString, QString> &exifInfo) {
    if(imageInfoOverlay)
        imageInfoOverlay->setExifInfo(exifInfo);
}

void OverlayHost::showMessageDirectory(const QString &dirName) {
    floatingMessage->showMessage(dirName, FloatingMessageIcon::ICON_DIRECTORY, 1700);
}

void OverlayHost::showMessage(const QString &text, int duration) {
    floatingMessage->showMessage(text, FloatingMessageIcon::NO_ICON, duration);
}

void OverlayHost::showMessageSuccess(const QString &text) {
    floatingMessage->showMessage(text, FloatingMessageIcon::ICON_SUCCESS, 1500);
}

void OverlayHost::showWarning(const QString &text) {
    floatingMessage->showMessage(text, FloatingMessageIcon::ICON_WARNING, 1500);
}

void OverlayHost::showError(const QString &text) {
    floatingMessage->showMessage(text, FloatingMessageIcon::ICON_ERROR, 2800);
}

void OverlayHost::updateControlsOverlay() {
    auto pos = settings->panelPosition();
    if(!settings->panelEnabled() || pos == PANEL_BOTTOM || pos == PANEL_LEFT)
        controlsOverlay->show();
    else
        controlsOverlay->hide();
}

void OverlayHost::hideControlsOverlay() {
    controlsOverlay->hide();
}

void OverlayHost::hideTransientOverlays() {
    if(copyOverlay)
        copyOverlay->hide();
    if(renameOverlay)
        renameOverlay->hide();
    imageInfoOverlay->hide();
}

void OverlayHost::setRenameOverlayName(const QString &name) {
    if(renameOverlay)
        renameOverlay->setName(name);
}

void OverlayHost::saveCopySettings() {
    if(copyOverlay)
        copyOverlay->saveSettings();
}
