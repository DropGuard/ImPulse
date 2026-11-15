#include "actionmanager.h"

#include <QShortcut>
#include <functional>
#include <QWidget>

ActionManager *actionManager = nullptr;

namespace {
// tokens QKeySequence cannot represent: the pointer vocabulary stays on the
// manual funnel, and a table entry carrying one is display-only
bool isPointerToken(const QString &token) {
    return token == QStringLiteral("WheelUp") || token == QStringLiteral("WheelDown") ||
           token == QStringLiteral("LMB") || token == QStringLiteral("RMB") ||
           token == QStringLiteral("MiddleButton") || token == QStringLiteral("XButton1") ||
           token == QStringLiteral("XButton2") || token.endsWith(QStringLiteral("_DoubleClick"));
}

// one keyboard token -> its QKeySequence claims. "Enter" is the one InputMap
// name that does not map 1:1: Qt reads the main-row Enter as Return and keeps
// Enter for the keypad, and both scancodes resolve to the same token today —
// so the token claims both sequences. Declared here because the disjointness
// law (scopesClaiming) compares claims by expanded sequence, not by spelling.
QList<QKeySequence> sequencesForToken(const QString &token) {
    if(token == QStringLiteral("Enter"))
        return {QKeySequence(QStringLiteral("Return")), QKeySequence(QStringLiteral("Enter"))};
    QKeySequence seq = QKeySequence::fromString(token);
    if(seq.count() == 0)
        return {}; // not a Qt name: the vocabulary sweep
                   // (everyInputMapNameIsQtParseable) keeps this branch empty
    return {seq};
}
} // namespace

ActionManager::ActionManager(QObject *parent) : QObject(parent) {
}
//------------------------------------------------------------------------------
ActionManager::~ActionManager() = default;
//------------------------------------------------------------------------------
ActionManager *ActionManager::getInstance() {
    if(!actionManager) {
        actionManager = new ActionManager();
        // the registry first: initShortcuts' config read validates every
        // stored entry against it, so it must exist before the first
        // validateAction — the ordering main.cpp used to buy by
        // constructing appActions ahead of this singleton
        actionManager->initActions();
        // the two initializers are instance members now: they fill this
        // object's own maps instead of reaching for the global that names it
        actionManager->initDefaults();
        actionManager->initShortcuts();
        actionManager->applyKeyboardProjection();
    }
    return actionManager;
}
//------------------------------------------------------------------------------
// The registry, written out: one stateless QAction per name, in the order the
// old Actions registry kept them. A name here without a connectAction in
// core.cpp is an action the keymap can bind and invoke that does nothing —
// exactly the silence R12 reads these two lists against each other for.
void ActionManager::initActions() {
    addAction("nextImage");
    addAction("prevImage");
    addAction("toggleFullscreen");
    addAction("fitWindow");
    addAction("fitWidth");
    addAction("fitNormal");
    addAction("nextFitMode");
    addAction("toggleFitMode");
    addAction("resize");
    addAction("rotateRight");
    addAction("rotateLeft");
    addAction("scrollUp");
    addAction("scrollDown");
    addAction("scrollLeft");
    addAction("scrollRight");
    addAction("zoomIn");
    addAction("zoomOut");
    addAction("zoomInCursor");
    addAction("zoomOutCursor");
    addAction("open");
    addAction("save");
    addAction("saveAs");
    addAction("setWallpaper");
    addAction("crop");
    addAction("removeFile");
    addAction("copyFile");
    addAction("moveFile");
    addAction("jumpToFirst");
    addAction("jumpToLast");
    addAction("openSettings");
    addAction("closeFullScreenOrExit");
    addAction("exit");
    addAction("flipH");
    addAction("flipV");
    addAction("folderView");
    addAction("pauseVideo");
    addAction("frameStep");
    addAction("frameStepBack");
    addAction("documentView");
    addAction("toggleFolderView");
    addAction("moveToTrash");
    addAction("reloadImage");
    addAction("copyFileClipboard");
    addAction("copyPathClipboard");
    addAction("renameFile");
    addAction("contextMenu");
    addAction("toggleTransparencyGrid");
    addAction("sortByName");
    addAction("sortByTime");
    addAction("sortBySize");
    addAction("toggleImageInfo");
    addAction("toggleShuffle");
    addAction("toggleScalingFilter");
    addAction("toggleMute");
    addAction("volumeUp");
    addAction("volumeDown");
    addAction("toggleSlideshow");
    addAction("showInDirectory");
    addAction("quickCopy");
    addAction("goUp");
    addAction("discardEdits");
    addAction("nextDirectory");
    addAction("prevDirectory");
    addAction("seekVideoForward");
    addAction("seekVideoBackward");
    addAction("lockZoom");
    addAction("lockView");
    addAction("print");
    addAction("toggleFullscreenInfoBar");
    addAction("pasteFile");
    // the folder grid's scope actions — the fork's own line
    addAction("gridSelectNext");
    addAction("gridSelectPrev");
    addAction("gridSelectAbove");
    addAction("gridSelectBelow");
    addAction("gridPageUp");
    addAction("gridPageDown");
    addAction("gridSelectFirst");
    addAction("gridSelectLast");
    addAction("gridSelectAll");
    addAction("gridOpen");
}

