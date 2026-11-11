#pragma once

#include <QObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMap>
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
enum class ShortcutScope {
    None,      // no surface declared: the state-armed scopes and the global
               // table decide
    FolderGrid // the folder view's thumbnail grid holds focus; its bindings
               // win for this event — the surface being typed at is the
               // freshest intent, fresher than a mode armed earlier
};

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
};

extern ActionManager *actionManager;
