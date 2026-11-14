#include "actionmanager.h"

#include <QShortcut>
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
    windowSurface = surface;
    if(!windowSurface)
        return;
    for(QAction *a : std::as_const(mActions))
        windowSurface->addAction(a);
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
        connect(gridSurface, &QObject::destroyed, this, [this]() {
            gridSurface = nullptr;
            gridProjections.clear();
            if(gridActive) {
                gridActive = false;
            }
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
        connect(documentSurface, &QObject::destroyed, this, [this]() {
            documentSurface = nullptr;
            documentProjections.clear();
            applyKeyboardProjection();
        });
    }
    applyKeyboardProjection();
}

void ActionManager::setGridActive(bool active) {
    if(gridActive == active)
        return;
    gridActive = active;
    applyKeyboardProjection();
}
//------------------------------------------------------------------------------
void ActionManager::applyKeyboardProjection() {
    rebuildGridShortcuts();
    rebuildDocumentShortcuts();
    reproject();
}
//------------------------------------------------------------------------------
// one keyboard token -> its QKeySequence claims. "Enter" is the one InputMap
// name that does not map 1:1: Qt reads the main-row Enter as Return and keeps
// Enter for the keypad, and both scancodes resolve to the same token today —
// so the token claims both sequences.
static QList<QKeySequence> sequencesForToken(const QString &token) {
    if(token == QStringLiteral("Enter"))
        return {QKeySequence(QStringLiteral("Return")), QKeySequence(QStringLiteral("Enter"))};
    QKeySequence seq = QKeySequence::fromString(token);
    if(seq.count() == 0)
        return {}; // not a Qt name: the vocabulary sweep
                   // (everyInputMapNameIsQtParseable) keeps this branch empty
    return {seq};
}
//------------------------------------------------------------------------------
// The global table's projection onto the registered QActions. What is left
// here is small on purpose: the keyboard's view-specific keys are not in this
// table at all any more, they ride the document surface's and the grid's own
// QShortcuts. So there is nothing to de-claim — a key belongs to exactly one
// surface by construction, and Qt's context rules do the rest.
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
// The document view's set. Same shape as the grid's, one difference: the armed
// inspect layer does not sit ABOVE this table, it is a variant OF it — fit
// original / free zoom says "I am inspecting this image", and the arrows pan
// the canvas instead of changing images. That is the same view, a different
// mode of it, so it is built here and never competes for a key with the base.
void ActionManager::rebuildDocumentShortcuts() {
    qDeleteAll(documentProjections);
    documentProjections.clear();
    if(!documentSurface)
        return;
    QMap<QString, QString> effective = docShortcuts;
    if(inspectMode) {
        for(auto it = inspectShortcuts.constBegin(); it != inspectShortcuts.constEnd(); ++it)
            if(!isPointerToken(it.key()))
                effective.insert(it.key(), it.value());
    }
    for(auto it = effective.constBegin(); it != effective.constEnd(); ++it) {
        if(isPointerToken(it.key()))
            continue; // pointer tokens never ride QShortcutMap
        const QList<QKeySequence> seqs = sequencesForToken(it.key());
        for(const QKeySequence &seq : seqs) {
            auto *sc = new QShortcut(seq, documentSurface);
            sc->setContext(Qt::WidgetWithChildrenShortcut);
            const QString actionName = it.value();
            connect(sc, &QShortcut::activated, this, [this, actionName]() {
                invokeAction(actionName);
            });
            documentProjections.append(sc);
        }
    }
}
//------------------------------------------------------------------------------
void ActionManager::initDefaults() {
    defaults.insert("F", "toggleFullscreen");
    defaults.insert("F11", "toggleFullscreen");
    defaults.insert("LMB_DoubleClick", "toggleFullscreen");
    defaults.insert("MiddleButton", "exit");
    defaults.insert(InputMap::keyNameCtrl() + "+O", "open");
    defaults.insert(InputMap::keyNameAlt() + "+X", "exit");
    defaults.insert(InputMap::keyNameCtrl() + "+Q", "exit");
    defaults.insert("Esc", "closeFullScreenOrExit");
    defaults.insert("Del", "moveToTrash");
    defaults.insert(InputMap::keyNameShift() + "+Del", "removeFile");
    defaults.insert("C", "copyFile");
    defaults.insert("M", "moveFile");
    defaults.insert("Enter", "folderView");
    defaults.insert("Backspace", "folderView");
    defaults.insert(InputMap::keyNameCtrl() + "+C", "copyFileClipboard");
    defaults.insert(InputMap::keyNameCtrl() + "+" + InputMap::keyNameShift() + "+C", "copyPathClipboard");
    defaults.insert("F2", "renameFile");
    defaults.insert("RMB", "contextMenu");
    defaults.insert("Menu", "contextMenu");
    defaults.insert(InputMap::keyNameCtrl() + "+D", "showInDirectory");
    defaults.insert(InputMap::keyNameShift() + "+F", "toggleFullscreenInfoBar");
    defaults.insert(InputMap::keyNameCtrl() + "+V", "pasteFile");

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
    docDefaults.insert(InputMap::keyNameCtrl() + "+W", "setWallpaper");
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
    // is the global action the switch used to invoke directly; Enter opens
    // the selection rather than toggling the view (the global Enter binding
    // is the toggle, and the grid's capture used to precede it).
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
}
//------------------------------------------------------------------------------
void ActionManager::addShortcut(const QString &keys, const QString &action) {
    ActionType type = validateAction(action);
    if(type != ActionType::ACTION_INVALID) {
        shortcuts.insert(keys, action);
        applyKeyboardProjection();
    }
}
//------------------------------------------------------------------------------
void ActionManager::removeShortcut(const QString &keys) {
    shortcuts.remove(keys);
    applyKeyboardProjection();
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
    shortcuts.clear();
    applyKeyboardProjection();
}
//------------------------------------------------------------------------------
// Removes all shortcuts for specified action. Slow (reverse map lookup).
void ActionManager::removeAllShortcuts(const QString &actionName) {
    if(validateAction(actionName) == ActionType::ACTION_INVALID)
        return;

    for(auto i = shortcuts.begin(); i != shortcuts.end();) {
        if(i.value() == actionName)
            i = shortcuts.erase(i);
        else
            ++i;
    }
    applyKeyboardProjection();
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
    shortcuts = defaults;
    inspectShortcuts = inspectDefaults;
    gridShortcuts = gridDefaults;
    applyKeyboardProjection();
}

