#include "actionmanager.h"

ActionManager *actionManager = nullptr;

ActionManager::ActionManager(QObject *parent) : QObject(parent) {
}
//------------------------------------------------------------------------------
ActionManager::~ActionManager() = default;
//------------------------------------------------------------------------------
ActionManager *ActionManager::getInstance() {
    if(!actionManager) {
        actionManager = new ActionManager();
        // the two initializers are instance members now: they fill this
        // object's own maps instead of reaching for the global that names it
        actionManager->initDefaults();
        actionManager->initShortcuts();
    }
    return actionManager;
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
    }
}
//------------------------------------------------------------------------------
void ActionManager::removeShortcut(const QString &keys) {
    shortcuts.remove(keys);
}
//------------------------------------------------------------------------------
QStringList ActionManager::actionList() {
    return appActions->getList();
}
//------------------------------------------------------------------------------
const QMap<QString, QString> &ActionManager::allShortcuts() {
    return shortcuts;
}
//------------------------------------------------------------------------------
void ActionManager::removeAllShortcuts() {
    shortcuts.clear();
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
}

void ActionManager::setInspectMode(bool inspect) {
    inspectMode = inspect;
}
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void ActionManager::saveShortcuts() {
    settings->saveShortcuts(shortcuts);
}
//------------------------------------------------------------------------------
QString ActionManager::actionForShortcut(const QString &keys) {
    return shortcuts[keys];
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
        QMetaObject::invokeMethod(this, actionName.toLatin1().constData(), Qt::DirectConnection);
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
    if(appActions->getMap().contains(actionName))
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
    }
}

const QMap<QString, QString> &ActionManager::allGridShortcuts() {
    return gridShortcuts;
}

void ActionManager::removeAllGridShortcuts() {
    gridShortcuts.clear();
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
}

void ActionManager::saveGridShortcuts() {
    settings->saveShortcuts(gridShortcuts, QStringLiteral("gridShortcuts"));
}

void ActionManager::addInspectShortcut(const QString &keys, const QString &action) {
    ActionType type = validateAction(action);
    if(type != ActionType::ACTION_INVALID) {
        inspectShortcuts.insert(keys, action);
    }
}

const QMap<QString, QString> &ActionManager::allInspectShortcuts() {
    return inspectShortcuts;
}

void ActionManager::removeAllInspectShortcuts() {
    inspectShortcuts.clear();
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
}

void ActionManager::saveInspectShortcuts() {
    settings->saveShortcuts(inspectShortcuts, QStringLiteral("inspectShortcuts"));
}
//------------------------------------------------------------------------------
bool ActionManager::processEvent(QInputEvent *event, ShortcutScope scope) {
    return invokeActionForShortcut(ShortcutBuilder::fromEvent(event), scope);
}
//------------------------------------------------------------------------------
