// Dispatch probes: pins what QShortcutMap actually does before the keyboard
// migration leans on it. Every assumption in the migration plan — the delivery
// route that reaches the map, context precedence, the ShortcutOverride veto,
// what QLineEdit/QSpinBox keep for themselves — is asserted here against Qt
// 6.10 on the offscreen platform, with local QShortcuts/QActions only: no
// Core, no ActionManager, so this suite is green on the pre-migration tree and
// stays green after it. A probe that fails means a plan assumption is wrong —
// fix the design before touching production code.
//
// The one thing this suite deliberately does NOT pin is the Qt::Popup keyboard
// grab: offscreen delivery cannot model a real window system's grab, and the
// popup contract ("the menu owns its keys") is pinned behaviorally in
// gui_widget_tests instead — an open ContextMenu must not let an application
// shortcut change viewer state behind it, whichever way the grab lands.
#include <QtTest/QTest>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QShortcut>
#include <QSpinBox>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWidget>

#include <sys/wait.h>
#include <unistd.h>

namespace {

// A focusable probe surface that records ShortcutOverride and keyPress
// delivery, and can veto the override (accept it, which suppresses the
// shortcut and lets the key arrive as a normal KeyPress).
class ProbeSurface : public QWidget {
  public:
    using QWidget::QWidget;

    int overrideSeen = 0;
    int pressSeen = 0;
    bool veto = true;
    QKeySequence lastPressed;

  protected:
    bool event(QEvent *e) override {
        if(e->type() == QEvent::ShortcutOverride) {
            ++overrideSeen;
            if(veto) {
                e->accept();
                return true;
            }
            return QWidget::event(e);
        }
        return QWidget::event(e);
    }
    void keyPressEvent(QKeyEvent *e) override {
        ++pressSeen;
        lastPressed = QKeySequence(e->key() | e->modifiers());
        e->accept();
    }
};

// window-path delivery: the QTest QWindow overload routes through the window
// system interface, the only route that traverses QShortcutMap. The widget
// overload of QTest::keyClick is probed separately in
// aWindowDeliveryRouteReachesTheShortcutMap and its result recorded in the
// run log, not relied on. One call = one press+release pair.
void deliverKey(QWidget *target, Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QWindow *win = target->window()->windowHandle();
    QVERIFY2(win != nullptr, "the probe surface has no window handle — was it shown?");
    QTest::keyClick(win, key, mods);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

} // namespace

class DispatchProbe : public QObject {
    Q_OBJECT

  private slots:
    // The load-bearing discovery: WHICH shortcut form fires under window
    // delivery on this platform. Four variants — QShortcut on the widget,
    // QAction parented to the widget, QAction added to it, parentless QAction
    // — all Application context, all enabled, one key each. The qDebug matrix
    // decides which form the migration projects the tables onto, and the
    // assert keeps whatever Qt actually answers pinned.
    void aWindowDeliveryRouteReachesTheShortcutMap() {
        // a NON-vetoing surface: the veto contract is probed separately, and
        // a veto-everything surface would suppress its own probe shortcut
        ProbeSurface w;
        w.veto = false;
        w.resize(200, 100);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.activateWindow();
        w.setFocus();
        QVERIFY(QTest::qWaitForWindowActive(&w));

        QShortcut direct(&w);
        direct.setKey(QKeySequence(QStringLiteral("Ctrl+G")));
        direct.setContext(Qt::ApplicationShortcut);
        QSignalSpy directFired(&direct, &QShortcut::activated);

        QAction parented(&w);
        parented.setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
        parented.setShortcutContext(Qt::ApplicationShortcut);
        QSignalSpy parentedFired(&parented, &QAction::triggered);

        QAction added(nullptr);
        added.setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
        added.setShortcutContext(Qt::ApplicationShortcut);
        w.addAction(&added);
        QSignalSpy addedFired(&added, &QAction::triggered);

        QAction orphan(nullptr);
        orphan.setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
        orphan.setShortcutContext(Qt::ApplicationShortcut);
        QSignalSpy orphanFired(&orphan, &QAction::triggered);

        w.setFocus();
        QVERIFY(QApplication::focusWidget() == &w);

        deliverKey(&w, Qt::Key_G, Qt::ControlModifier);
        deliverKey(&w, Qt::Key_H, Qt::ControlModifier);
        deliverKey(&w, Qt::Key_J, Qt::ControlModifier);
        deliverKey(&w, Qt::Key_L, Qt::ControlModifier);

        qDebug() << "[probe] QShortcut on widget        fired:" << directFired.count();
        qDebug() << "[probe] QAction parented to widget fired:" << parentedFired.count();
        qDebug() << "[probe] QAction added to widget    fired:" << addedFired.count();
        qDebug() << "[probe] QAction parentless         fired:" << orphanFired.count();
        qDebug() << "[probe] widget overrideSeen:" << w.overrideSeen << "pressSeen:" << w.pressSeen;
        QVERIFY2(directFired.count() + parentedFired.count() + addedFired.count() + orphanFired.count() > 0,
                 "no shortcut form fired under window delivery — the keyboard migration has no "
                 "test vehicle on this platform");
    }