QAction *ActionManager::addAction(const QString &name) {
    if(mActions.contains(name))
        qFatal("addAction: '%s' is already registered", qPrintable(name));
    auto *a = new QAction(this);
    a->setObjectName(name);
    // every projected shortcut is application-wide: the actions live on
    // ActionManager, not in any window's subtree, so a WindowShortcut context
    // could never resolve
    a->setShortcutContext(Qt::ApplicationShortcut);
    mActions.insert(name, a);
    return a;
}
//------------------------------------------------------------------------------
// The keyboard projection. attachWindowSurface/attachGridSurface/setGridActive
// and every table mutation funnel through applyKeyboardProjection: the tables
// are re-read and the Qt-side claims (QAction shortcut lists, grid QShortcuts)
// are rebuilt to match.
void ActionManager::attachWindowSurface(QWidget *surface) {
    // a QAction's shortcuts are inert until the action is added to a widget
    // (pinned in dispatch_probe_tests) — the window is that widget
    if(windowSurface == surface)
        return;
    // detach: take the actions back off the old window before the pointer is
    // replaced. A QAction's shortcut is live exactly while it is added to a
    // widget, so leaving them on a still-alive surface would keep them firing
    // after the new window attached — the same one-owner rule as everything
    // else in this tree.
    if(windowSurface) {
        for(QAction *a : std::as_const(mActions))
            windowSurface->removeAction(a);
    }
    windowSurface = surface;
    if(!windowSurface) {
        applyKeyboardProjection();
        return;
    }
    for(QAction *a : std::as_const(mActions))
        windowSurface->addAction(a);
    // The same destroyed hook the grid and document surfaces carry. It is
    // not a crash fix today — ActionManager is a never-destructed singleton,
    // so the dangling pointer is only ever compared, never dereferenced, and
    // ASan has nothing to report. It is the implicit context removed: the
    // safety of this pointer rested on two unstated assumptions, "the
    // singleton outlives every window" and "the next Core re-attaches before
    // anyone reads it". The guard makes it explicit, and the guard matters:
    // an unconditional clear would erase a NEWER surface's pointer when an
    // OLDER window dies late.
    connect(windowSurface, &QObject::destroyed, this, [this](QObject *dead) {
        if(windowSurface != dead)
            return; // a newer surface already replaced this one
        windowSurface = nullptr;
        applyKeyboardProjection();
    });
    applyKeyboardProjection();
}
//------------------------------------------------------------------------------
void ActionManager::attachGridSurface(QWidget *surface) {
    if(gridSurface == surface)
        return;
    gridSurface = surface;
    if(gridSurface) {
        // the surface dies with the window it belongs to; its QShortcuts are
        // its children, so the projections die with it and the stale pointer
        // is cleaned up here rather than in a destructor nobody owns
        connect(gridSurface, &QObject::destroyed, this, [this](QObject *dead) {
            if(gridSurface != dead)
                return; // a newer surface already replaced this one
            gridSurface = nullptr;
            gridProjections.clear();
            gridActive = false;
            applyKeyboardProjection();
        });
    }
    applyKeyboardProjection();
}
//------------------------------------------------------------------------------
void ActionManager::attachDocumentSurface(QWidget *surface) {
    if(documentSurface == surface)
        return;
    documentSurface = surface;
    if(documentSurface) {
        connect(documentSurface, &QObject::destroyed, this, [this](QObject *dead) {
            if(documentSurface != dead)
                return; // a newer surface already replaced this one
            documentSurface = nullptr;
            documentProjections.clear();
            inspectProjections.clear(); // both claim sets die with the surface
            applyKeyboardProjection();
        });
    }
    applyKeyboardProjection();
}

void ActionManager::setGridActive(bool active) {
    if(gridActive == active)
        return;
    gridActive = active;
    // No projection rebuild. gridActive is consumed by the RESOLVER
    // (actionForKeySequence), not by any projection function: the grid's
    // QShortcuts are gated by Qt's own WidgetWithChildrenShortcut context on
    // the focused subtree, and the global projection has carried no grid
    // token since the view split made the tables disjoint. Rebuilding here
    // was the de-claim era's leftover — it deleted and recreated every
    // QShortcut on a focus change that changed nothing.
}
//------------------------------------------------------------------------------
// The one write path. See the declaration comment: a writer hands commit()
// a lambda that edits the table it was given, and commit() owns the
// follow-ups. Enforcing disjointness lives HERE, not at each call site, so
// a scope edit that collides with the global table is caught whether it came
// from the editor, a config load, or a test.
// one erase-all-by-action, shared by the four removeAll*Shortcuts(action)
// doors — the loop shape lived four times, which is how one would eventually
// disagree with the others
void ActionManager::eraseAction(QMap<QString, QString> &table, const QString &actionName) {
    for(auto i = table.begin(); i != table.end();) {
        if(i.value() == actionName)
            i = table.erase(i);
        else
            ++i;
    }
}

void ActionManager::commit(Scope scope, const TableMutator &mutate) {
    // the mutable table reference never leaves this function: it is created,
    // handed to the mutator, and gone. There is no accessor returning a
    // writable table, so a writer cannot edit a table and skip the law or
    // the projection rebuild — that combination is unwritable, not just
    // discouraged.
    QMap<QString, QString> *table = &shortcuts;
    switch(scope) {
    case Scope::Global: table = &shortcuts; break;
    case Scope::Document: table = &docShortcuts; break;
    case Scope::Grid: table = &gridShortcuts; break;
    case Scope::Inspect: table = &inspectShortcuts; break;
    }
    mutate(*table);
    // the law applies to every commit: a global edit can introduce a token a
    // scope owns, and a scope edit can introduce a token the global table
    // still holds — either direction is the same ambiguity
    enforceDisjointness();
    applyKeyboardProjection();
}
//------------------------------------------------------------------------------
const QMap<QString, QString> &ActionManager::tableFor(Scope scope) const {
    switch(scope) {
    case Scope::Global: return shortcuts;
    case Scope::Document: return docShortcuts;
    case Scope::Grid: return gridShortcuts;
    case Scope::Inspect: return inspectShortcuts;
    }
    return shortcuts;
}
//------------------------------------------------------------------------------
// The one-key-one-surface law on the live tables: a token a scope table owns
// is removed from the global table. The global table's claims are always
// live, so a shared token is ambiguous in that scope with neither claim
// firing. Pointer tokens are exempt (they never ride QShortcutMap).
bool ActionManager::enforceDisjointness() {
    bool removed = false;
    for(auto i = shortcuts.begin(); i != shortcuts.end();) {
        bool claimed = false;
        if(!isPointerToken(i.key()))
            claimed = !scopesClaiming(i.key()).isEmpty();
        if(claimed) {
            i = shortcuts.erase(i);
            removed = true;
        } else {
            ++i;
        }
    }
    return removed;
}
//------------------------------------------------------------------------------
// The read side of the disjointness law: which scope tables claim this token,
// by expanded sequence ("Enter" claims both Return and keypad Enter). ONE
// implementation — the conflict checker, the editor warning, and
// enforceDisjointness all ask here, so a fifth scope added later is visible
// to all of them by construction.
QStringList ActionManager::scopesClaiming(const QString &token) const {
    QStringList claims;
    if(isPointerToken(token))
        return claims;
    const QList<QKeySequence> seqs = sequencesForToken(token);
    struct ScopeTable {
        Scope scope;
        const QMap<QString, QString> *table;
        const char *label;
    };
    const ScopeTable tables[] = {
        {Scope::Document, &docShortcuts, "document"},
        {Scope::Grid, &gridShortcuts, "folder view"},
        {Scope::Inspect, &inspectShortcuts, "inspect"},
    };
    for(const ScopeTable &entry : tables) {
        bool found = false;
        for(auto it = entry.table->constBegin(); it != entry.table->constEnd() && !found; ++it) {
            if(isPointerToken(it.key()))
                continue;
            const QList<QKeySequence> tokenSeqs = sequencesForToken(it.key());
            for(const QKeySequence &seq : tokenSeqs) {
                if(seqs.contains(seq)) {
                    found = true;
                    break;
                }
            }
        }
        if(found)
            claims << QString::fromLatin1(entry.label);
    }
    return claims;
}

