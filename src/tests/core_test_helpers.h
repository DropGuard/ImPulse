// Helpers two or more test suites need, and that would drift if each kept its
// own copy.
//
// Deliberately small, and the rule is strict: a helper used by exactly one
// suite stays in that suite's file. A shared header that collects everything
// becomes the dumping ground the core_nav split was meant to end.
//
// The suites are self-contained moc TUs ("#include x.moc at the tail — the
// QTest idiom) for classes defined in their own .cpp. Classes living HERE are
// moc'd because this header is listed in each suite target's sources — an
// outlet that had to be wired deliberately (it was not, for years, which is
// why a header Q_OBJECT once failed to link and was misread as an AUTOMOC
// law). The one-suite rule above is a review convention, not a build
// enforcement: the build would happily moc a hundred classes collected here.
#pragma once

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/time.h>
#endif

#include <QtTest/QTest>
#include <gmock/gmock.h>
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
#include "gui/iuserinteractions.h"
#include "gui/ioverlayhost.h"
#include "gui/icoreview.h"
#include "gui/icropsurface.h"
#include "gui/folderview/foldergridview.h"
#include "gui/panels/mainpanel/thumbnailstrip.h"
#include "shortcutbuilder.h"
#include "utils/inputmap.h"
#include <QPainter>
#include "gui/ioverlayhost.h"
#include "gui/icoreview.h"
#include "utils/inputmap.h"

namespace coretest {

// ---- the crash-burst guard ---------------------------------------------------
//
// The fork harness streams a FULL core through systemd-coredump for every
// abnormally-exiting child — the nav_-null incident crash-looped ~80 slots,
// each writing its whole memory image (ProcessSizeMax defaults to 32G), and
// that write-through was most of the suite's 512-871 s. Two guards:
//
// setChildCoreLimit(): each slot child caps its own RLIMIT_CORE — the burst's
// write-through is bounded, and the notes-first core layout keeps backtraces
// intact under truncation. QIMGV_TEST_CORE_UNLIMITED removes the cap when a
// fresh crash needs the full image. The cap's design assumption is that crash
// evidence lives in stacks and notes; a diagnosis-driven abort (a test that
// aborts on a shortfall to name a live lock or a lost delivery) needs the
// heap itself — thread stacks sit in mappings the truncation drops first, so
// such hunts run with the variable set (2026-11-19: the 64MB core of a 2.2GB
// process named no frame, the unlimited one named the whole picture).
//
// The parent-side burst counter lives in each main's loop: three consecutive
// abnormal slots abort the rest — a fixture-wide failure carries no new
// evidence per slot, and twenty identical reds carry less than three.

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

// Window-path delivery: the only route that traverses QShortcutMap. The
// widget-level sendEvent/notify routes bypass shortcut dispatch entirely
// (dispatch_probe_tests pins which is which), so any leg that asserts "the
// map fired the action" must go through the target's window handle. One call
// is one press+release pair.
// Returns false when the target has no window handle (it was never shown), so
// the caller asserts in ITS own vocabulary: this header is shared by suites on
// two frameworks, and an assertion macro here would have to pick one — the one
// it picks either reports to QTest's absent result object (silent green under
// gtest) or drags gtest into a QTest translation unit.
inline bool deliverWindowKey(QWidget *target, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWindow *win = target->window()->windowHandle();
    if(!win)
        return false;
    QTest::keyClick(win, key, modifiers);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return true;
}

// ---- grid/test helpers shared by the widget-facing suites ----

// a scoped TestableFolderGridView for suites that drive the grid directly
class TestableFolderGridView : public FolderGridView {
  public:
    using FolderGridView::dragEnterEvent;
    using FolderGridView::dragLeaveEvent;
    using FolderGridView::dragMoveEvent;
    using FolderGridView::dropEvent;
    using FolderGridView::keyPressEvent;
};

// a grid-targeted key press over keyPressFor: the crop and grid suites drive
// their surfaces by name+scancode, not by raw Qt::Key
inline void pressGridKey(TestableFolderGridView &grid, const QString &keyName, Qt::Key key,
                         Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press = keyPressFor(key, keyName, modifiers);
    grid.keyPressEvent(&press);
}

// selection() returns QList by value: a named local keeps clazy's
// detaching-temporary quiet AND avoids a real detach per .last()
inline int lastSelection(FolderGridView &grid) {
    const QList<int> sel = grid.selection();
    return sel.last();
}

// a 16x8 with two colored halves: a solid square could never show a rotation
inline QString writeRectImage(const QString &path) {
    QImage img(16, 8, QImage::Format_RGB32);
    img.fill(Qt::red);
    QPainter p(&img);
    p.fillRect(8, 0, 8, 8, Qt::blue);
    p.end();
    if(!img.save(path, "png")) {
        qFatal("cannot encode %s", qPrintable(path));
    }
    return path;
}

class CountingStripProxy : public ThumbnailStrip {
  public:
    using ThumbnailStrip::ThumbnailStrip;
    void setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) override {
        ++thumbnailCalls;
        lastThumbnailPos = pos;
        ThumbnailStrip::setThumbnail(pos, thumb);
    }
    void setDragHover(int index) override {
        ++hoverCalls;
        ThumbnailStrip::setDragHover(index);
    }
    int thumbnailCalls = 0;
    int hoverCalls = 0;
    int lastThumbnailPos = -1;
};

