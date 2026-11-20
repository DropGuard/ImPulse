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
    : IOverlayHost(containerArg), m_container(containerArg), m_viewer(viewerArg), m_docWidget(docWidgetArg) {
    m_floatingMessage = new FloatingMessageProxy(m_viewer); // todo: use additional one for folderview?
    m_imageInfoOverlay = new ImageInfoOverlayProxy(m_viewer);
    m_controlsOverlay = new ControlsOverlay(m_docWidget);
}

OverlayHost::~OverlayHost() {
    delete m_floatingMessage;
    delete m_imageInfoOverlay;
}

void OverlayHost::setupCopyOverlay() {
    m_copyOverlay = new CopyOverlay(m_viewer);
    m_pendingSurfaces.append(m_copyOverlay);
    connect(m_copyOverlay, &CopyOverlay::copyRequested, this, &OverlayHost::copyRequested);
    connect(m_copyOverlay, &CopyOverlay::moveRequested, this, &OverlayHost::moveRequested);
}

void OverlayHost::setupSaveOverlay() {
    // not a pending surface: this prompt IS the projection of an uncommitted
    // edit (Core shows it from isEdited()), and that state is already the
    // lock's condition. Registering it would answer the same question twice.
    m_saveOverlay = new SaveConfirmOverlay(m_viewer);
    connect(m_saveOverlay, &SaveConfirmOverlay::saveClicked, this, &OverlayHost::saveRequested);
    connect(m_saveOverlay, &SaveConfirmOverlay::saveAsClicked, this, &OverlayHost::saveAsClicked);
    connect(m_saveOverlay, &SaveConfirmOverlay::discardClicked, this, &OverlayHost::discardEditsRequested);
}

void OverlayHost::setupRenameOverlay() {
    m_renameOverlay = new RenameOverlay(m_container);
    m_pendingSurfaces.append(m_renameOverlay);
    m_renameOverlay->setName(QString());
    connect(m_renameOverlay, &RenameOverlay::renameRequested, this, &OverlayHost::renameRequested);
}

void OverlayHost::triggerCopyOverlay() {
    if(!m_viewer->isDisplaying())
        return;
    if(!m_copyOverlay)
        setupCopyOverlay();

    if(m_container->currentViewMode() == MODE_FOLDERVIEW)
        return;
    if(m_copyOverlay->operationMode() == OVERLAY_COPY) {
        m_copyOverlay->isHidden() ? m_copyOverlay->show() : m_copyOverlay->hide();
    } else {
        m_copyOverlay->setDialogMode(OVERLAY_COPY);
        m_copyOverlay->show();
    }
}

void OverlayHost::triggerMoveOverlay() {
    if(!m_viewer->isDisplaying())
        return;
    if(!m_copyOverlay)
        setupCopyOverlay();

    if(m_container->currentViewMode() == MODE_FOLDERVIEW)
        return;
    if(m_copyOverlay->operationMode() == OVERLAY_MOVE) {
        m_copyOverlay->isHidden() ? m_copyOverlay->show() : m_copyOverlay->hide();
    } else {
        m_copyOverlay->setDialogMode(OVERLAY_MOVE);
        m_copyOverlay->show();
    }
}

void OverlayHost::showSaveOverlay() {
    if(!settings->showSaveOverlay())
        return;
    if(!m_saveOverlay)
        setupSaveOverlay();
    m_saveOverlay->show();
    m_saveOverlay->setFocus();
}

void OverlayHost::hideSaveOverlay() {
    if(!m_saveOverlay)
        return;
    m_saveOverlay->hide();
}

void OverlayHost::toggleRenameOverlay(const QString &currentName) {
    if(!m_renameOverlay)
        setupRenameOverlay();
    if(m_renameOverlay->isHidden()) {
        m_renameOverlay->setBackdropEnabled((m_container->currentViewMode() == MODE_FOLDERVIEW));
        m_renameOverlay->setName(currentName);
        m_renameOverlay->show();
    } else {
        m_renameOverlay->hide();
    }
}

void OverlayHost::toggleImageInfoOverlay() {
    if(m_container->currentViewMode() == MODE_FOLDERVIEW)
        return;
    if(m_imageInfoOverlay->isHidden())
        m_imageInfoOverlay->show();
    else
        m_imageInfoOverlay->hide();
}

void OverlayHost::setExifInfo(const QMap<QString, QString> &exifInfo) {
    if(m_imageInfoOverlay)
        m_imageInfoOverlay->setExifInfo(exifInfo);
}

void OverlayHost::showMessageDirectory(const QString &dirName) {
    m_floatingMessage->showMessage(dirName, FloatingMessageIcon::ICON_DIRECTORY, 1700);
}

void OverlayHost::showMessage(const QString &text, int duration) {
    m_floatingMessage->showMessage(text, FloatingMessageIcon::NO_ICON, duration);
}

void OverlayHost::showMessageSuccess(const QString &text) {
    m_floatingMessage->showMessage(text, FloatingMessageIcon::ICON_SUCCESS, 1500);
}

void OverlayHost::showWarning(const QString &text) {
    m_floatingMessage->showMessage(text, FloatingMessageIcon::ICON_WARNING, 1500);
}

void OverlayHost::showError(const QString &text) {
    m_floatingMessage->showMessage(text, FloatingMessageIcon::ICON_ERROR, 2800);
}

void OverlayHost::updateControlsOverlay() {
    auto pos = settings->panelPosition();
    if(!settings->panelEnabled() || pos == PANEL_BOTTOM || pos == PANEL_LEFT)
        m_controlsOverlay->show();
    else
        m_controlsOverlay->hide();
}

void OverlayHost::hideControlsOverlay() {
    m_controlsOverlay->hide();
}

bool OverlayHost::hasPendingSurface() const {
    for(const IPendingSurface *surface : m_pendingSurfaces)
        if(surface->isPending())
            return true;
    return false;
}

void OverlayHost::hideTransientOverlays() {
    if(m_copyOverlay)
        m_copyOverlay->hide();
    if(m_renameOverlay)
        m_renameOverlay->hide();
    m_imageInfoOverlay->hide();
}

void OverlayHost::setRenameOverlayName(const QString &name) {
    if(m_renameOverlay)
        m_renameOverlay->setName(name);
}

void OverlayHost::saveCopySettings() {
    if(m_copyOverlay)
        m_copyOverlay->saveSettings();
}
