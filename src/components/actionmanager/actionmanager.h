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

// The binding tables' four scopes. Public because it is a FACT ABOUT THE
// KEYMAP, not an implementation detail of this class: the settings editor
// renders and saves scopes, its rows carry one as their identity, and the
// tree used to spell the same four concepts three ways — this enum, the
// editor's function-local "all"/"document"/"grid"/"inspect" strings, and a
// separate if/else chain in the save path. A fifth scope added to the family
// used to be silently droppable by the chain's missing else.
//   Global   — live everywhere (ApplicationShortcut projection)
//   Document — while one image is on screen
//   Grid     — while the folder view holds keyboard focus
//   Inspect  — the document table's variant while fit-original/free-zoom arms
enum class Scope {
    Global,
    Document,
    Grid,
    Inspect
};
// Every scope, in the order the editor renders them. ONE enumeration point:
// the editors that draw sections, the writers that save them, and any census
// that must cover the family all read this, so a scope cannot be added to
// one reader and forgotten by another.
constexpr Scope kAllScopes[] = {Scope::Global, Scope::Document, Scope::Grid, Scope::Inspect};
// The display name each table answers with (the conflict warning's labels and
// the editor's section titles both come from here).
QString scopeLabel(Scope scope);
// The stable tag a saved row carries (Qt::UserRole) and a config section keys
// on — the codec's spelling, not the display name.
QString scopeTag(Scope scope);
// Parse a tag back; an unknown tag is the caller's error to handle.
bool scopeFromTag(const QString &tag, Scope &out);

class ActionManager : public QObject {
    Q_OBJECT
  public:
    static ActionManager *getInstance();
    ~ActionManager() override;
    bool processEvent(QInputEvent *);
    // The one token->QKeySequence expansion, shared by the projection, the
    // veto predicates and every surface that reads sequences from tokens.
    // "Enter" claims both Returns (Qt reads the main-row Enter as Return and
    // keeps Enter for the keypad); a token that is not a Qt name expands to
    // nothing — the vocabulary sweep keeps that branch empty.
    static QList<QKeySequence> sequencesForToken(const QString &token);
    // the keymap's resolution primitive: shortcut string -> action dispatch,
    // walking the armed inspect layer -> global. Public so the pointer
    // funnel's decision itself is testable (ShortcutBuilder's physical event
    // mapping is covered separately in machinery_tests).
    //
    // There is no caller-scope parameter any more: the keyboard no longer
    // comes through this path at all. A focused surface's keys are projected
    // onto QShortcuts and claimed back with ShortcutOverride; what reaches
    // here is a pointer token (wheel, buttons, double-click), which has no
    // surface to rank — or a key a surface vetoed out of QShortcutMap and
    // re-emitted (the crop overlay's save chord). Both answer through
    // actionForToken, the same walk actionForKeySequence runs.
    bool invokeActionForShortcut(
        const QString &shortcut); // arms/disarms the inspect scope: while armed, the inspect layer's
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
    // Removed: `actionForShortcut(token)` — it read the global table only, was
    // named like the resolver's twins (actionForToken / actionForKeySequence)
    // while ignoring every scope, and answered through QMap::operator[] so a
    // question about an unbound key INSERTED a null row into the live table.
    // Its one caller (the editor's conflict warning) now asks
    // scopeClaimsForToken, which names the global table in its answer.
    // A floating surface's yield test, in one place so the doors cannot
    // drift: "does this key name a subject-changing action?" — true means the
    // surface must claim the key back from QShortcutMap (ShortcutOverride) or
    // consume it on delivery. Keys by sequence: an override event carries the
    // logical key only (nativeScanCode is 0 by construction), and a delivered
    // event answers through the same projected tables the map matched against.
    bool keyNamesSubjectChangingAction(const QKeyEvent *overrideEvent) const;
    // The scope an unbound action is offered in — the editor's section
    // placement, which is a property of the action (actions_table.h's home
    // field), not of the editor that renders it.
    Scope homeScopeFor(const QString &action) const;
    // Which scope tables already claim this token, by display name ("document",
    // "folder view", "inspect"), empty when none does. The conflict checker
    // asks this instead of walking the tables itself — the four-table walk it
    // replaced had already forgotten the document table once.
    QStringList scopeClaimsForToken(const QString &token) const;
    // which action (if any) the sequence claims across the live tables —
    // grid, then the document view's (inspect variant folded in), then global
    QString actionForKeySequence(const QKeySequence &seq) const;
    // the token-shaped twin of actionForKeySequence: same tables, same order,
    // same gates — only the predicate differs (exact token match instead of
    // expanded-sequence match). Pointer tokens are the deliberate exception:
    // the grid gate asks which surface holds KEYBOARD focus, a question no
    // pointer event answers — every surface consumes its own wheel first, so
    // a pointer token reaching here has no surface to rank and answers
    // inspect -> doc -> global.
    QString actionForToken(const QString &token) const;
    const QString shortcutForAction(const QString &action);
    const QList<QString> shortcutsForAction(const QString &action);
    QStringList actionList();
    const QMap<QString, QString> &allShortcuts();
    void removeShortcut(const QString &keys);
    void removeAllShortcuts();
    void removeAllShortcuts(const QString &actionName);
    QString keyForNativeScancode(quint32 scanCode);
    void saveShortcuts();
    // ---- the scope-generic doors -----------------------------------------
    //
    // The per-scope spellings these replace predate the family being ONE
    // concept, and the editor that consumed them carried a parallel
    // vocabulary of its own: four string tags, four render passes and a
    // four-arm if/else that dropped an unknown tag silently. A scope is a
    // value, not a function name, so a fifth scope reaches every reader
    // through kAllScopes without any of them learning anything new.
    const QMap<QString, QString> &tableForPublic(Scope scope) const; // read side of tableFor
    void insertShortcut(Scope scope, const QString &keys, const QString &action);
    void removeAllShortcuts(Scope scope);
    // erase every binding of one action from one table (the script-removal
    // path). The per-scope spellings were three calls at that site and the
    // site had already forgotten the fourth table once.
    void removeAllShortcuts(Scope scope, const QString &actionName);
    void saveScope(Scope scope); // persists one table, tombstones included
    // The per-scope spellings that used to live here — addDocShortcut,
    // allGridShortcuts, saveInspectShortcuts and their siblings — are gone:
    // every one of them was tableFor(scope) under a different name, and the
    // production call sites all ask the scope-generic doors above. Two
    // spellings of one table is how the script-removal path came to clear
    // three tables out of four.
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