// the thumbnail widgets in layout order — the drop helper needs positions
inline QList<ThumbnailWidget *> orderedThumbnails(FolderGridView &grid) {
    QList<ThumbnailWidget *> widgets;
    const QList<QGraphicsItem *> sceneItems = grid.items(); // named: no detach
    for(QGraphicsItem *item : sceneItems)
        if(auto *tw = dynamic_cast<ThumbnailWidget *>(item))
            widgets << tw;
    std::sort(widgets.begin(), widgets.end(), [](const ThumbnailWidget *a, const ThumbnailWidget *b) {
        const QPointF pa = a->scenePos(), pb = b->scenePos();
        return pa.y() != pb.y() ? pa.y() < pb.y() : pa.x() < pb.x();
    });
    return widgets;
}

// the viewport position over a given thumbnail index, for synthesized drops
inline QPoint dropPosOverThumbnail(FolderGridView &grid, int index) {
    const QList<ThumbnailWidget *> widgets = orderedThumbnails(grid);
    if(widgets.count() <= index) {
        qFatal("no thumbnail at index %d", index);
    }
    const QPointF center = widgets.at(index)->mapToScene(widgets.at(index)->boundingRect().center());
    return grid.mapFromScene(center);
}

// A recording ICoreView for headless Core tests: counts the display sink
// calls and keeps the last arguments. The mode entries are counted, not
// stored — the mode itself is Core's state, readable there.
// The same contract as RecordingView, but the calls are EXPECTATIONS rather
// than a tally: a test states what the view should be asked to do, and an
// unmentioned call fails the test instead of being silently counted. That is
// the difference between "the repaint did not happen" checked against a number
// the test had to remember to read, and EXPECT_CALL(...).Times(0).
//
// Both exist on purpose. RecordingView is right for a test that only needs the
// side effects (what size arrived, in what order); this one is right when the
// CALLS are the subject.
class MockCoreView : public ICoreView {
  public:
    MOCK_METHOD(void, showImage, (std::shared_ptr<const QImage>), (override));
    MOCK_METHOD(void, showAnimation, (const std::shared_ptr<QMovie> &), (override));
    MOCK_METHOD(void, showVideo, (const QString &), (override));
    MOCK_METHOD(void, closeImage, (), (override));
    MOCK_METHOD(void, repaintView, (), (override));
    MOCK_METHOD(void, setUpdatesEnabled, (bool), (override));
    MOCK_METHOD(void, showWindowDefault, (), (override));
    MOCK_METHOD(void, toggleFullscreenInfoBar, (), (override));
    MOCK_METHOD(void, setLoopPlayback, (bool), (override));
    MOCK_METHOD(void, onSortingChanged, (SortingMode), (override));
    MOCK_METHOD(void, onScalingFinished, (std::unique_ptr<const QImage>), (override));
};

class RecordingView : public ICoreView {
  public:
    int showImageCalls = 0;
    QSize lastImageSize;
    int showAnimationCalls = 0;
    int showVideoCalls = 0;
    int closeImageCalls = 0;
    int loopPlaybackRequests = 0;
    QList<bool> loopPlaybackValues;
    int scalingResults = 0;

    void showImage(std::shared_ptr<const QImage> image) override {
        showImageCalls++;
        lastImageSize = image ? image->size() : QSize();
    }
    void showAnimation(const std::shared_ptr<QMovie> &movie) override {
        Q_UNUSED(movie)
        showAnimationCalls++;
    }
    void showVideo(const QString &file) override {
        Q_UNUSED(file)
        showVideoCalls++;
    }
    void closeImage() override {
        closeImageCalls++;
    }
    void repaintView() override {
    }
    void setUpdatesEnabled(bool enabled) override {
        Q_UNUSED(enabled)
    }
    void showWindowDefault() override {
    }
    void toggleFullscreenInfoBar() override {
    }
    void setLoopPlayback(bool loop) override {
        loopPlaybackRequests++;
        loopPlaybackValues << loop;
    }
    void onSortingChanged(SortingMode mode) override {
        Q_UNUSED(mode)
    }
    void onScalingFinished(std::unique_ptr<const QImage> scaled) override {
        Q_UNUSED(scaled)
        scalingResults++;
    }
};

// A scripted crop surface: the ICropSurface implementation without the
// widget tree. The intent legs are real signals.
class FakeCropSurface : public QObject, public ICropSurface {
    Q_OBJECT
  public:
    int toggleCalls = 0;
    bool active = false;
    void toggle() override {
        ++toggleCalls;
        active = !active;
    }
    bool isActive() const override {
        return active;
    }
  signals:
    void cropRequested(QRect rect) override;
    void cropAndSaveRequested(QRect rect) override;
};

// A file's bytes, with a test failure (not a silent empty array) when it cannot
// be read.
inline QByteArray contentOf(const QString &path) {
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly)) {
        qFatal("cannot read %s", qPrintable(path));
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
    bool pendingSurfaces = false; // the lock's condition, test-set
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
    bool hasPendingSurface() const override {
        return pendingSurfaces;
    }
};

} // namespace coretest
