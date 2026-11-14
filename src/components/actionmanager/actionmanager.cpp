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
void ActionManager::setGridActive(bool active) {
    if(gridActive == active)
        return;
    gridActive = active;
    applyKeyboardProjection();
}
//------------------------------------------------------------------------------
void ActionManager::applyKeyboardProjection() {
    rebuildGridShortcuts();
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
        return {}; // not a Qt name (Linux "Wake Up"/"PgBack"/"PgForward", macOS
                   // enum names): display-only, the manual funnel still answers it
    return {seq};
}
//------------------------------------------------------------------------------
void ActionManager::reproject() {
    // the keys the grid claims while it holds focus come off the global table
    // — two live claims of one key would be ambiguous with NEITHER firing
    // (dispatch_probe_tests), so the scopes de-claim instead of layering
    QSet<QString> gridClaims;
    if(gridActive && gridSurface) {
        for(auto it = gridShortcuts.constBegin(); it != gridShortcuts.constEnd(); ++it)
            if(!isPointerToken(it.key()))
                gridClaims.insert(it.key());
    }
    // the armed inspect layer replaces the global claims for its keys, except
    // where the focused grid outranks it
    QMap<QString, QString> inspectLayer;
    if(inspectMode) {
        for(auto it = inspectShortcuts.constBegin(); it != inspectShortcuts.constEnd(); ++it)
            if(!isPointerToken(it.key()) && !gridClaims.contains(it.key()))
                inspectLayer.insert(it.key(), it.value());
    }
    // per-action shortcut lists over the effective global table
    QMap<QString, QList<QKeySequence>> projected;
    for(auto it = shortcuts.constBegin(); it != shortcuts.constEnd(); ++it) {
        if(isPointerToken(it.key()) || gridClaims.contains(it.key()) || inspectLayer.contains(it.key()))
            continue;
        projected[it.value()].append(sequencesForToken(it.key()));
    }
    for(auto it = inspectLayer.constBegin(); it != inspectLayer.constEnd(); ++it)
        projected[it.value()].append(sequencesForToken(it.key()));
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
void ActionManager::initDefaults() {
    defaults.insert("Right", "nextImage");
    defaults.insert("Left", "prevImage");
    defaults.insert("XButton2", "nextImage");
    defaults.insert("XButton1", "prevImage");
    defaults.insert("WheelDown", "nextImage");
    defaults.insert("WheelUp", "prevImage");
    defaults.insert("F", "toggleFullscreen");
    defaults.insert("F11", "toggleFullscreen");
    defaults.insert("LMB_DoubleClick", "toggleFullscreen");
    defaults.insert("MiddleButton", "exit");
    defaults.insert("Space", "toggleFitMode");
    defaults.insert(InputMap::keyNameShift() + "+Space", "nextFitMode");
    defaults.insert("1", "fitWindow");
    defaults.insert("2", "fitWidth");
    defaults.insert("3", "fitNormal");
    defaults.insert("R", "resize");
    defaults.insert("H", "flipH");
    defaults.insert("V", "flipV");
    defaults.insert(InputMap::keyNameCtrl() + "+R", "rotateRight");
    defaults.insert(InputMap::keyNameCtrl() + "+L", "rotateLeft");
    defaults.insert(InputMap::keyNameCtrl() + "+WheelUp", "zoomInCursor");
    defaults.insert(InputMap::keyNameCtrl() + "+WheelDown", "zoomOutCursor");
    defaults.insert("=", "zoomIn"); // [=+] key on the number row
    defaults.insert(InputMap::keyNameCtrl() + "+=", "zoomIn");
    defaults.insert("+", "zoomIn");
    defaults.insert(InputMap::keyNameCtrl() + "++", "zoomIn");
    defaults.insert("-", "zoomOut");
    defaults.insert(InputMap::keyNameCtrl() + "+-", "zoomOut");
    defaults.insert(InputMap::keyNameCtrl() + "+Down", "zoomOut");
    defaults.insert(InputMap::keyNameCtrl() + "+Up", "zoomIn");
    defaults.insert("Up", "scrollUp");
    defaults.insert("Down", "scrollDown");
    defaults.insert(InputMap::keyNameCtrl() + "+O", "open");
    defaults.insert(InputMap::keyNameCtrl() + "+S", "save");
    defaults.insert(InputMap::keyNameCtrl() + "+" + InputMap::keyNameShift() + "+S", "saveAs");
    defaults.insert(InputMap::keyNameCtrl() + "+W", "setWallpaper");
    defaults.insert("X", "crop");
    defaults.insert(InputMap::keyNameCtrl() + "+P", "print");
    defaults.insert(InputMap::keyNameAlt() + "+X", "exit");
    defaults.insert(InputMap::keyNameCtrl() + "+Q", "exit");
    defaults.insert("Esc", "closeFullScreenOrExit");
    defaults.insert("Del", "moveToTrash");
    defaults.insert(InputMap::keyNameShift() + "+Del", "removeFile");
    defaults.insert("C", "copyFile");
    defaults.insert("M", "moveFile");
    defaults.insert("Home", "jumpToFirst");
    defaults.insert("End", "jumpToLast");
    defaults.insert("PgUp", "prevImage");
    defaults.insert("PgDown", "nextImage");
    defaults.insert(InputMap::keyNameCtrl() + "+Right", "seekVideoForward");
    defaults.insert(InputMap::keyNameCtrl() + "+Left", "seekVideoBackward");
    defaults.insert(",", "frameStepBack");
    defaults.insert(".", "frameStep");
    defaults.insert("Enter", "folderView");
    defaults.insert("Backspace", "folderView");
    defaults.insert("F5", "reloadImage");
    defaults.insert(InputMap::keyNameCtrl() + "+C", "copyFileClipboard");
    defaults.insert(InputMap::keyNameCtrl() + "+" + InputMap::keyNameShift() + "+C", "copyPathClipboard");
    defaults.insert("F2", "renameFile");
    defaults.insert("RMB", "contextMenu");
    defaults.insert("Menu", "contextMenu");
    defaults.insert("I", "toggleImageInfo");
    defaults.insert(InputMap::keyNameCtrl() + "+`", "toggleShuffle");
    defaults.insert(InputMap::keyNameCtrl() + "+D", "showInDirectory");
    defaults.insert("`", "toggleSlideshow");
    defaults.insert(InputMap::keyNameCtrl() + "+Z", "discardEdits");
    defaults.insert(InputMap::keyNameShift() + "+Right", "nextDirectory");
    defaults.insert(InputMap::keyNameShift() + "+Left", "prevDirectory");
    defaults.insert(InputMap::keyNameShift() + "+F", "toggleFullscreenInfoBar");
    defaults.insert(InputMap::keyNameCtrl() + "+V", "pasteFile");

#ifdef __APPLE__
    defaults.insert(InputMap::keyNameAlt() + "+Up", "zoomIn");
    defaults.insert(InputMap::keyNameAlt() + "+Down", "zoomOut");
    defaults.insert(InputMap::keyNameCtrl() + "+Comma", "openSettings");
#else
    defaults.insert("P", "openSettings");
#endif

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
QString ActionManager::actionForKeySequence(const QKeySequence &seq) const {
    QSet<QString> gridClaims;
    if(gridActive && gridSurface) {
        for(auto it = gridShortcuts.constBegin(); it != gridShortcuts.constEnd(); ++it)
            if(!isPointerToken(it.key()))
                gridClaims.insert(it.key());
    }
    for(auto it = shortcuts.constBegin(); it != shortcuts.constEnd(); ++it) {
        if(isPointerToken(it.key()) || gridClaims.contains(it.key()))
            continue;
        if(sequencesForToken(it.key()).contains(seq))
            return it.value();
    }
    if(inspectMode) {
        for(auto it = inspectShortcuts.constBegin(); it != inspectShortcuts.constEnd(); ++it) {
            if(isPointerToken(it.key()) || gridClaims.contains(it.key()))
                continue;
            if(sequencesForToken(it.key()).contains(seq))
                return it.value();
        }
    }
    return QString();
}

bool ActionManager::keyNamesSubjectChangingAction(const QKeyEvent *overrideEvent) const {
    const QKeySequence seq(overrideEvent->key() | overrideEvent->modifiers());
    if(seq.count() == 0)
        return false;
    return isSubjectChangingAction(actionForKeySequence(seq));
}

bool ActionManager::eventIsSubjectChanging(QInputEvent *event) {
    const QString shortcut = ShortcutBuilder::fromEvent(event);
    if(shortcut.isEmpty())
        return false;
    // const lookup: actionForShortcut's operator[] would insert an empty entry
    // for every new key pressed in a surface — a slow leak in a long session
    const auto it = shortcuts.constFind(shortcut);
    return it != shortcuts.constEnd() && isSubjectChangingAction(*it);
}

// returns first shortcut that is found
const QString ActionManager::shortcutForAction(const QString &action) {
    return shortcuts.key(action, "");
}

const QList<QString> ActionManager::shortcutsForAction(const QString &action) {
    return shortcuts.keys(action);
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
bool ActionManager::invokeActionForShortcut(const QString &shortcut, ShortcutScope scope) {
    if(shortcut.isEmpty())
        return false;
    return resolveShortcut(shortcut, scope);
}

// one resolution order for every event: the surface the caller declares wins
// (the surface being typed at is the freshest intent — a grid holding focus
// outranks an inspect mode armed before the folder view was opened), then the
// state-armed scopes (inspect: fit original / free zoom means "I am
// inspecting this image", and the software does not get to decide the
// inspection is over), then the global table.
bool ActionManager::resolveShortcut(const QString &shortcut, ShortcutScope scope) {
    if(scope == ShortcutScope::FolderGrid && gridShortcuts.contains(shortcut))
        return invokeAction(gridShortcuts[shortcut]);
    if(inspectMode && inspectShortcuts.contains(shortcut))
        return invokeAction(inspectShortcuts[shortcut]);
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
void ActionManager::readShortcuts() {
    settings->readShortcuts(shortcuts);
    validateShortcuts(shortcuts);
}

void ActionManager::readGridShortcuts() {
    settings->readShortcuts(gridShortcuts, QStringLiteral("gridShortcuts"));
    validateShortcuts(gridShortcuts);
}

// the inspect layer's user overrides: defaults first, config entries overlay
// them — the same insert-if-present semantic the global table carries
void ActionManager::readInspectShortcuts() {
    inspectShortcuts = inspectDefaults;
    settings->readShortcuts(inspectShortcuts, QStringLiteral("inspectShortcuts"));
    validateShortcuts(inspectShortcuts);
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
bool ActionManager::processEvent(QInputEvent *event, ShortcutScope scope) {
    return invokeActionForShortcut(ShortcutBuilder::fromEvent(event), scope);
}
//------------------------------------------------------------------------------
