#pragma once

#include <QList>

// One row moving from oldIndex to newIndex, applied to a selection: the row
// itself follows the move, the rows it passed shift by one. Two consumers —
// ThumbnailView (after physically moving its widgets) and the viewless
// viewless ThumbnailStripProxy (on its buffered state) — and one copy of the rule,
// because a second copy drifts exactly the way duplicated permutation
// arithmetic does.
inline void remapSelectionForRename(QList<int> &selection, int oldIndex, int newIndex) {
    if(oldIndex == newIndex)
        return;
    for(int i = 0; i < selection.count(); i++) {
        if(selection[i] == oldIndex)
            selection[i] = newIndex;
        else if(oldIndex < newIndex && selection[i] > oldIndex && selection[i] <= newIndex)
            selection[i]--;
        else if(newIndex < oldIndex && selection[i] >= newIndex && selection[i] < oldIndex)
            selection[i]++;
    }
}
