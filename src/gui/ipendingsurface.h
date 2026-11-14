#pragma once

// A surface holding INPUT THE USER HAS TYPED BUT NOT SUBMITTED — the rename
// field's half-typed name, the copy menu's undecided target. While one is
// pending, navigation is locked: the software cannot infer the user's intent,
// and silently carrying or dropping that input is the one outcome the
// contract forbids.
//
// That is a narrower class than "a surface is visible", which is what this
// interface used to answer. Two registrants came off the list:
//
//   SaveConfirmOverlay — it is the projection of an uncommitted edit. The
//     edit state (Image::isEdited) is the lock's condition already, and
//     NavigationController reads it directly; registering the prompt made it
//     answer a question that was already answered.
//
//   CropOverlay — a selection that has not been applied changes nothing, so
//     there is nothing to lose; once applied, Core::crop puts the result in
//     the same Image::isEdited state as every other edit. What the surface
//     does own is the KEYS it must arbitrate, and it claims those through
//     ShortcutOverride like every other floating surface.
//
// So the registry answers one question only, and its two registrants are the
// two surfaces that genuinely have typed-but-unsubmitted input.

class IPendingSurface {
  public:
    virtual ~IPendingSurface() = default;
    // pure virtual on purpose: forgetting to implement it is a compile error
    // at the surface's birth, not a silent ungated hole at the gate
    virtual bool isPending() const = 0;
};
