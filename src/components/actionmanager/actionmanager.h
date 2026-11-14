#pragma once

#include <QObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMap>
#include <QSet>
#include <QDebug>
#include <QStringList>
#include <QAction>
#include "shortcutbuilder.h"
#include "components/scriptmanager/scriptmanager.h"
#include "settings.h"

class QWidget;
class QShortcut;

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
    // The ShortcutOverride-era form of the same question. An override event
    // carries the LOGICAL key only — nativeScanCode is 0 by construction, so
    // the scancode-token scan above cannot see it. This one keys on the
    // projected sequence instead: true means the key names a subject-changing
    // action in the effective projection (grid de-claims and the armed inspect
    // layer respected), and the surface must claim the override back.
    bool keyNamesSubjectChangingAction(const QKeyEvent *overrideEvent) const;
    // which action (if any) the sequence claims in the effective projection
    QString actionForKeySequence(const QKeySequence &seq) const;
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
    // ---- the registry ----------------------------------------------------
    //
    // Every action is a QAction owned by this object's parent tree: the
    // standard Qt shape for what the hand-declared signal block used to be.
    // The registry's actions are stateless trigger tokens — trigger() fires
    // triggered() into whatever Core connected, and that is the whole
    // contract. QAction's stateful face (checkable, checked, enabled) is
    // deliberately unused: state lives with its owners (WindowShell,
    // ViewerWidget, NavigationController), and mirroring it here is how
    // state-sync hell starts — two truths, written into each other. If a UI
    // element ever needs to reflect state, the QAction becomes that state's
    // single source of truth, one owner, one direction, never two.
    //
    // name -> token. nullptr for an unknown name; dispatch only consults
    // this after validateAction, so the production paths never dereference
    // a miss. The tokens are also why every process that constructs this
    // singleton needs a live QApplication, not a bare QCoreApplication:
    // Qt's QAction constructor resolves its private-class factory through
    // QGuiApplicationPrivate::self, which a QCoreApplication leaves null —
    // the first registration would segfault there.
    QAction *action(const QString &name) const;
    // Registration lives in actionmanager.cpp (initActions), connection in
    // core.cpp — two files that must agree, and a name that reaches here
    // without a registration is a typo the compiler cannot see. The qFatal
    // is the census-in-ctor precedent, Release surviving: every windowed
    // suite's Core construction exercises every one of these connects.
    template <typename Receiver, typename Func>
    void connectAction(const QString &name, const Receiver *receiver, Func slot) {
        QAction *a = action(name);
        if(!a)
            qFatal("connectAction: '%s' is not a registered action", qPrintable(name));
        QObject::connect(a, &QAction::triggered, receiver, slot);
    }
    // ---- the keyboard projection ----------------------------------------
    //
    // The string tables stay the source of truth; QShortcutMap becomes the
    // dispatcher. Keyboard-representable tokens are projected onto the
    // registered QActions (setShortcuts, ApplicationShortcut), so a bound key
    // fires before it ever reaches a widget's keyPressEvent. Pointer tokens
    // (wheel, mouse buttons, double-click) keep the manual funnel — no
    // QKeySequence can represent them.
    //
    // Two empirical Qt facts the projection is built around (pinned in
    // dispatch_probe_tests): an action's shortcuts are INERT until the action
    // is added to a widget (attachWindowSurface), and two live claims of one
    // key are ambiguous with NEITHER firing (no context precedence). The
    // scopes therefore de-claim instead of layering: while the grid holds
    // focus its keys are removed from the global projection, and while the
    // inspect mode is armed its entries replace the global ones. The grid's
    // own bindings ride QShortcuts parented to the grid surface
    // (WidgetWithChildrenShortcut), active exactly while the subtree holds
    // focus.
    void attachWindowSurface(QWidget *surface); // the actions become live on this window
    void attachGridSurface(QWidget *surface);   // the grid scope's QShortcuts (null detaches)
    void setGridActive(bool active);            // the grid surface gained/lost keyboard focus
  public slots:
    bool invokeAction(const QString &actionName);

  private:
    explicit ActionManager(QObject *parent = nullptr);
    // the registry's single registration point: one parented QAction per
    // name. R12 reads this list against core.cpp's connectAction calls.
    QAction *addAction(const QString &name);
    void initActions();
    QMap<QString, QAction *> mActions; // <name, trigger token>

    // the keyboard projection's state and its one apply-everything entry
    // point, called from every table mutation
    QWidget *windowSurface = nullptr;   // where the QActions are registered
    QWidget *gridSurface = nullptr;     // where the grid scope's QShortcuts live
    QList<QShortcut *> gridProjections; // rebuilt whenever the grid table moves
    bool gridActive = false;            // the grid surface holds keyboard focus
    void applyKeyboardProjection();
    void reproject();            // global table (de-claimed) + inspect layer -> setShortcuts
    void rebuildGridShortcuts(); // grid table -> QShortcuts on the grid surface

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
    void validateShortcuts(QMap<QString, QString> &map);
    void readShortcuts();
    void readGridShortcuts();
    void readInspectShortcuts();
    bool resolveShortcut(const QString &shortcut, ShortcutScope scope);
    ActionType validateAction(const QString &actionName);

  signals:
    // the one carrier the registry keeps: script actions are dynamic (any
    // "s:<name>" a user can bind), so there is no QAction to trigger for
    // them — invokeAction strips the prefix and fires this instead. R12
    // documents it as the expected connected-but-unregistered exception.
    void runScript(const QString &);
};

extern ActionManager *actionManager;
