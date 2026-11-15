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

// The keyboard law, least surprise by focus, in Qt's own vocabulary: a
// projected key fires through QShortcutMap before any widget sees it; the
// focused surface claims back (ShortcutOverride veto) the keys it must
// arbitrate — its own workflow keys, and every subject-changing action,
// because a walk that swaps the file under a floating surface destroys what
// that surface is acting on. The manual funnel remains for the events
// QShortcutMap cannot match: pointer tokens (wheel, mouse buttons,
// double-click) and the table entries no QKeySequence can represent. Inside
// that law every floating surface answers Esc the same way (close this
// layer — the flavors differ, the outcome does not), and a disk write never
// sits on a bare key: in-memory answers only, the save chords (Ctrl+S,
// Ctrl+Shift+S) and clicks do the writing.
//
// The binding tables hold one global set and three scopes — the document
// view's, the grid's, and the inspect variant folded into the document's —
// because the same physical key means different things per surface. The
// global set is disjoint from all three (R12c): its claims are always live
// (ApplicationShortcut), so a token it shared with a scope table would be
// ambiguous in that scope with NEITHER claim firing. What the tables do not
// have is a caller-declared rank: the keyboard does not travel through a
// resolve call that a caller annotates. A focused surface ranks by claiming
// its keys back before dispatch (its own QShortcuts plus ShortcutOverride),
// which is why ownership replaces layering.

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
    bool processEvent(QInputEvent *);
    // the keymap's resolution primitive: shortcut string -> action dispatch,
    // walking the armed inspect layer -> global. Public so the pointer
    // funnel's decision itself is testable (ShortcutBuilder's physical event
    // mapping is covered separately in machinery_tests).
    //
    // There is no caller-scope parameter any more: the keyboard no longer
    // comes through this path at all. A focused surface's keys are projected
    // onto QShortcuts and claimed back with ShortcutOverride; what reaches
    // here is a pointer token (wheel, buttons, double-click), which has no
    // surface to rank.
    bool invokeActionForShortcut(const QString &shortcut);
    // arms/disarms the inspect scope: while armed, the inspect layer's
    // bindings override the base ones — in fit original / free zoom the
    // arrow keys pan the canvas and never change images (mode = intent)
    void setInspectMode(bool inspect);
    // The document view is on screen. The same switch decides whether that
    // table's QShortcuts are live AND whether the resolver sees them — one
    // condition, so the two cannot disagree. (The surface's own visibility is
    // not the authority: a headless Core has a document view and no window.)
    void setDocumentActive(bool active);
    void addShortcut(const QString &keys, const QString &action);
    void resetDefaults();
    QString actionForShortcut(const QString &keys);
    // A floating surface's yield test, in one place so the doors cannot
    // drift: "does this key name a subject-changing action?" — true means the
    // surface must claim the key back from QShortcutMap (ShortcutOverride) or
    // consume it on delivery. Keys by sequence: an override event carries the
    // logical key only (nativeScanCode is 0 by construction), and a delivered
    // event answers through the same projected tables the map matched against.
    bool keyNamesSubjectChangingAction(const QKeyEvent *overrideEvent) const;
    // which action (if any) the sequence claims across the live tables —
    // grid, then the document view's (inspect variant folded in), then global
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
    // the document view's scope, same shape as the grid's: the table is the
    // keymap editor's storage and the projection's source
    void addDocShortcut(const QString &keys, const QString &action);
    const QMap<QString, QString> &allDocShortcuts();
    void removeAllDocShortcuts();
    void removeAllDocShortcuts(const QString &actionName);
    void saveDocShortcuts();
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
    void attachWindowSurface(QWidget *surface);   // the actions become live on this window
    void attachGridSurface(QWidget *surface);     // the grid scope's QShortcuts (null detaches)
    void attachDocumentSurface(QWidget *surface); // the document view's QShortcuts (null detaches)
    void setGridActive(bool active);              // the grid surface gained/lost keyboard focus
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
    QWidget *windowSurface = nullptr;       // where the QActions are registered
    QWidget *gridSurface = nullptr;         // where the grid scope's QShortcuts live
    QWidget *documentSurface = nullptr;     // where the document view's QShortcuts live
    QList<QShortcut *> gridProjections;     // rebuilt whenever the grid table moves
    QList<QShortcut *> documentProjections; // rebuilt whenever the document table moves
    bool gridActive = false;                // the grid surface holds keyboard focus
    void applyKeyboardProjection();
    void reproject();                // global table (de-claimed) + inspect layer -> setShortcuts
    void rebuildGridShortcuts();     // grid table -> QShortcuts on the grid surface
    void rebuildDocumentShortcuts(); // document table (+ inspect variant) -> QShortcuts

    QMap<QString, QString> defaults, shortcuts; // <shortcut, action>
    // the inspect scope's layer: only the deltas live here (fit original /
    // free zoom pan with the arrows instead of changing images)
    QMap<QString, QString> inspectDefaults, inspectShortcuts; // <shortcut, action>
    bool inspectMode = false;
    // true from construction: the app starts in the document view (State's own
    // default), and a headless Core may never see a mode change at all
    bool documentActive = true;
    // the folder grid's scope layer: defaults mirror the navigation the grid
    // hardcoded before the scope system existed
    QMap<QString, QString> gridDefaults, gridShortcuts; // <shortcut, action>
    // the document view's scope layer: every key that only means something
    // while one image is on screen — the walk, zoom, fit, the edit verbs and
    // the two commands that need the result to be visible. They used to live
    // in the global table with `if(viewMode == FOLDERVIEW) return` guards
    // inside the commands; a binding that does not exist needs no guard.
    QMap<QString, QString> docDefaults, docShortcuts; // <shortcut, action>

    void initDefaults();
    void initShortcuts();
    // the one-key-one-surface law on the live tables: global entries whose
    // token a scope table claims are erased (they would be ambiguous in the
    // owning scope — two live claims of one key fire nothing). Runs at
    // config load and on every editor insert.
    void dropGlobalEntriesClaimedByScopes();
    void validateShortcuts(QMap<QString, QString> &map);
    void readShortcuts();
    void readGridShortcuts();
    void readInspectShortcuts();
    void readDocShortcuts();
    bool resolveShortcut(const QString &shortcut);
    ActionType validateAction(const QString &actionName);

  signals:
    // the one carrier the registry keeps: script actions are dynamic (any
    // "s:<name>" a user can bind), so there is no QAction to trigger for
    // them — invokeAction strips the prefix and fires this instead. R12
    // documents it as the expected connected-but-unregistered exception.
    void runScript(const QString &);
};

extern ActionManager *actionManager;
