#include "gui/crop/cropcontroller.h"

// the complete State, for the view-mode gate
#include "core.h"

#include "gui/crop/croppanel.h"
#include "gui/crop/cropoverlay.h"
#include "gui/panels/sidepanel/sidepanel.h"
#include "gui/viewers/viewerwidget.h"

CropController::CropController(MW *windowArg, const State *stateArg, ViewerWidget *viewerArg, SidePanel *sidePanelArg,
                               DocumentWidget *docWidgetArg)
    : QObject(windowArg), m_window(windowArg), m_state(stateArg), m_viewer(viewerArg), m_sidePanel(sidePanelArg),
      m_docWidget(docWidgetArg), m_activeSidePanel(SIDEPANEL_NONE) {
}

void CropController::setupCropPanel() {
    if(m_cropPanel)
        return;
    m_cropOverlay = new CropOverlay(m_viewer);
    m_cropPanel = new CropPanel(m_cropOverlay, m_window);
    connect(m_cropPanel, &CropPanel::cancel, this, &CropController::hideCropPanel);
    // Apply does NOT close the surface: the applied crop settles into the
    // viewer, the selection becomes the whole new image, and the user is
    // still inside — re-cropping is dragging again. Esc is the only exit.
    connect(m_cropPanel, &CropPanel::crop, this, &CropController::onPanelCrop);
    connect(m_cropPanel, &CropPanel::cropAndSave, this, &CropController::hideCropPanel);
    connect(m_cropPanel, &CropPanel::cropAndSave, this, &CropController::cropAndSaveRequested);
}

void CropController::onPanelCrop(QRect rect) {
    m_cropJustApplied = true;
    emit cropRequested(rect);
}

void CropController::showCropPanel() {
    if(m_state->viewMode.value() == MODE_FOLDERVIEW)
        return;

    // the crop wiring is live from initActions, but the panel is built in
    // setupFullUi — which Core schedules 50ms after showGui. An early trigger
    // used to dereference the null pair (cropPanelSurvivesAnEarlyTrigger).
    if(!m_cropPanel)
        setupCropPanel();

    if(m_activeSidePanel != SIDEPANEL_CROP) {
        m_docWidget->hideFloatingPanel();
        m_sidePanel->setWidget(m_cropPanel);
        m_sidePanel->show();
        m_cropOverlay->show();
        // keyboard focus defines the input context: with the overlay focused,
        // its own Esc/Enter keys work instead of falling through to the
        // ActionManager (where Esc would exit fullscreen / quit)
        m_cropOverlay->setFocus();
        m_activeSidePanel = SIDEPANEL_CROP;
        // the user's zoom is the frame they picked — it stays. updateCropPanelData
        // pushes the viewer's geometry (scale, draw rect, real size) into the
        // overlay, and while cropping the zoom/scroll relays are physically
        // disconnected (setInteractionEnabled), so the pushed geometry cannot
        // go stale. The old fit window here was a reset disguised as a guard.
        m_viewer->setCanvasInputPolicy(CanvasInputPolicy::Framing);
        // feed the panel current image info
        updateCropPanelData();
    }
}

bool CropController::isActive() const {
    return m_activeSidePanel == SIDEPANEL_CROP;
}

void CropController::toggle() {
    if(m_activeSidePanel != SIDEPANEL_CROP)
        showCropPanel();
    else
        hideCropPanel();
}

void CropController::hideCropPanel() {
    m_sidePanel->hide();
    if(m_activeSidePanel == SIDEPANEL_CROP) {
        m_cropOverlay->hide();
        m_viewer->setCanvasInputPolicy(CanvasInputPolicy::All);
    }
    m_activeSidePanel = SIDEPANEL_NONE;
}

void CropController::updateCropPanelData() {
    if(m_cropPanel && m_activeSidePanel == SIDEPANEL_CROP) {
        // the overlay pulls its geometry from the viewer itself; the panel's
        // size readout is a label, so it still gets pushed
        m_cropPanel->setImageRealSize(m_viewer->sourceSize());
        if(m_cropJustApplied) {
            // the applied crop just settled into the viewer: the selection
            // becomes the whole new image — visually clean (the tint is
            // fully subtracted), and the next drag starts from "here is
            // everything you have now"
            m_cropJustApplied = false;
            m_cropOverlay->selectAll();
        } else {
            m_cropOverlay->onImageChanged();
        }
    }
}
