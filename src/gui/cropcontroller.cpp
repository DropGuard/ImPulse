#include "gui/cropcontroller.h"

#include "gui/panels/croppanel/croppanel.h"
#include "gui/overlays/cropoverlay.h"
#include "gui/panels/sidepanel/sidepanel.h"
#include "gui/viewers/viewerwidget.h"

CropController::CropController(MW *windowArg, ViewerWidget *viewerArg, SidePanel *sidePanelArg,
                               DocumentWidget *docWidgetArg)
    : QObject(windowArg), window(windowArg), viewer(viewerArg), sidePanel(sidePanelArg), docWidget(docWidgetArg),
      activeSidePanel(SIDEPANEL_NONE) {
}

void CropController::setupCropPanel() {
    if(cropPanel)
        return;
    cropOverlay = new CropOverlay(viewer);
    cropPanel = new CropPanel(cropOverlay, window);
    connect(cropPanel, &CropPanel::cancel, this, &CropController::hideCropPanel);
    // Apply does NOT close the surface: the applied crop settles into the
    // viewer, the selection becomes the whole new image, and the user is
    // still inside — re-cropping is dragging again. Esc is the only exit.
    connect(cropPanel, &CropPanel::crop, this, &CropController::onPanelCrop);
    connect(cropPanel, &CropPanel::cropAndSave, this, &CropController::hideCropPanel);
    connect(cropPanel, &CropPanel::cropAndSave, this, &CropController::cropAndSaveRequested);
}

void CropController::onPanelCrop(QRect rect) {
    cropJustApplied = true;
    emit cropRequested(rect);
}

void CropController::showCropPanel() {
    if(window->currentViewMode() == MODE_FOLDERVIEW)
        return;

    // the crop wiring is live from initActions, but the panel is built in
    // setupFullUi — which Core schedules 50ms after showGui. An early trigger
    // used to dereference the null pair (cropPanelSurvivesAnEarlyTrigger).
    if(!cropPanel)
        setupCropPanel();

    if(activeSidePanel != SIDEPANEL_CROP) {
        docWidget->hideFloatingPanel();
        sidePanel->setWidget(cropPanel);
        sidePanel->show();
        cropOverlay->show();
        // keyboard focus defines the input context: with the overlay focused,
        // its own Esc/Enter keys work instead of falling through to the
        // ActionManager (where Esc would exit fullscreen / quit)
        cropOverlay->setFocus();
        activeSidePanel = SIDEPANEL_CROP;
        // the user's zoom is the frame they picked — it stays. updateCropPanelData
        // pushes the viewer's geometry (scale, draw rect, real size) into the
        // overlay, and while cropping the zoom/scroll relays are physically
        // disconnected (setInteractionEnabled), so the pushed geometry cannot
        // go stale. The old fit window here was a reset disguised as a guard.
        viewer->setCanvasInputPolicy(CanvasInputPolicy::Framing);
        // feed the panel current image info
        updateCropPanelData();
    }
}

bool CropController::isCropPanelActive() const {
    return activeSidePanel == SIDEPANEL_CROP;
}

void CropController::trigger() {
    if(activeSidePanel != SIDEPANEL_CROP)
        showCropPanel();
    else
        hideCropPanel();
}

void CropController::hideCropPanel() {
    sidePanel->hide();
    if(activeSidePanel == SIDEPANEL_CROP) {
        cropOverlay->hide();
        viewer->setCanvasInputPolicy(CanvasInputPolicy::All);
    }
    activeSidePanel = SIDEPANEL_NONE;
}

void CropController::updateCropPanelData() {
    if(cropPanel && activeSidePanel == SIDEPANEL_CROP) {
        // the overlay pulls its geometry from the viewer itself; the panel's
        // size readout is a label, so it still gets pushed
        cropPanel->setImageRealSize(viewer->sourceSize());
        if(cropJustApplied) {
            // the applied crop just settled into the viewer: the selection
            // becomes the whole new image — visually clean (the tint is
            // fully subtracted), and the next drag starts from "here is
            // everything you have now"
            cropJustApplied = false;
            cropOverlay->selectAll();
        } else {
            cropOverlay->onImageChanged();
        }
    }
}