    // ---- the one write path ------------------------------------------------
    //
    // Every table mutation goes through commit(scope). A writer cannot reach
    // the QMap directly any more: it calls commit() with a lambda that edits
    // the map, and commit() is the only place that (a) enforces the
    // one-key-one-surface law against the changed table, (b) rebuilds the
    // projection, and (c) keeps the tables' invariants. The four bugs this
    // replaced (resetDefaults missing a table, the menu hint reader, the
    // conflict checker, the Enter dead key) were all "a writer did the map
    // edit but skipped one of the follow-ups" — a shape that cannot be
    // written once the map is not handed out.
    //
    // The map is passed as a MUTABLE REFERENCE inside the lambda, so the
    // lambda body is ordinary QMap code; what it cannot do is forget the
    // follow-up, because it never gets a chance to run one.
    using TableMutator = std::function<void(QMap<QString, QString> &)>;
    void commit(Scope scope, const TableMutator &mutate);
    // the shared erase-all-by-action loop the four removeAll* doors use
    static void eraseAction(QMap<QString, QString> &table, const QString &actionName);
    // The table a scope names, and its default layer. ONE mapping: every
    // consumer that used to reach for `&shortcuts`/`&docShortcuts` by hand
    // (the commit switch, the editor's save/persist paths) now asks here, so
    // a fifth scope cannot acquire a table in one place and miss another.
    QMap<QString, QString> &tableFor(Scope scope);
    const QMap<QString, QString> &tableFor(Scope scope) const;
    // the default layer of a scope: the hand-written base initDefaults seeds
    // and initShortcuts copies into the live table before the config overlays.
    QMap<QString, QString> &defaultsFor(Scope scope);
    const QMap<QString, QString> &defaultsFor(Scope scope) const;
    // the disjointness law, run by commit() after every write: a token a
    // scope table owns is removed from the global table. Returns true when
    // it removed something (commit ignores the result; it exists for tests).
    bool enforceDisjointness();
    // the read side of the same law: which scope tables claim this token.
    // One implementation, used by the conflict checker and by the editor's
    // warning — a new table cannot be forgotten by a reader that asks here.
    QStringList scopesClaiming(const QString &token) const;

