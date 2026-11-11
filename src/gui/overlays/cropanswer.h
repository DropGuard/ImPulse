#pragma once

#include <QKeyEvent>
#include <optional>

// The crop surface's two answers and their keys. The scheme carries one
// house rule: a bare key may only trigger an in-memory change — Apply crops
// the image in memory and the disk stays guarded by the save prompt and its
// chords. A disk write never sits on a bare key; it belongs to the save
// chords (Ctrl+S, Ctrl+Shift+S) and to buttons — and a save chord pressed
// while a selection stands commits that crop first, so "save" always means
// save what I have set up.
//
//   Enter   Apply   (Crop — in memory)
//   Esc     Cancel
//
// Shift+Enter degrades to plain Apply: the retired crop-and-save chord
// forgives into the safe answer rather than doing nothing.
enum class CropAnswer {
    Apply,
    Cancel
};

// The scheme's keys, resolved once for both crop surfaces (the canvas
// overlay and the side panel). nullopt for keys the surface does not own —
// the caller routes those to the action system, the overlay idiom that
// keeps global actions live while crop is up.
inline std::optional<CropAnswer> cropAnswerForKey(const QKeyEvent *event) {
    if(event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)
        return CropAnswer::Apply; // modifier-blind: Shift+Enter applies too, never saves
    if(event->key() == Qt::Key_Escape)
        return CropAnswer::Cancel;
    return std::nullopt;
}