void ActionManager::setDocumentActive(bool active) {
    if(documentActive == active)
        return;
    documentActive = active;
    applyKeyboardProjection();
}
//------------------------------------------------------------------------------
void ActionManager::setInspectMode(bool inspect) {
    inspectMode = inspect;
    applyKeyboardProjection(); // the armed layer swaps the projected claims
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

// returns first shortcut that is found
const QString ActionManager::shortcutForAction(const QString &action) {
    return shortcuts.key(action, "");
}

const QList<QString> ActionManager::shortcutsForAction(const QString &action) {
    // every live table: a surface asking "which keys does this action carry?"
    // must see the document view's binding too, or it will veto the wrong
    // keys (the crop overlay's save chord is exactly that question)
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
    if(type != ActionType::ACTION_INVALID) {
        docShortcuts.insert(keys, action);
        applyKeyboardProjection();
    }
}
//------------------------------------------------------------------------------
const QMap<QString, QString> &ActionManager::allDocShortcuts() {
    return docShortcuts;
}
//------------------------------------------------------------------------------
void ActionManager::removeAllDocShortcuts() {
    docShortcuts.clear();
    applyKeyboardProjection();
}
//------------------------------------------------------------------------------
void ActionManager::removeAllDocShortcuts(const QString &actionName) {
    for(auto i = docShortcuts.begin(); i != docShortcuts.end();) {
        if(i.value() == actionName)
            i = docShortcuts.erase(i);
        else
            ++i;
    }
    applyKeyboardProjection();
}
//------------------------------------------------------------------------------
void ActionManager::saveDocShortcuts() {
    settings->saveShortcuts(docShortcuts, QStringLiteral("docShortcuts"));
}
//------------------------------------------------------------------------------
void ActionManager::addGridShortcut(const QString &keys, const QString &action) {
    ActionType type = validateAction(action);
    if(type != ActionType::ACTION_INVALID) {
        gridShortcuts.insert(keys, action);
        applyKeyboardProjection();
    }
}

const QMap<QString, QString> &ActionManager::allGridShortcuts() {
    return gridShortcuts;
}

void ActionManager::removeAllGridShortcuts() {
    gridShortcuts.clear();
    applyKeyboardProjection();
}

// Removes all grid shortcuts for specified action. Slow (reverse map lookup).
void ActionManager::removeAllGridShortcuts(const QString &actionName) {
    if(validateAction(actionName) == ActionType::ACTION_INVALID)
        return;

    for(auto i = gridShortcuts.begin(); i != gridShortcuts.end();) {
        if(i.value() == actionName)
            i = gridShortcuts.erase(i);
        else
            ++i;
    }
    applyKeyboardProjection();
}

void ActionManager::saveGridShortcuts() {
    settings->saveShortcuts(gridShortcuts, QStringLiteral("gridShortcuts"));
}

void ActionManager::addInspectShortcut(const QString &keys, const QString &action) {
    ActionType type = validateAction(action);
    if(type != ActionType::ACTION_INVALID) {
        inspectShortcuts.insert(keys, action);
        applyKeyboardProjection();
    }
}

const QMap<QString, QString> &ActionManager::allInspectShortcuts() {
    return inspectShortcuts;
}

void ActionManager::removeAllInspectShortcuts() {
    inspectShortcuts.clear();
    applyKeyboardProjection();
}

// Removes all inspect shortcuts for specified action. Slow (reverse map lookup).
void ActionManager::removeAllInspectShortcuts(const QString &actionName) {
    if(validateAction(actionName) == ActionType::ACTION_INVALID)
        return;

    for(auto i = inspectShortcuts.begin(); i != inspectShortcuts.end();) {
        if(i.value() == actionName)
            i = inspectShortcuts.erase(i);
        else
            ++i;
    }
    applyKeyboardProjection();
}

void ActionManager::saveInspectShortcuts() {
    settings->saveShortcuts(inspectShortcuts, QStringLiteral("inspectShortcuts"));
}
//------------------------------------------------------------------------------
bool ActionManager::processEvent(QInputEvent *event) {
    return invokeActionForShortcut(ShortcutBuilder::fromEvent(event));
}
//------------------------------------------------------------------------------
