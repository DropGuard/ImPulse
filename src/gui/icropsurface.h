#pragma once

// What Core needs from the crop surface, and nothing more: the toggle, the
// active query, and the two intent legs. Deliberately NOT a QObject — the
// interface declares methods, and the QObject implementation realizes the
// intent ones as real signals with override (the IDirectoryView idiom, also
// used by ICoreView::setLoopPlayback). Production implements it as
// CropController; a headless test provides a recording fake.

#include <QRect>

class ICropSurface {
  public:
    virtual ~ICropSurface() = default;

    // open the surface if closed, close it if open. Whether opening is
    // allowed at all is split by design: the caller gates on having an image,
    // the surface gates on the view mode.
    virtual void toggle() = 0;
    virtual bool isActive() const = 0;

    // intent: the user applied a crop inside the surface. Declared here as
    // plain methods; CropController realizes them as signals. A connect
    // written against this interface's name does NOT compile (this is not a
    // QObject — the same measurement the DirectoryPresenter documented), so
    // consumers connect against the concrete type while the pointer travels
    // as the interface.
    virtual void cropRequested(QRect rect) = 0;
    virtual void cropAndSaveRequested(QRect rect) = 0;
};