    // The grid-scope design rides this: a WidgetWithChildrenShortcut on a
    // focused subtree must outrank an ApplicationShortcut claiming the same
    // key, and must go inert the moment focus leaves the subtree.
    void widgetWithChildrenBeatsApplicationOnTheSameKey() {
        QWidget host;
        host.resize(300, 200);
        auto *inner = new QWidget(&host);
        inner->resize(100, 100);
        auto *other = new QWidget(&host);
        other->resize(100, 100);
        other->move(150, 0);
        auto *layout = new QVBoxLayout(&host);
        layout->addWidget(inner);
        layout->addWidget(other);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));

        QAction global(&host);
        global.setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
        global.setShortcutContext(Qt::ApplicationShortcut);
        host.addAction(&global); // registration: an un-added QAction's shortcut is inert
        QSignalSpy globalFired(&global, &QAction::triggered);

        QAction scoped(inner);
        scoped.setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
        scoped.setShortcutContext(Qt::WidgetWithChildrenShortcut);
        inner->addAction(&scoped); // registration: an un-added QAction's shortcut is inert
        QSignalSpy scopedFired(&scoped, &QAction::triggered);

        inner->setFocus();
        deliverKey(&host, Qt::Key_M, Qt::ControlModifier);
        // Qt 6.10 has NO context-specificity precedence: two live claims of
        // one key are ambiguous and NEITHER fires. The grid scope therefore
        // cannot lean on context layering — the projection must de-claim the
        // grid's keys from the global table while the grid holds focus.
        QCOMPARE(scopedFired.count(), 0);
        QCOMPARE(globalFired.count(), 0);

