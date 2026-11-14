#pragma once

#include <QObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMap>
#include <QSet>
#include <QDebug>
#include <QStringList>
#include "utils/actions.h"
#include "shortcutbuilder.h"
#include "components/scriptmanager/scriptmanager.h"
#include "settings.h"

enum ActionType {
    ACTION_INVALID,
    ACTION_NORMAL,
    ACTION_SCRIPT
};

// Which surface a key event arrives at. A scope is a binding table layered
// over the global one: the same physical key may mean different things per
// surface (Right pages in the document view, moves the grid selection in the
// folder view), and one flat table cannot hold both.
//
// The whole keyboard answers to one law, least surprise by focus: keys are
// handled by the surface that holds it — the focused one owns its keys,
// yields the rest to the action system explicitly, and ranks above any
// state-armed scope. Inside that law every floating surface answers Esc the
// same way (close this layer — the flavors differ, the outcome does not),
// and a disk write never sits on a bare key: in-memory answers only, the
// save chords (Ctrl+S, Ctrl+Shift+S) and clicks do the writing.
enum class ShortcutScope {
    None,      // no surface declared: the state-armed scopes and the global
               // table decide
    FolderGrid // the folder view's thumbnail grid holds focus; its bindings
               // win for this event — the surface being typed at is the
               // freshest intent, fresher than a mode armed earlier
};

// A floating surface that does not own a key yields it to the action system —
// that is the overlay idiom, and it is what keeps zoom, fullscreen and the
// save chords alive while a panel is open. The exception is the class of
// actions that change WHICH FILE is being acted on: a copy-target menu whose
// Page Down swapped the image no longer says which file it copies, and a
// crop whose Page Down swapped the image has already thrown the selection
// away (CropOverlay::onImageChanged clears it).
//
// The distinction is by action, not by key: bindings are user-editable, and a
// key that changed meaning as the user remapped it would silently reopen the
// hole. The list is intentionally static — it is a property of what each
// action does, not of the state the app is in.
bool isSubjectChangingAction(const QString &actionName);

class ActionManager : public QObject {
    Q_OBJECT
  public:
    static ActionManager *getInstance();
    ~ActionManager() override;
    bool processEvent(QInputEvent *, ShortcutScope scope = ShortcutScope::None);
    // the keymap's resolution primitive: shortcut string -> action dispatch,
    // walking caller scope -> armed scopes -> global. Public so the input
    // funnel's decision itself is testable (ShortcutBuilder's physical event
    // mapping is covered separately in machinery_tests).
    bool invokeActionForShortcut(const QString &shortcut, ShortcutScope scope = ShortcutScope::None);
    // arms/disarms the inspect scope: while armed, the inspect layer's
    // bindings override the base ones — in fit original / free zoom the
    // arrow keys pan the canvas and never change images (mode = intent)
    void setInspectMode(bool inspect);
    void addShortcut(const QString &keys, const QString &action);
    void resetDefaults();
    QString actionForShortcut(const QString &keys);
    // A floating surface's yield test, in one place so the four of them cannot
    // drift: "does this event name a subject-changing action?" — true means the
    // surface must consume the event instead of forwarding it.
    bool eventIsSubjectChanging(QInputEvent *event);
    const QString shortcutForAction(const QString &action);
    const QList<QString> shortcutsForAction(const QString &action);
    QStringList actionList();
    const QMap<QString, QString> &allShortcuts();
    void removeShortcut(const QString &keys);
    void removeAllShortcuts();
    void removeAllShortcuts(const QString &actionName);
    QString keyForNativeScancode(quint32 scanCode);
    void saveShortcuts();
    // the folder grid's scope layer, same shape as the global one
    void addGridShortcut(const QString &keys, const QString &action);
    const QMap<QString, QString> &allGridShortcuts();
    void removeAllGridShortcuts();
    void removeAllGridShortcuts(const QString &actionName);
    void saveGridShortcuts();
    // the inspect scope's user override layer: defaults seeded from
    // inspectDefaults, config entries overlay them, same as the global table
    void addInspectShortcut(const QString &keys, const QString &action);
    const QMap<QString, QString> &allInspectShortcuts();
    void removeAllInspectShortcuts();
    void removeAllInspectShortcuts(const QString &actionName);
    void saveInspectShortcuts();
  public slots:
    bool invokeAction(const QString &actionName);

  private:
    explicit ActionManager(QObject *parent = nullptr);
    QMap<QString, QString> defaults, shortcuts; // <shortcut, action>
    // the inspect scope's layer: only the deltas live here (fit original /
    // free zoom pan with the arrows instead of changing images)
    QMap<QString, QString> inspectDefaults, inspectShortcuts; // <shortcut, action>
    bool inspectMode = false;
    // the folder grid's scope layer: defaults mirror the navigation the grid
    // hardcoded before the scope system existed
    QMap<QString, QString> gridDefaults, gridShortcuts; // <shortcut, action>

    void initDefaults();
    void initShortcuts();
    QString modifierKeys(QEvent *event);
    void validateShortcuts(QMap<QString, QString> &map);
    void readShortcuts();
    void readGridShortcuts();
    void readInspectShortcuts();
    bool resolveShortcut(const QString &shortcut, ShortcutScope scope);
    ActionType validateAction(const QString &actionName);

  signals:
    void open();
    void save();
    void saveAs();
    void openSettings();
    void crop();
    void setWallpaper();
    void nextImage();
    void prevImage();
    void fitWindow();
    void fitWidth();
    void fitNormal();
    void flipH();
    void flipV();
    void nextFitMode();
    void toggleFitMode();
    void toggleFullscreen();
    void scrollUp();
    void scrollDown();
    void scrollLeft();
    void scrollRight();
    void zoomIn();
    void zoomOut();
    void zoomInCursor();
    void zoomOutCursor();
    void resize();
    void rotateLeft();
    void rotateRight();
    void exit();
    void removeFile();
    void copyFile();
    void moveFile();
    void closeFullScreenOrExit();
    void jumpToFirst();
    void jumpToLast();
    void folderView();
    void documentView();
    void runScript(const QString &);
    void pauseVideo();
    void seekVideoForward();
    void seekVideoBackward();
    void frameStep();
    void frameStepBack();
    void toggleFolderView();
    void moveToTrash();
    void reloadImage();
    void copyFileClipboard();
    void copyPathClipboard();
    void renameFile();
    void contextMenu();
    void toggleTransparencyGrid();
    void sortByName();
    void sortByTime();
    void sortBySize();
    void toggleImageInfo();
    void toggleShuffle();
    void toggleScalingFilter();
    void showInDirectory();
    void quickCopy();
    void toggleMute();
    void volumeUp();
    void volumeDown();
    void toggleSlideshow();
    void discardEdits();
    void goUp();
    void nextDirectory();
    void prevDirectory();
    void lockZoom();
    void lockView();
    void print();
    void toggleFullscreenInfoBar();
    void pasteFile();
    // the folder grid's scope actions: the navigation the grid used to
    // hardcode in its keyPressEvent (upstream #365)
    void gridSelectNext();
    void gridSelectPrev();
    void gridSelectAbove();
    void gridSelectBelow();
    void gridPageUp();
    void gridPageDown();
    void gridSelectFirst();
    void gridSelectLast();
    void gridSelectAll();
    void gridOpen();
};

extern ActionManager *actionManager;
