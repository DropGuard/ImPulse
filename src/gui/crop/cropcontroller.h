#pragma once

// The crop flow, extracted from MW (the strangler rewrite of mainwindow.cpp):
// the side panel hosting CropPanel, the CropOverlay that tracks the viewer's
// geometry, and the active-panel state. Owns the "lock zoom + gate viewer
// input while cropping" dance. Crop requests are re-emitted for Core.

#include <QObject>

#include "gui/icropsurface.h"
#include "gui/mainwindow.h"

class ViewerWidget;
class SidePanel;
class DocumentWidget;
class CropPanel;
class CropOverlay;

class CropController : public QObject, public ICropSurface {
    Q_OBJECT
  public:
    explicit CropController(MW *window, const State *state, ViewerWidget *viewer, SidePanel *sidePanel,
                            DocumentWidget *docWidget);

    bool isActive() const override;
    void toggle() override;
    void showCropPanel();
    void hideCropPanel();
    // viewer geometry changed (resize / fit / new image) while cropping
    void updateCropPanelData();
    // setupFullUi materializes the panel; the controller can also do it
    // on demand (the crop action wiring is live before that)
    void setupCropPanel();

  signals:
    // the ICropSurface intent legs, realized as signals (the interface
    // declares them as plain methods; this is the override that makes the
    // realization). Both rects are in IMAGE pixel coordinates — the frame the
    // editor applies ImageLib::cropped against; the surface owns the
    // scene/device conversions and hands the editor pure geometry.
    void cropRequested(QRect rect) override;
    void cropAndSaveRequested(QRect rect) override;

  private slots:
    // arms cropJustApplied, then relays: the panel-data nudge that follows
    // the edit settling answers it by making the selection the whole new
    // image — the surface never closes on Apply, so re-cropping is dragging
    // again and Esc keeps meaning exactly one thing: leave.
    void onPanelCrop(QRect rect);

  private:
    MW *m_window;
    // the one truth for the view-mode gate: the window's centralWidget keeps
    // only a projection of it
    const State *m_state;
    ViewerWidget *m_viewer;
    SidePanel *m_sidePanel;
    DocumentWidget *m_docWidget;
    CropPanel *m_cropPanel = nullptr;
    CropOverlay *m_cropOverlay = nullptr;
    ActiveSidePanel m_activeSidePanel;
    bool m_cropJustApplied = false;
};