// The public face of the same law, for the settings editor's conflict
// warning: a key may legitimately live in several scopes, so the editor names
// each table holding it. This is the ONE walk over the scope tables — the
// hand-written three-table version it replaced had forgotten the document
// table, which is how a document-scope collision showed no warning.
QStringList ActionManager::scopeClaimsForToken(const QString &token) const {
    return scopesClaiming(token);
}
//------------------------------------------------------------------------------
void ActionManager::applyKeyboardProjection() {
    rebuildGridShortcuts();
    rebuildDocumentShortcuts();
    reproject();
}
//------------------------------------------------------------------------------
// The global table's projection onto the registered QActions. What is left
// here is small on purpose: the keyboard's view-specific keys are not in this
// table at all any more, they ride the document surface's and the grid's own
// QShortcuts. So there is nothing to de-claim — a key belongs to exactly one
// surface by construction, and Qt's context rules do the rest. That
// construction is not a hope: R12c keeps the default tables disjoint, and
// dropGlobalEntriesClaimedByScopes holds the line against config load and
// editor inserts.
void ActionManager::reproject() {
    QMap<QString, QList<QKeySequence>> projected;
    for(auto it = shortcuts.constBegin(); it != shortcuts.constEnd(); ++it) {
        if(isPointerToken(it.key()))
            continue; // pointer tokens never ride QShortcutMap
        projected[it.value()].append(sequencesForToken(it.key()));
    }
    // every registered action gets its projected list — an action with no
    // keyboard claim gets an empty one, clearing stale sequences
    for(auto it = mActions.constBegin(); it != mActions.constEnd(); ++it)
        it.value()->setShortcuts(projected.value(it.key()));
}
//------------------------------------------------------------------------------
void ActionManager::rebuildGridShortcuts() {
    qDeleteAll(gridProjections);
    gridProjections.clear();
    if(!gridSurface)
        return;
    for(auto it = gridShortcuts.constBegin(); it != gridShortcuts.constEnd(); ++it) {
        if(isPointerToken(it.key()))
            continue; // pointer tokens never ride QShortcutMap
        const QList<QKeySequence> seqs = sequencesForToken(it.key());
        for(const QKeySequence &seq : seqs) {
            auto *sc = new QShortcut(seq, gridSurface);
            sc->setContext(Qt::WidgetWithChildrenShortcut);
            const QString actionName = it.value();
            connect(sc, &QShortcut::activated, this, [this, actionName]() {
                invokeAction(actionName);
            });
            gridProjections.append(sc);
        }
    }
}
//------------------------------------------------------------------------------
// The document view's set, plus the inspect layer's own claims. The inspect
// layer does not sit ABOVE this table, it is a variant OF it: fit original /
// free zoom says "I am inspecting this image", and the arrows pan the canvas
// instead of changing images. That is the same view in a different mode.
//
// The two claim sets are built side by side but stay separate lists, because
// the way they hand over changed: arming is now an enabled-bit flip
// (applyInspectProjection), not a rebuild. A rebuild per fit-mode change was
// deleting and recreating the whole document projection to express what is
// one boolean of routing.
void ActionManager::rebuildDocumentShortcuts() {
    for(const SurfaceClaim &claim : std::as_const(documentProjections))
        delete claim.shortcut;
    documentProjections.clear();
    for(const SurfaceClaim &claim : std::as_const(inspectProjections))
        delete claim.shortcut;
    inspectProjections.clear();
    if(!documentSurface)
        return;

    for(auto it = docShortcuts.constBegin(); it != docShortcuts.constEnd(); ++it) {
        if(isPointerToken(it.key()))
            continue; // pointer tokens never ride QShortcutMap
        const QList<QKeySequence> seqs = sequencesForToken(it.key());
        for(const QKeySequence &seq : seqs) {
            auto *sc = new QShortcut(seq, documentSurface);
            sc->setContext(Qt::WidgetWithChildrenShortcut);
            sc->setEnabled(false); // applyInspectProjection decides; arming was deferred to it
            const QString actionName = it.value();
            connect(sc, &QShortcut::activated, this, [this, actionName]() {
                invokeAction(actionName);
            });
            documentProjections.append({sc, it.key()});
        }
    }
    for(auto it = inspectShortcuts.constBegin(); it != inspectShortcuts.constEnd(); ++it) {
        if(isPointerToken(it.key()))
            continue;
        const QList<QKeySequence> seqs = sequencesForToken(it.key());
        for(const QKeySequence &seq : seqs) {
            auto *sc = new QShortcut(seq, documentSurface);
            sc->setContext(Qt::WidgetWithChildrenShortcut);
            sc->setEnabled(false);
            const QString actionName = it.value();
            connect(sc, &QShortcut::activated, this, [this, actionName]() {
                invokeAction(actionName);
            });
            inspectProjections.append({sc, it.key()});
        }
    }
    // the bits are set once, here, so the projection and inspectMode agree the
    // moment the objects exist — no window where both sets or neither is live
    applyInspectProjection();
}
//------------------------------------------------------------------------------
// Arming and disarming, without touching an object: the inspect claims and the
// document claims they shadow are both already built; only their enabled bits
// move. The law is per TOKEN, not per set, because users can bind any token
// into either table — "disable all of document while inspect is armed" would
// silently take away document keys that inspect does not claim (every
// non-arrow key). Token comparison, not sequence comparison: two tables can
// spell the same physical key differently and still collide.
//
// Order matters for exactly one reason: turn OFF before turning ON, so no key
// is ever live twice, not even between two statements.
void ActionManager::applyInspectProjection() {
    QSet<QString> inspectTokens;
    for(const SurfaceClaim &claim : std::as_const(inspectProjections))
        inspectTokens.insert(claim.token);

    if(inspectMode) {
        // document first: give up every claim the inspect layer is about to take
        for(const SurfaceClaim &claim : std::as_const(documentProjections)) {
            if(inspectTokens.contains(claim.token))
                claim.shortcut->setEnabled(false);
        }
        for(const SurfaceClaim &claim : std::as_const(inspectProjections))
            claim.shortcut->setEnabled(true);
    } else {
        // disarm first, then hand the document's own claims back
        for(const SurfaceClaim &claim : std::as_const(inspectProjections))
            claim.shortcut->setEnabled(false);
        for(const SurfaceClaim &claim : std::as_const(documentProjections))
            claim.shortcut->setEnabled(true);
    }
}
//------------------------------------------------------------------------------
void ActionManager::initDefaults() {
    defaults.insert("F", "toggleFullscreen");
    defaults.insert("F11", "toggleFullscreen");
    // A double click is a mouse GESTURE, not a key: it stays at the global
    // level because fullscreen is the one action with no conflicting
    // gesture-level meaning (a double click on a thumbnail opens it, which is
    // a grid-scope affair). Kept as a token because the keymap editor shows
    // it, but it is not a keyboard binding.
    defaults.insert("LMB_DoubleClick", "toggleFullscreen");
    // MiddleButton->exit is gone. Middle-click is "open in a new tab" /
    // "autoscroll" muscle memory, and quitting the application on a mis-tap
    // loses whatever the user had open. Exit keeps its standard chords.
    defaults.insert(InputMap::keyNameCtrl() + "+O", "open");
    defaults.insert(InputMap::keyNameCtrl() + "+Q", "exit");
    defaults.insert("Esc", "closeFullScreenOrExit");
    defaults.insert("Del", "moveToTrash");
    defaults.insert(InputMap::keyNameShift() + "+Del", "removeFile");
    defaults.insert(InputMap::keyNameCtrl() + "+C", "copyFileClipboard");
    defaults.insert(InputMap::keyNameCtrl() + "+" + InputMap::keyNameShift() + "+C", "copyPathClipboard");
    defaults.insert("F2", "renameFile");
    defaults.insert("Menu", "contextMenu");
    defaults.insert(InputMap::keyNameCtrl() + "+D", "showInDirectory");
    defaults.insert(InputMap::keyNameShift() + "+F", "toggleFullscreenInfoBar");

#ifdef __APPLE__
    defaults.insert(InputMap::keyNameAlt() + "+Up", "zoomIn");
    defaults.insert(InputMap::keyNameAlt() + "+Down", "zoomOut");
    defaults.insert(InputMap::keyNameCtrl() + "+Comma", "openSettings");
#else
    defaults.insert("P", "openSettings");
#endif

    // ---- the document view's keys ----------------------------------------
    // Everything that only means something with one image on screen: the walk,
    // zoom, fit, the edit verbs, and the commands whose result must be visible.
    // These used to live in the global table with `if(viewMode == FOLDERVIEW)
    // return` guards inside each command; a binding that does not exist in the
    // folder view needs no guard, and the guard belonged to the wrong layer.
    docDefaults.insert("Right", "nextImage");
    docDefaults.insert("Left", "prevImage");
    docDefaults.insert("XButton2", "nextImage");
    docDefaults.insert("XButton1", "prevImage");
    docDefaults.insert("WheelDown", "nextImage");
    docDefaults.insert("WheelUp", "prevImage");
    docDefaults.insert("PgUp", "prevImage");
    docDefaults.insert("PgDown", "nextImage");
    docDefaults.insert("Space", "toggleFitMode");
    docDefaults.insert(InputMap::keyNameShift() + "+Space", "nextFitMode");
    docDefaults.insert("1", "fitWindow");
    docDefaults.insert("2", "fitWidth");
    docDefaults.insert("3", "fitNormal");
    docDefaults.insert("R", "resize");
    docDefaults.insert("H", "flipH");
    docDefaults.insert("V", "flipV");
    // copy/move to a chosen directory, via the overlays. Document-scope by
    // what the action needs, not by habit: the target overlay names the file
    // it is about to move, and the folder view has no "the file" to name (it
    // has a selection, and the overlay path reads selectedPath()). The runtime
    // guard that used to skip FOLDERVIEW here is gone with the binding —
    // OverlayHost::triggerCopyOverlay still refuses folder mode as its own
    // contract, but the key no longer reaches it there at all.
    docDefaults.insert("C", "copyFile");
    docDefaults.insert("M", "moveFile");
    // The context menu is ViewerWidget's (ContextMenu is constructed and shown
    // there, and it refuses when the viewer is not visible). The grid's
    // right-click is an unwritten TODO (thumbnailview.cpp: "todo: context
    // menu maybe?"), and its options popup is opened by a toolbar button, not
    // a gesture — so RMB here never reached anything in the folder view.
    docDefaults.insert("RMB", "contextMenu");
    // Ctrl+V opens what the clipboard holds — a URL, or an image buffer saved
    // to a file first. Its effect is loadPath(), i.e. "the file on screen is
    // now that one", which needs a document view to mean anything. In the
    // folder view there is no "the file on screen", so the binding was a
    // global claim reaching a document action.
    docDefaults.insert(InputMap::keyNameCtrl() + "+V", "pasteFile");
    // the toggle pair: keys that LEAVE the document view for the grid. They
    // belong here, not in the global table — the grid's own Enter/Backspace
    // (gridOpen/goUp) are live exactly while the grid holds focus, and a
    // global claim of the same key would be ambiguous there with NEITHER
    // firing (dispatch_probe_tests). R12c holds the tables disjoint; a key
    // belongs to the surface it departs, not the surface it points to.
    docDefaults.insert("Enter", "folderView");
    docDefaults.insert("Backspace", "folderView");
    docDefaults.insert(InputMap::keyNameCtrl() + "+R", "rotateRight");
    docDefaults.insert(InputMap::keyNameCtrl() + "+L", "rotateLeft");
    docDefaults.insert(InputMap::keyNameCtrl() + "+WheelUp", "zoomInCursor");
    docDefaults.insert(InputMap::keyNameCtrl() + "+WheelDown", "zoomOutCursor");
    docDefaults.insert("=", "zoomIn"); // [=+] key on the number row
    docDefaults.insert(InputMap::keyNameCtrl() + "+=", "zoomIn");
    docDefaults.insert("+", "zoomIn");
    docDefaults.insert(InputMap::keyNameCtrl() + "++", "zoomIn");
    docDefaults.insert("-", "zoomOut");
    docDefaults.insert(InputMap::keyNameCtrl() + "+-", "zoomOut");
    docDefaults.insert(InputMap::keyNameCtrl() + "+Down", "zoomOut");
    docDefaults.insert(InputMap::keyNameCtrl() + "+Up", "zoomIn");
    docDefaults.insert("Up", "scrollUp");
    docDefaults.insert("Down", "scrollDown");
    docDefaults.insert(InputMap::keyNameCtrl() + "+S", "save");
    docDefaults.insert(InputMap::keyNameCtrl() + "+" + InputMap::keyNameShift() + "+S", "saveAs");
    // W, not Ctrl+W: the mass-standard close-window chord must not carry a
    // desktop-wide side effect. The context menu keeps the command reachable
    // (contextmenu.cpp), so the default chord only decides what the keyboard
    // does — and bare W sits with the view's other single-letter verbs
    // (R resize, H flip, V flip, X crop, I info).
    docDefaults.insert("W", "setWallpaper");
    docDefaults.insert("X", "crop");
    docDefaults.insert(InputMap::keyNameCtrl() + "+P", "print");
    docDefaults.insert("Home", "jumpToFirst");
    docDefaults.insert("End", "jumpToLast");
    docDefaults.insert(InputMap::keyNameCtrl() + "+Right", "seekVideoForward");
    docDefaults.insert(InputMap::keyNameCtrl() + "+Left", "seekVideoBackward");
    docDefaults.insert(",", "frameStepBack");
    docDefaults.insert(".", "frameStep");
    docDefaults.insert(InputMap::keyNameCtrl() + "+Z", "discardEdits");
    docDefaults.insert(InputMap::keyNameShift() + "+Right", "nextDirectory");
    docDefaults.insert(InputMap::keyNameShift() + "+Left", "prevDirectory");
    docDefaults.insert("`", "toggleSlideshow");
    docDefaults.insert(InputMap::keyNameCtrl() + "+`", "toggleShuffle");
    docDefaults.insert("I", "toggleImageInfo");
    docDefaults.insert("F5", "reloadImage");

    // the inspect scope's defaults are platform-neutral: the arrows are the
    // canvas keys while fit original / free zoom declare an inspection
    inspectDefaults.insert("Left", "scrollLeft");
    inspectDefaults.insert("Right", "scrollRight");

    // the folder grid's scope defaults are the navigation the grid hardcoded
    // before the scope system existed — one translation of that switch, so
    // the default grid behaves byte-identically to the hardcoded era. goUp
    // is the action the switch used to invoke directly; Enter opens the
    // selection rather than toggling the view — the toggle lives in the
    // DOCUMENT table (the view it departs), so neither table carries the
    // other's key: a shared token would be ambiguous while the grid holds
    // focus, with neither claim firing (R12c).
    gridDefaults.insert("Left", "gridSelectPrev");
    gridDefaults.insert("Right", "gridSelectNext");
    gridDefaults.insert("Up", "gridSelectAbove");
    gridDefaults.insert("Down", "gridSelectBelow");
    gridDefaults.insert("PgUp", "gridPageUp");
    gridDefaults.insert("PgDown", "gridPageDown");
    gridDefaults.insert("Home", "gridSelectFirst");
    gridDefaults.insert("End", "gridSelectLast");
    gridDefaults.insert("Enter", "gridOpen");
    gridDefaults.insert("Backspace", "goUp");
    gridDefaults.insert(InputMap::keyNameCtrl() + "+A", "gridSelectAll");
}

