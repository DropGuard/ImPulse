// Helpers two or more test suites need, and that would drift if each kept its
// own copy.
//
// Deliberately small, and the rule is strict: a helper used by exactly one
// suite stays in that suite's file. A shared header that collects everything
// becomes the dumping ground the core_nav split was meant to end.
#pragma once

#include <QtTest/QTest>
#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QKeyEvent>
#include <QMap>
#include <QString>
#include <QEventLoop>
#include <QQueue>
#include <QWidget>

#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"
#include "settings.h"
#include "sharedresources.h"
#include "utils/actions.h"
#include "gui/iuserinteractions.h"
#include "gui/ioverlayhost.h"
#include "utils/inputmap.h"

namespace coretest {

// The process-wide singletons a GUI suite needs, initialized by construction.
//
// Why a member and not an initTestCase line:
//
// The list used to be copied into every suite's initTestCase, and the copies had
// already diverged in spirit: core_nav and gui_widgets listed all six,
// engine_stack_fixture listed only `settings`. They agreed by luck, not by
// construction. The cost of the luck running out is a segfault inside whatever
// constructor dereferenced the missing one — when this suite was first split and
// scriptManager was left out, SettingsDialog::readScripts() crashed twelve tests.
//
// The first fix was an explicit initAppSingletons() call plus a verify pass that
// asserted the pointers were non-null. Both were wrong. getInstance() allocates
// on first call and never returns null, so those six asserts were tautologies
// that could not fail for any reason this codebase can produce; the seventh,
// checking the application identity, was tautological too — IsolatedHome, a
// member a few lines above, sets it. A check that cannot fail is a comment with
// the cost of a test.
//
// A constructor cannot be forgotten the way a call can: omitting the member
// leaves the singletons null and the very first use crashes at its own line,
// which is the honest failure. Suites declare `AppSingletons singletons;` once.
//
// The order mirrors main.cpp, the only place the production sequence is written
// down, so a singleton added to the app is added here once.
class AppSingletons {
  public:
    AppSingletons() {
        inputMap = InputMap::getInstance();
        appActions = Actions::getInstance();
        settings = Settings::getInstance();
        scriptManager = ScriptManager::getInstance();
        actionManager = ActionManager::getInstance();
        shrRes = SharedResources::getInstance();
    }

