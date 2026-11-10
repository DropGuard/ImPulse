#pragma once

// The crop flow, extracted from MW (the strangler rewrite of mainwindow.cpp):
// the side panel hosting CropPanel, the CropOverlay that tracks the viewer's
// geometry, and the active-panel state. Owns the "lock zoom + gate viewer
// input while cropping" dance. Crop requests are re-emitted for Core.

#include <QObject>

#include "gui/mainwindow.h"

class ViewerWidget;
class SidePanel;
class DocumentWidget;
class CropPanel;
class CropOverlay;

class CropController : public QObject {
    Q_OBJECT
  public:
    explicit CropController(MW *window, ViewerWidget *viewer, SidePanel *sidePanel, DocumentWidget *docWidget);

    bool isCropPanelActive() const;
    void trigger();
    void showCropPanel();
    void hideCropPanel();
    // viewer geometry changed (resize / fit / new image) while cropping
    void updateCropPanelData();
    void onWindowResized();
    // setupFullUi materializes the panel; the controller can also do it
    // on demand (the crop action wiring is live before that)
    void setupCropPanel();

  signals:
    void cropRequested(QRect);
    void cropAndSaveRequested(QRect);

  private:
    MW *window;
    ViewerWidget *viewer;
    SidePanel *sidePanel;
    DocumentWidget *docWidget;
    CropPanel *cropPanel = nullptr;
    CropOverlay *cropOverlay = nullptr;
    ActiveSidePanel activeSidePanel;
};
