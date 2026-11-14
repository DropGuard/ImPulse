#pragma once

// A surface that holds unfinished user work: while it is pending, mode
// transitions and navigation are locked — the software cannot infer the
// user's intent, and silently carrying (or dropping) the work is the one
// outcome this contract forbids.
//
// Registered at each surface's setup into OverlayHost's registry; the lock's
// condition is the registry, not a checklist at the gate. imageInfo is the
// known passive exception and is simply not registered.

class IPendingSurface {
  public:
    virtual ~IPendingSurface() = default;
    // pure virtual on purpose: forgetting to implement it is a compile error
    // at the surface's birth, not a silent ungated hole at the gate
    virtual bool isPending() const = 0;
};