    // the registry's single registration point: one parented QAction per
    // name. R12 reads this list against core.cpp's connectAction calls.
    QAction *addAction(const QString &name);
    void initActions();
    QMap<QString, QAction *> m_actions; // <name, trigger token>

    // the keyboard projection's state and its one apply-everything entry
    // point, called from every table mutation
    QWidget *m_windowSurface = nullptr;   // where the QActions are registered
    QWidget *m_gridSurface = nullptr;     // where the grid scope's QShortcuts live
    QWidget *m_documentSurface = nullptr; // where the document view's QShortcuts live
    QList<QShortcut *> m_gridProjections; // rebuilt whenever the grid table moves
    // Both document-surface claim sets store their source token: the hand-over
    // when inspection arms is per KEY, and a QShortcut does not remember which
    // table token produced it ("Enter" expands to two sequences, so squinting
    // at sc->key() would miss half the claim).
    struct SurfaceClaim {
        QShortcut *shortcut = nullptr;
        QString token;
    };
    QList<SurfaceClaim> m_documentProjections; // the document table's claims
    // The inspect layer's own claim set, kept apart from documentProjections so
    // arming it is an enabled-bit flip instead of a rebuild. It is a SAME-KEY,
    // SAME-SURFACE variant: Left means prevImage normally and scrollLeft when
    // inspection is armed. Two live claims on one surface are ambiguous with
    // NEITHER firing (dispatch_probe_tests), so arming cannot be "add a second
    // claim" — it is "disable the document claim and enable the inspect one,
    // per shared key". QShortcut's enabled bit does exactly that, and unlike a
    // QAction's it has no trigger() path to swallow: invokeAction() bypasses
    // QShortcut entirely, so menus keep working while a scope is disarmed.
    QList<SurfaceClaim> m_inspectProjections;
    bool m_gridActive = false; // the grid surface holds keyboard focus
    void applyKeyboardProjection();
    void reproject();                // global table (de-claimed) + inspect layer -> setShortcuts
    void rebuildGridShortcuts();     // grid table -> QShortcuts on the grid surface
    void rebuildDocumentShortcuts(); // document table -> QShortcuts; inspect -> inspectProjections
    void applyInspectProjection();   // inspectMode -> the enabled bits (no rebuild)

    QMap<QString, QString> m_defaults, m_shortcuts; // <shortcut, action>
    // the inspect scope's layer: only the deltas live here (fit original /
    // free zoom pan with the arrows instead of changing images)
    QMap<QString, QString> m_inspectDefaults, m_inspectShortcuts; // <shortcut, action>
    bool m_inspectMode = false;
    // true from construction: the app starts in the document view (State's own
    // default), and a headless Core may never see a mode change at all
    bool m_documentActive = true;
    // the folder grid's scope layer: defaults mirror the navigation the grid
    // hardcoded before the scope system existed
    QMap<QString, QString> m_gridDefaults, m_gridShortcuts; // <shortcut, action>
    // the document view's scope layer: every key that only means something
    // while one image is on screen — the walk, zoom, fit, the edit verbs and
    // the two commands that need the result to be visible. They used to live
    // in the global table with `if(viewMode == FOLDERVIEW) return` guards
    // inside the commands; a binding that does not exist needs no guard.
    QMap<QString, QString> m_docDefaults, m_docShortcuts; // <shortcut, action>

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
    ActionType validateAction(const QString &actionName);

  signals:
    // the one carrier the registry keeps: script actions are dynamic (any
    // "s:<name>" a user can bind), so there is no QAction to trigger for
    // them — invokeAction strips the prefix and fires this instead. R12
    // documents it as the expected connected-but-unregistered exception.
    void runScript(const QString &);
};

extern ActionManager *actionManager;