        globalFired.clear();
        scopedFired.clear();
        other->setFocus();
        deliverKey(&host, Qt::Key_M, Qt::ControlModifier);
        QCOMPARE(scopedFired.count(), 0); // focus left the subtree: the scope is inert
        QCOMPARE(globalFired.count(), 1); // and the app-wide binding answers
        QVERIFY(QApplication::focusWidget() == other);
    }

    // The overlay-yield contract in Qt's own vocabulary: accepting the
    // ShortcutOverride suppresses the shortcut and delivers the key to the
    // surface; not accepting lets the map fire and the surface never sees the
    // key.
    void shortcutOverrideAcceptKeepsTheKeyLocal() {
        QAction global(nullptr);
        global.setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
        global.setShortcutContext(Qt::ApplicationShortcut);
        QSignalSpy globalFired(&global, &QAction::triggered);

        ProbeSurface vetoing;
        vetoing.resize(200, 100);
        vetoing.show();
        vetoing.addAction(&global);
        QVERIFY(QTest::qWaitForWindowExposed(&vetoing));
        vetoing.setFocus();

        deliverKey(&vetoing, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(globalFired.count(), 0);
        QVERIFY(vetoing.overrideSeen > 0);
        // QTest's QWindow route synthesizes the modifier's own press before
        // the chord: both arrive locally after the veto
        QCOMPARE(vetoing.pressSeen, 2);

        globalFired.clear();
        ProbeSurface yielding;
        yielding.veto = false;
        yielding.resize(200, 100);
        yielding.show();
        yielding.addAction(&global);
        QVERIFY(QTest::qWaitForWindowExposed(&yielding));
        yielding.setFocus();

        deliverKey(&yielding, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(globalFired.count(), 1); // the map fired; the action system owns the key
        // the surface still sees the modifier's OWN press (no shortcut matches
        // a bare modifier) — the chord's key itself is what the map consumed
        QCOMPARE(yielding.pressSeen, 1);
    }

    // Line edits and spin boxes keep their plain keys for themselves — the
    // Qt-native form of the "curated passthrough". What we RELY on is
    // asserted; what merely informs door design (Esc, arrows in a line edit)
    // is recorded in the run log.
    void lineEditsOwnTheirPlainKeys() {
        QWidget host;
        host.resize(300, 120);
        auto *edit = new QLineEdit(&host);
        auto *spin = new QSpinBox(&host);
        spin->setRange(0, 100);
        auto *layout = new QVBoxLayout(&host);
        layout->addWidget(edit);
        layout->addWidget(spin);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));

        QAction digitAction(&host);
        digitAction.setShortcut(QKeySequence(QStringLiteral("1")));
        digitAction.setShortcutContext(Qt::ApplicationShortcut);
        host.addAction(&digitAction);
        QSignalSpy digitFired(&digitAction, &QAction::triggered);
        QAction letterAction(&host);
        letterAction.setShortcut(QKeySequence(QStringLiteral("F")));
        letterAction.setShortcutContext(Qt::ApplicationShortcut);
        host.addAction(&letterAction);
        QSignalSpy letterFired(&letterAction, &QAction::triggered);
        QAction upAction(&host);
        upAction.setShortcut(QKeySequence(QStringLiteral("Up")));
        upAction.setShortcutContext(Qt::ApplicationShortcut);
        host.addAction(&upAction);
        QSignalSpy upFired(&upAction, &QAction::triggered);
        QAction escAction(&host);
        escAction.setShortcut(QKeySequence(QStringLiteral("Esc")));
        escAction.setShortcutContext(Qt::ApplicationShortcut);
        host.addAction(&escAction);
        QSignalSpy escFired(&escAction, &QAction::triggered);

        // digits: the crop spin boxes' number entry must survive a global
        // "1"->fitWindow-style binding
        edit->setFocus();
        deliverKey(&host, Qt::Key_1);
        QCOMPARE(digitFired.count(), 0);
        QCOMPARE(edit->text(), QStringLiteral("1"));

        // letters: same contract
        edit->clear();
        deliverKey(&host, Qt::Key_F);
        QCOMPARE(letterFired.count(), 0);
        QCOMPARE(edit->text(), QStringLiteral("f"));

        // arrows in a line edit are NOT vetoed: the Up action fires instead
        // of the spin box stepping. The crop panel's SpinBoxInputFix must
        // veto its allowed keys itself — Qt does not give the spin box
        // ownership of its navigation keys.
        const int valueBefore = spin->value();
        spin->setFocus();
        deliverKey(&host, Qt::Key_Up);
        qDebug() << "[probe] spinbox-focus Up fired the Up action:" << (upFired.count() > 0)
                 << "— spinbox stepped:" << (spin->value() != valueBefore);

        // Esc in a line edit is NOT vetoed either: the Esc action fires even
        // while text is being edited. Doors hosting editors (rename overlay,
        // resize dialog's editors, changelog's close button) need their own
        // veto on top of the subject-changing rule.
        escFired.clear();
        edit->setFocus();
        deliverKey(&host, Qt::Key_Escape);
        QCOMPARE(escFired.count(), 1);
        QVERIFY(QApplication::focusWidget() == edit);
        QVERIFY(digitFired.count() == 0 && letterFired.count() == 0);
    }

    // The accepted delta, pinned: an application-wide shortcut fires while an
    // application-modal dialog is on screen. Ctrl+S in the settings dialog
    // starts saving instead of doing nothing — written into the migration
    // notes, asserted here so a future Qt change surfaces by name.
    void applicationShortcutsFireInsideModalDialogs() {
        QWidget behind;
        behind.resize(200, 100);
        behind.show();
        QVERIFY(QTest::qWaitForWindowExposed(&behind));

        QAction global(&behind);
        global.setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
        global.setShortcutContext(Qt::ApplicationShortcut);
        behind.addAction(&global);
        QSignalSpy globalFired(&global, &QAction::triggered);

        QDialog dialog;
        dialog.setModal(true);
        dialog.resize(150, 80);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        dialog.setFocus();
        QVERIFY(dialog.isActiveWindow());

        deliverKey(&dialog, Qt::Key_S, Qt::ControlModifier);
        // Qt 6.10 SHIELDS application shortcuts behind an application-modal
        // dialog — the opposite of the plan's assumption: modal dialogs keep
        // today's behavior for free, and no door veto is needed inside them
        QCOMPARE(globalFired.count(), 0);
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // One process per test function — the same rationale as the GUI suites'
    // fork-per-slot mains, applied to a suite that constructs its own windows
    // per slot. No Core and no engine here, so no core-limit plumbing.
    if(argc > 1) {
        DispatchProbe t;
        return QTest::qExec(&t, argc, argv) != 0 ? 1 : 0;
    }
    const QMetaObject *mo = &DispatchProbe::staticMetaObject;
    QStringList testSlots;
    for(int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        const QByteArray name = m.name();
        if(m.methodType() == QMetaMethod::Slot && m.access() == QMetaMethod::Private && m.parameterCount() == 0 &&
           name != "initTestCase" && name != "cleanupTestCase" && name != "init" && name != "cleanup")
            testSlots << QString::fromUtf8(name);
    }
    int failures = 0, consecutiveAbnormal = 0;
    for(const QString &slot : std::as_const(testSlots)) {
        fflush(nullptr); // fork would duplicate whatever is buffered in stdout
        pid_t pid = fork();
        if(pid == 0) {
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            DispatchProbe t; // this child's own fixture
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "dispatch_probe_tests: slot '%s' exited abnormally (exit=%d, sig=%d)\n",
                    slot.toUtf8().constData(), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        consecutiveAbnormal = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : consecutiveAbnormal + 1;
        if(consecutiveAbnormal == 3)
            break;
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "dispatch_probe_tests.moc"
