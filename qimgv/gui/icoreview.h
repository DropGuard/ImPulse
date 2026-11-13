#pragma once

// What Core needs from the window, and nothing more — the display sink, the
// two mode legs, and the visibility query. Deliberately NOT a QObject: MW
// already carries QWidget's QObject, and a second one is forbidden — a pure
// abstract class lets MW implement the interface directly, so production is
// view_ = mw with no adapter in between.
//
// Every method is here because Core calls it, not because MW has it — the
// interface is Core's needs, and the window happens to be one implementation.

#include <QMovie>
#include <QImage>
#include <QString>
#include <memory>

#include "settings.h" // SortingMode, ViewMode

class ICoreView {
  public:
    virtual ~ICoreView() = default;

    // the display sink: guiSetImage's three branches and its teardown.
    // Images travel as QImage — the paint-device conversion happens in the
    // implementation, on the GUI thread, so the model layer and the headless
    // tests never construct a QPixmap.
    virtual void showImage(std::shared_ptr<const QImage> image) = 0;
    virtual void showAnimation(const std::shared_ptr<QMovie> &movie) = 0;
    virtual void showVideo(const QString &file) = 0;
    virtual void closeImage() = 0;
    virtual void repaintView() = 0;
    virtual void setUpdatesEnabled(bool enabled) = 0;

    // window lifecycle bits Core drives
    virtual void showWindowDefault() = 0;
    virtual bool isWindowVisible() = 0;
    virtual void toggleFullscreenInfoBar() = 0;

    // projections Core pushes after mutations
    virtual void onSortingChanged(SortingMode mode) = 0;
    virtual void onScalingFinished(std::unique_ptr<const QImage> scaled) = 0;

    // enterMode's two legs (MW's own names)
    virtual void enableFolderView() = 0;
    virtual void enableDocumentView() = 0;

    // the slideshow's loop state, relayed to the viewer
    virtual void setLoopPlayback(bool mode) = 0;
};