//------------------------------------------------------------------------------
void ActionManager::initShortcuts() {
    // defaults are the base layer, user entries overlay them: a config
    // written before a default existed — or carrying only a few entries —
    // still picks the new default up. The old all-or-nothing copy left any
    // config holding at least one entry permanently pinned to its own era.
    shortcuts = defaults;
    gridShortcuts = gridDefaults;
    readShortcuts();
    readGridShortcuts();
    readDocShortcuts();
    readInspectShortcuts();
    // a config saved by an older release carries the pre-split global table,
    // Enter and Backspace included — the disjointness law has to survive
    // config load, not just live in the defaults
    enforceDisjointness();
}

//------------------------------------------------------------------------------
void ActionManager::addShortcut(const QString &keys, const QString &action) {
    ActionType type = validateAction(action);
    if(type == ActionType::ACTION_INVALID)
        return;
    // the editor door: a token a scope table owns never enters here — commit()
    // runs the disjointness law and the projection update, so this site cannot
    // forget either
    commit(Scope::Global, [&](QMap<QString, QString> &t) {
        t.insert(keys, action);
    });
}
//------------------------------------------------------------------------------
void ActionManager::removeShortcut(const QString &keys) {
    commit(Scope::Global, [&](QMap<QString, QString> &t) {
        t.remove(keys);
    });
}
//------------------------------------------------------------------------------
QStringList ActionManager::actionList() {
    return mActions.keys();
}