    // Deliberately not a destructor that nuls the globals: the suites are
    // process-lifetime objects and the singletons are the app's, not the test's.
    // Putting them back would be a claim that a suite can hand a working
    // process to the next one, and nothing here does.
    AppSingletons(const AppSingletons &) = delete;
    AppSingletons &operator=(const AppSingletons &) = delete;
};

// Deliver everything already queued, then stop. This is the honest form of
// "give the event loop a chance", as opposed to a fixed qWait: it does not
// assert that some amount of wall-clock time was enough, and it cannot pass
// early just because the machine was fast. What it does NOT do is wait for
// something that has not been queued yet -- a wait for a future event is a
// QTRY_* on a condition, not a sleep.
inline void drainEvents(int rounds = 3) {
    // three rounds, because a handler that queues another event needs a second
    // pass to see it; past that the queue is either empty or waiting on
    // something this helper cannot see
    for(int i = 0; i < rounds; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

// The scripted IUserInteractions: answers queued as DialogResults (consumed in
// order), call counts and last arguments as the assertions' teeth. An empty
// queue answers a default-constructed DialogResult (no, no all, no cancel) --
// a deliberate trap: an unexpected question surfaces as a failed expectation,
// not as a silently-tolerated dialog.
//
// Shared because the suites that drive Core's dialogs live in three files now;
// a second copy would be a second answer to "what does a scripted yes mean".
class FakeInteractions : public IUserInteractions {
  public:
    QQueue<DialogResult> replaceAnswers;
    QList<QPair<QString, QString>> replaceArgs;
    int replaceCalls = 0;
    int confirmCalls = 0;
    bool confirmAnswer = true;

    bool showConfirmation(const QString &, const QString &) override {
        ++confirmCalls;
        return confirmAnswer;
    }
    DialogResult fileReplaceDialog(const QString &src, const QString &dst, FileReplaceMode, bool) override {
        ++replaceCalls;
        replaceArgs.append({src, dst});
        return replaceAnswers.isEmpty() ? DialogResult{} : replaceAnswers.dequeue();
    }
};

// The scancode the platform's own table pairs with a key name — looked up, not
// hardcoded, so a sweep survives table edits and stays honest about the
// platform it runs on.
inline quint32 scanCodeFor(const QString &keyName) {
    const QMap<quint32, QString> keyTable = inputMap->keys();
    for(auto it = keyTable.constBegin(); it != keyTable.constEnd(); ++it)
        if(it.value() == keyName)
            return it.key();
    return 0;
}

// A KeyPress the way ShortcutBuilder will see it: with a real native scan code,
// because a synthesized event's default 0 yields an empty shortcut name and
// would silently resolve to nothing.
inline QKeyEvent keyPressFor(Qt::Key key, const QString &keyName, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    return QKeyEvent(QEvent::KeyPress, key, modifiers, scanCodeFor(keyName), 0u, 0u, QString(), false, 0);
}

// A file's bytes, with a test failure (not a silent empty array) when it cannot
// be read.
inline QByteArray contentOf(const QString &path) {
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly)) {
        QTest::qFail(qPrintable(QStringLiteral("cannot read %1").arg(path)), __FILE__, __LINE__);
        return {};
    }
    return f.readAll();
}

// The widget census: every live QWidget class and its count. `allWidgets` is
// Qt's own registry, so this sees children a destructor forgot.
inline QMap<QString, int> widgetClasses() {
    QMap<QString, int> counts;
    const QList<QWidget *> widgets = QApplication::allWidgets();
    for(QWidget *w : widgets)
        counts[QString::fromUtf8(w->metaObject()->className())]++;
    return counts;
}

// A scripted IOverlayHost: counts the calls and keeps the last arguments, so a
// test can watch where Core's messages land without owning the widget tree.
// The overlay->Core signal legs stay live -- a test emits them on the fake to
// drive Core exactly the way the real overlays do (saveRequested is the save
// prompt's save button, renameRequested is the rename overlay's confirm).
class FakeOverlayHost : public IOverlayHost {
  public:
    int messageCalls = 0;
    QString lastMessage;
    int successCalls = 0;
    QString lastSuccess;
    int errorCalls = 0;
    QString lastError;
    int saveOverlayShows = 0;
    int saveOverlayHides = 0;
    int copyMenuTriggers = 0;
    int moveMenuTriggers = 0;
    int renameRequests = 0;
    QString lastRenameCurrent;

    void showMessage(const QString &text, int duration = 1500) override {
        Q_UNUSED(duration)
        messageCalls++;
        lastMessage = text;
    }
    void showMessageSuccess(const QString &text) override {
        successCalls++;
        lastSuccess = text;
    }
    void showWarning(const QString &text) override {
        lastMessage = text;
    }
    void showError(const QString &text) override {
        errorCalls++;
        lastError = text;
    }
    void showMessageDirectory(const QString &dirName) override {
        lastMessage = dirName;
    }
    void setExifInfo(const QMap<QString, QString> &exifInfo) override {
        Q_UNUSED(exifInfo)
    }
    void toggleRenameOverlay(const QString &currentName) override {
        renameRequests++;
        lastRenameCurrent = currentName;
    }
    void showSaveOverlay() override {
        saveOverlayShows++;
    }
    void hideSaveOverlay() override {
        saveOverlayHides++;
    }
    void triggerCopyOverlay() override {
        copyMenuTriggers++;
    }
    void triggerMoveOverlay() override {
        moveMenuTriggers++;
    }
    void toggleImageInfoOverlay() override {
    }
};

} // namespace coretest
