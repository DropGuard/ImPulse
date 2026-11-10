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
    connect(cropPanel, &CropPanel::crop, this, &CropController::hideCropPanel);
    connect(cropPanel, &CropPanel::crop, this, &CropController::cropRequested);
    connect(cropPanel, &CropPanel::cropAndSave, this, &CropController::hideCropPanel);
    connect(cropPanel, &CropPanel::cropAndSave, this, &CropController::cropAndSaveRequested);
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
        activeSidePanel = SIDEPANEL_CROP;
        // the user's zoom is the frame they picked — it stays. updateCropPanelData
        // pushes the viewer's geometry (scale, draw rect, real size) into the
        // overlay, and while cropping the zoom/scroll relays are physically
        // disconnected (setInteractionEnabled), so the pushed geometry cannot
        // go stale. The old fit window here was a reset disguised as a guard.
        viewer->setCanvasInputEnabled(false);
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
        viewer->setCanvasInputEnabled(true);
    }
    activeSidePanel = SIDEPANEL_NONE;
}

void CropController::updateCropPanelData() {
    if(cropPanel && activeSidePanel == SIDEPANEL_CROP) {
        cropPanel->setImageRealSize(viewer->sourceSize());
        cropOverlay->setImageDrawRect(viewer->imageRect());
        cropOverlay->setImageScale(viewer->currentScale());
        cropOverlay->setImageRealSize(viewer->sourceSize());
    }
}

void CropController::onWindowResized() {
    if(activeSidePanel == SIDEPANEL_CROP) {
        cropOverlay->setImageScale(viewer->currentScale());
        cropOverlay->setImageDrawRect(viewer->imageRect());
    }
}