QAction *ActionManager::action(const QString &name) const {
    return mActions.value(name, nullptr);
}
//------------------------------------------------------------------------------
const QMap<QString, QString> &ActionManager::allShortcuts() {
    return shortcuts;
}
//------------------------------------------------------------------------------
void ActionManager::removeAllShortcuts() {
    commit(Scope::Global, [](QMap<QString, QString> &t) {
        t.clear();
    });
}
//------------------------------------------------------------------------------
// Removes all shortcuts for specified action. Slow (reverse map lookup).
void ActionManager::removeAllShortcuts(const QString &actionName) {
    if(validateAction(actionName) == ActionType::ACTION_INVALID)
        return;
    commit(Scope::Global, [&](QMap<QString, QString> &t) {
        eraseAction(t, actionName);
    });
}
//------------------------------------------------------------------------------
QString ActionManager::keyForNativeScancode(quint32 scanCode) {
    if(inputMap->keys().contains(scanCode)) {
        return inputMap->keys()[scanCode];
    }
    return "";
}
//------------------------------------------------------------------------------
void ActionManager::resetDefaults() {
    // every table through the same door: a table added to the family later
    // cannot be silently forgotten here — a missing commit is a missing line
    // this function's shape makes conspicuous, not a map assignment buried
    // among three correct ones (which is exactly how docShortcuts was missed)
    commit(Scope::Global, [&](QMap<QString, QString> &t) {
        t = defaults;
    });
    commit(Scope::Document, [&](QMap<QString, QString> &t) {
        t = docDefaults;
    });
    commit(Scope::Grid, [&](QMap<QString, QString> &t) {
        t = gridDefaults;
    });
    commit(Scope::Inspect, [&](QMap<QString, QString> &t) {
        t = inspectDefaults;
    });
}

void ActionManager::setDocumentActive(bool active) {
    if(documentActive == active)
        return;
    documentActive = active;
    // Same shape as setGridActive: only the resolver consumes this flag. The
    // document QShortcuts live on DocumentWidget and are gated by its
    // WidgetWithChildrenShortcut context; the projection does not read
    // documentActive, so rebuilding on a mode change was all churn.
}
//------------------------------------------------------------------------------
void ActionManager::setInspectMode(bool inspect) {
    if(inspectMode == inspect)
        return;
    inspectMode = inspect;
    // no object is created or destroyed: the two claim sets are already built
    // on the document surface, and arming is a hand-over of enabled bits per
    // shared token. This was a full qDeleteAll-and-recreate per fit-mode
    // change — the last remaining rebuild triggered by a state flip.
    applyInspectProjection();
}
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void ActionManager::saveShortcuts() {
    settings->saveShortcuts(shortcuts);
}
//------------------------------------------------------------------------------
bool isSubjectChangingAction(const QString &actionName) {
    // everything whose effect is "the file under the user's attention is now a
    // different one". A floating surface that yields these to the action system
    // stops being able to say what it is acting on.
    static const QSet<QString> subjectChanging = {
        // the walk
        QStringLiteral("nextImage"),
        QStringLiteral("prevImage"),
        QStringLiteral("nextDirectory"),
        QStringLiteral("prevDirectory"),
        QStringLiteral("jumpToFirst"),
        QStringLiteral("jumpToLast"),
        QStringLiteral("goUp"),
        // the view the image lives in
        QStringLiteral("folderView"),
        QStringLiteral("documentView"),
        QStringLiteral("toggleFolderView"),
        // the automatic walk
        QStringLiteral("toggleSlideshow"),
        QStringLiteral("toggleShuffle"),
        // the file itself
        QStringLiteral("moveToTrash"),
        QStringLiteral("removeFile"),
        // and the way out
        QStringLiteral("closeFullScreenOrExit"),
        QStringLiteral("exit"),
    };
    return subjectChanging.contains(actionName);
}
//------------------------------------------------------------------------------
QString ActionManager::actionForShortcut(const QString &keys) {
    return shortcuts[keys];
}
//------------------------------------------------------------------------------
// which action the sequence claims in the effective projection: the global
// table minus the focused grid's claims, plus the armed inspect layer. The
// same de-claim law reproject() applies to the Qt-side shortcut lists — the
// veto predicates must answer against what the map will actually fire.
// What a key means right now, across every live table. The order is the
// surfaces' own resolution: a focused grid outranks everything (its keys are
// de-claimed from the document set by focus alone, since the grid's QShortcuts
// belong to the grid's subtree), then the document view's table — the armed
// inspect variant already folded into it — then the global one.
QString ActionManager::actionForKeySequence(const QKeySequence &seq) const {
    QSet<QString> gridClaims;
    if(gridActive && gridSurface) {
        for(auto it = gridShortcuts.constBegin(); it != gridShortcuts.constEnd(); ++it)
            if(!isPointerToken(it.key()))
                gridClaims.insert(it.key());
    }
    if(gridActive && gridSurface) {
        for(auto it = gridShortcuts.constBegin(); it != gridShortcuts.constEnd(); ++it) {
            if(isPointerToken(it.key()))
                continue;
            if(sequencesForToken(it.key()).contains(seq))
                return it.value();
        }
    }
    // the document view's map answers only while its surface is on screen —
    // the same condition that decides whether its QShortcuts are live, so the
    // resolver and the dispatcher cannot disagree
    if(documentActive) {
        QMap<QString, QString> document = docShortcuts;
        if(inspectMode)
            for(auto it = inspectShortcuts.constBegin(); it != inspectShortcuts.constEnd(); ++it)
                if(!isPointerToken(it.key()))
                    document.insert(it.key(), it.value());
        for(auto it = document.constBegin(); it != document.constEnd(); ++it) {
            if(isPointerToken(it.key()))
                continue;
            if(sequencesForToken(it.key()).contains(seq))
                return it.value();
        }
    }
    for(auto it = shortcuts.constBegin(); it != shortcuts.constEnd(); ++it) {
        if(isPointerToken(it.key()) || gridClaims.contains(it.key()))
            continue;
        if(sequencesForToken(it.key()).contains(seq))
            return it.value();
    }
    return QString();
}

bool ActionManager::keyNamesSubjectChangingAction(const QKeyEvent *overrideEvent) const {
    const QKeySequence seq(overrideEvent->key() | overrideEvent->modifiers());
    if(seq.count() == 0)
        return false;
    return isSubjectChangingAction(actionForKeySequence(seq));
}

// The first token bound to this action in a table the surface can be showing
// — used by the context menu to PRINT a hint, so it must answer with a key
// the user can actually press. The document table comes first because most
// of the menu's commands (save, crop, the edit verbs, setWallpaper) moved
// there in the view split; reading only the global table left those items
// with a blank hint (7b20e6e6 moved the bindings and this reader did not
// follow). This menu belongs to ViewerWidget, so it is only ever shown with
// the document scope live. The grid table is NOT read: the grid has its own
// popup (FVOptionsPopup), and its bare navigation tokens would name keys
// whose meaning at that surface is gridOpen/goUp, not this action.
const QString ActionManager::shortcutForAction(const QString &action) {
    const QString doc = docShortcuts.key(action, QString());
    if(!doc.isEmpty())
        return doc;
    const QString inspect = inspectShortcuts.key(action, QString());
    if(!inspect.isEmpty())
        return inspect;
    return shortcuts.key(action, "");
}

// The reverse query a floating surface uses to build its veto list: given an
// action, which tokens does the map dispatch to it? Every caller treats the
// result as a WHITELIST ("let these through" in the rename overlay, "claim
// these back" in the crop overlay), so a longer answer can only make the
// surface yield more, never swallow more. That is why the grid table's
// absence is not a veto hole — but it is also why it must not be added
// blindly: the callers feed these tokens straight to QKeySequence, and the
// grid's bare navigation names would widen a whitelist with keys that mean
// something else at that surface. Add it the day a grid binding becomes a
// subject-changing action the overlays must arbitrate; the veto predicate
// itself (isSubjectChangingAction) is where that decision lives.
const QList<QString> ActionManager::shortcutsForAction(const QString &action) {
    QList<QString> keys = shortcuts.keys(action);
    for(auto it = docShortcuts.constBegin(); it != docShortcuts.constEnd(); ++it)
        if(it.value() == action)
            keys << it.key();
    for(auto it = inspectShortcuts.constBegin(); it != inspectShortcuts.constEnd(); ++it)
        if(it.value() == action && !keys.contains(it.key()))
            keys << it.key();
    return keys;
}

//------------------------------------------------------------------------------
bool ActionManager::invokeAction(const QString &actionName) {
    ActionType type = validateAction(actionName);
    if(type == ActionType::ACTION_NORMAL) {
        // validateAction just proved the lookup succeeds; trigger() on a
        // stateless token is the whole dispatch — the same one-way fire the
        // invokeMethod-by-name dance used to perform
        mActions.value(actionName)->trigger();
        return true;
    } else if(type == ActionType::ACTION_SCRIPT) {
        QString scriptName = actionName;
        scriptName.remove(0, 2); // remove the "s:" prefix
        emit runScript(scriptName);
        return true;
    }
    return false;
}
//------------------------------------------------------------------------------
bool ActionManager::invokeActionForShortcut(const QString &shortcut) {
    if(shortcut.isEmpty())
        return false;
    return resolveShortcut(shortcut);
}

// The resolution order for the events this path still owns — pointer tokens
// only, since the keyboard is QShortcutMap's now: the armed inspect layer
// (fit original / free zoom means "I am inspecting this image", and the
// software does not get to decide the inspection is over), then the global
// table.
//
// There is deliberately no caller-scope rank here any more. The grid's rank
// was "the surface being typed at is the freshest intent", and a focused
// surface now says that through its own QShortcuts plus a ShortcutOverride
// claim — before this code is reached at all. Pointer events carry no focus
// question: a wheel over the grid zooms the image, it does not page the grid.
// A token ("Ctrl+S") resolved across every live table. This is the manual
// funnel's entry: pointer tokens come through here directly, and a surface
// that vetoed a key out of QShortcutMap re-emits it here to get the dispatch
// it suppressed (the crop overlay's save chord is the one case today). Both
// kinds must see the document view's bindings — a save chord is a save chord
// whichever table it lives in.
bool ActionManager::resolveShortcut(const QString &shortcut) {
    if(inspectMode && inspectShortcuts.contains(shortcut))
        return invokeAction(inspectShortcuts[shortcut]);
    if(docShortcuts.contains(shortcut))
        return invokeAction(docShortcuts[shortcut]);
    if(shortcuts.contains(shortcut))
        return invokeAction(shortcuts[shortcut]);
    return false;
}
//------------------------------------------------------------------------------
void ActionManager::validateShortcuts(QMap<QString, QString> &map) {
    for(auto i = map.begin(); i != map.end();) {
        if(validateAction(i.value()) == ActionType::ACTION_INVALID)
            i = map.erase(i);
        else
            ++i;
    }
}
//------------------------------------------------------------------------------
inline ActionType ActionManager::validateAction(const QString &actionName) {
    if(mActions.contains(actionName))
        return ActionType::ACTION_NORMAL;
    if(actionName.startsWith("s:")) {
        QString scriptName = actionName;
        scriptName.remove(0, 2);
        if(scriptManager->scriptExists(scriptName))
            return ActionType::ACTION_SCRIPT;
    }
    return ActionType::ACTION_INVALID;
}
//------------------------------------------------------------------------------
// Tokens renamed when the keyboard vocabulary moved onto Qt's name table
// (the vocabulary sweep pins that every InputMap name is Qt-parseable). Old
// configs carrying the pre-rename spellings are folded forward here — without
// this, a renamed binding would sit in the table, render in the editor, and
// do nothing: the exact silent hole R12b closes on the action side.
static void migrateLegacyTokenNames(QMap<QString, QString> &map) {
    static const QHash<QString, QString> renames = {
        {QStringLiteral("Wake Up"), QStringLiteral("WakeUp")},
        {QStringLiteral("PgBack"), QStringLiteral("Back")},
        {QStringLiteral("PgForward"), QStringLiteral("Forward")},
    };
    for(auto it = renames.constBegin(); it != renames.constEnd(); ++it) {
        if(!map.contains(it.key()))
            continue;
        const QString action = map.take(it.key());
        if(!map.contains(it.value()))
            map.insert(it.value(), action);
    }
}

void ActionManager::readShortcuts() {
    settings->readShortcuts(shortcuts);
    migrateLegacyTokenNames(shortcuts);
    validateShortcuts(shortcuts);
}

void ActionManager::readGridShortcuts() {
    settings->readShortcuts(gridShortcuts, QStringLiteral("gridShortcuts"));
    migrateLegacyTokenNames(gridShortcuts);
    validateShortcuts(gridShortcuts);
}

// the inspect layer's user overrides: defaults first, config entries overlay
// them — the same insert-if-present semantic the global table carries
void ActionManager::readDocShortcuts() {
    docShortcuts = docDefaults;
    settings->readShortcuts(docShortcuts, QStringLiteral("docShortcuts"));
    migrateLegacyTokenNames(docShortcuts);
    validateShortcuts(docShortcuts);
}

void ActionManager::readInspectShortcuts() {
    inspectShortcuts = inspectDefaults;
    settings->readShortcuts(inspectShortcuts, QStringLiteral("inspectShortcuts"));
    migrateLegacyTokenNames(inspectShortcuts);
    validateShortcuts(inspectShortcuts);
}

//------------------------------------------------------------------------------
void ActionManager::addDocShortcut(const QString &keys, const QString &action) {
    ActionType type = validateAction(action);
    if(type == ActionType::ACTION_INVALID)
        return;
    commit(Scope::Document, [&](QMap<QString, QString> &t) {
        t.insert(keys, action);
    });
}
//------------------------------------------------------------------------------
const QMap<QString, QString> &ActionManager::allDocShortcuts() {
    return docShortcuts;
}
//------------------------------------------------------------------------------
void ActionManager::removeAllDocShortcuts() {
    commit(Scope::Document, [](QMap<QString, QString> &t) {
        t.clear();
    });
}
//------------------------------------------------------------------------------
void ActionManager::removeAllDocShortcuts(const QString &actionName) {
    commit(Scope::Document, [&](QMap<QString, QString> &t) {
        eraseAction(t, actionName);
    });
}
//------------------------------------------------------------------------------
void ActionManager::saveDocShortcuts() {
    settings->saveShortcuts(docShortcuts, QStringLiteral("docShortcuts"));
}
//------------------------------------------------------------------------------
void ActionManager::addGridShortcut(const QString &keys, const QString &action) {
    ActionType type = validateAction(action);
    if(type == ActionType::ACTION_INVALID)
        return;
    commit(Scope::Grid, [&](QMap<QString, QString> &t) {
        t.insert(keys, action);
    });
}

const QMap<QString, QString> &ActionManager::allGridShortcuts() {
    return gridShortcuts;
}

void ActionManager::removeAllGridShortcuts() {
    commit(Scope::Grid, [](QMap<QString, QString> &t) {
        t.clear();
    });
}

// Removes all grid shortcuts for specified action. Slow (reverse map lookup).
void ActionManager::removeAllGridShortcuts(const QString &actionName) {
    if(validateAction(actionName) == ActionType::ACTION_INVALID)
        return;
    commit(Scope::Grid, [&](QMap<QString, QString> &t) {
        eraseAction(t, actionName);
    });
}

void ActionManager::saveGridShortcuts() {
    settings->saveShortcuts(gridShortcuts, QStringLiteral("gridShortcuts"));
}

void ActionManager::addInspectShortcut(const QString &keys, const QString &action) {
    ActionType type = validateAction(action);
    if(type == ActionType::ACTION_INVALID)
        return;
    commit(Scope::Inspect, [&](QMap<QString, QString> &t) {
        t.insert(keys, action);
    });
}

const QMap<QString, QString> &ActionManager::allInspectShortcuts() {
    return inspectShortcuts;
}

void ActionManager::removeAllInspectShortcuts() {
    commit(Scope::Inspect, [](QMap<QString, QString> &t) {
        t.clear();
    });
}

// Removes all inspect shortcuts for specified action. Slow (reverse map lookup).
void ActionManager::removeAllInspectShortcuts(const QString &actionName) {
    if(validateAction(actionName) == ActionType::ACTION_INVALID)
        return;
    commit(Scope::Inspect, [&](QMap<QString, QString> &t) {
        eraseAction(t, actionName);
    });
}

void ActionManager::saveInspectShortcuts() {
    settings->saveShortcuts(inspectShortcuts, QStringLiteral("inspectShortcuts"));
}
//------------------------------------------------------------------------------
bool ActionManager::processEvent(QInputEvent *event) {
    return invokeActionForShortcut(ShortcutBuilder::fromEvent(event));
}
//------------------------------------------------------------------------------
