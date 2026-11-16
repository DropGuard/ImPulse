#include <QTest>
#include "slot_harness.h"
#include <utility>
#include <QSignalSpy>
#include <QApplication>

#include "core_test_helpers.h"
#include "testhelpers.h"
#include "core.h"
#include "settings.h"
#include "sharedresources.h"
#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"
#include "utils/inputmap.h"
#include "gui/dialogs/settingsdialog.h"

class TestSlideshow : public QObject {
    Q_OBJECT

  private:
    IsolatedHome home;
    coretest::AppSingletons singletons;
    QString base;

    void writeImage(const QString &path) {
        QVERIFY(makeImageFile(path, QSize(8, 8)));
    }
    QString uniqueDir(const QString &tag) {
        const QString dir = QDir(base).filePath(tag + QStringLiteral("-{}").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

  private slots:
    void initTestCase() {
        QVERIFY(home.path() != nullptr);
        base = QDir(home.path()).filePath("slideshow");
        QVERIFY(QDir().mkpath(base));
    }

    QStringList slideshowFiles(const QString &tag) {
        const QString dir = QDir(base).filePath(tag);
        // not QVERIFY: it expands to `return;` and this helper has a value to
        // hand back — the same reason dropPosOverThumbnail uses qFail
        if(!QDir().mkpath(dir))
            QTest::qFail("cannot create the slideshow fixture directory", __FILE__, __LINE__);
        QStringList files;
        for(int i = 1; i <= 4; ++i) {
            const QString f = dir + QStringLiteral("/s%1.png").arg(i);
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            if(!img.save(f, "PNG"))
                QTest::qFail(qPrintable(QStringLiteral("cannot write ") + f), __FILE__, __LINE__);
            files << f;
        }
        return files;
    }

    void slideshowLifecycleRequestsLoopPlaybackOffThenOn() {
        const QStringList files = slideshowFiles("show1");
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        // the loop request lands on the view seam; RecordingView keeps every value

        core.startSlideshow();
        QVERIFY(core.viewState().slideshow);
        QCOMPARE(view.loopPlaybackRequests, 1);
        QCOMPARE(view.loopPlaybackValues.at(0), false);

        // a second start with the flag already set is the no-op it must be:
        // re-arming would emit a second request and re-enter enableDocumentView
        core.startSlideshow();
        QCOMPARE(view.loopPlaybackRequests, 1);

        core.stopSlideshow();
        QVERIFY(!core.viewState().slideshow);
        QCOMPARE(view.loopPlaybackRequests, 2);
        QCOMPARE(view.loopPlaybackValues.at(1), true);

        core.stopSlideshow();
        QCOMPARE(view.loopPlaybackRequests, 2);
    }

    void slideshowAdvanceIsSequentialAndArmsTheTimer() {
        const QStringList files = slideshowFiles("show2");
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.startSlideshow();
        // startSlideshow arms the timer itself, through startSlideshowTimer,
        // for a static image — measured, this assertion's first draft claimed
        // the opposite and failed
        QVERIFY(core.slideshowClock().isActive());

        core.nextImageSlideshow();
        QCOMPARE(core.currentFilePath(), files.at(1));
        QVERIFY2(core.slideshowClock().isActive(), "a static image under slideshow must keep the timer armed");

        core.nextImageSlideshow();
        QCOMPARE(core.currentFilePath(), files.at(2));
    }

    void nonLoopingSlideshowStopsAtTheEndOfTheListing() {
        const QStringList files = slideshowFiles("show3");
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.slideshowLoops());

        core.jumpToLast();
        QCOMPARE(core.currentFilePath(), files.at(3));
        // jumpToLast stops the show, so the state below is a fresh arm
        QVERIFY(!core.viewState().slideshow);

        core.jumpToFirst();
        QCOMPARE(core.currentFilePath(), files.at(0));

        core.startSlideshow();
        core.jumpToLast();
        core.startSlideshow();
        QVERIFY(core.viewState().slideshow);
        const QString atEnd = core.currentFilePath();

        core.nextImageSlideshow();
        QCOMPARE(core.currentFilePath(), atEnd);
        QVERIFY2(!core.viewState().slideshow, "a non-looping slideshow must stop itself at the end of the directory");
        QVERIFY(!core.slideshowClock().isActive());
    }

    void shuffleKeepsTheSlideshowCyclingPastTheEnd() {
        const QStringList files = slideshowFiles("show4");
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.viewState().shuffle);

        core.toggleShuffle();
        QVERIFY(core.viewState().shuffle); // this is what syncRandomizer sized for

        core.startSlideshow();
        core.jumpToLast();
        core.startSlideshow();
        QVERIFY(core.viewState().slideshow);

        // the sequential branch would have stopped here (pinned above with the
        // same setup); the randomizer branch starts a new cycle instead
        core.nextImageSlideshow();
        QVERIFY2(core.viewState().slideshow, "shuffle mode stopped at the end of the listing");
        QVERIFY(files.contains(core.currentFilePath()));
    }

    void toggleSlideshowStartsThenStops() {
        const QStringList files = slideshowFiles("show5");
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.viewState().slideshow);

        core.toggleSlideshow();
        QVERIFY(core.viewState().slideshow);
        QVERIFY(core.slideshowClock().isActive());

        core.toggleSlideshow();
        QVERIFY2(!core.viewState().slideshow, "the second toggle did not stop the slideshow");
        QVERIFY(!core.slideshowClock().isActive());
    }

    // The slideshow and editing are mutually exclusive states. startSlideshow
    // already refuses to start over an uncommitted edit (navigationDenied's
    // state leg); this is the closing half — a running clock refuses the edit,
    // because the next tick's unloadExcept would discard it. The refusal
    // speaks: the clock's owner is the party that would destroy the work.
    void editingIsRefusedWhileTheSlideshowRuns() {
        const QStringList files = slideshowFiles("show6");
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        auto img = core.directoryModel()->getImage(core.currentFilePath());
        QVERIFY(img);
        QVERIFY(!img->isEdited());

        core.startSlideshow();
        QVERIFY(core.viewState().slideshow);
        const int messagesBefore = overlayFake.messageCalls;

        core.flipH();
        QVERIFY2(!img->isEdited(), "the edit landed while the slideshow was running");
        QCOMPARE(overlayFake.messageCalls, messagesBefore + 1);
        QVERIFY2(overlayFake.lastMessage.contains(QStringLiteral("Stop the slideshow")),
                 qPrintable(QStringLiteral("unexpected refusal text: ") + overlayFake.lastMessage));
        QVERIFY(core.viewState().slideshow);           // the clock is untouched
        QCOMPARE(core.currentFilePath(), files.at(0)); // and nothing moved

        // the refusal is the state's, not flipH's: another edit verb goes
        // through the same gate
        core.rotateRight();
        QCOMPARE(overlayFake.messageCalls, messagesBefore + 2);
        QVERIFY(!img->isEdited());

        // stopping the slideshow is the whole resolution — nothing else
        core.stopSlideshow();
        core.flipH();
        QVERIFY2(img->isEdited(), "the edit stayed refused after the clock stopped");
    }
};

int main(int argc, char **argv) {
    // QApplication, not QCoreApplication: the tested paths still touch no
    // widget, pixmap or clipboard — the loop request lands on the
    // RecordingView seam, not on a window — but the action registry now
    // carries QActions, and Qt's QAction constructor resolves its
    // private-class factory through QGuiApplicationPrivate::self, which a
    // bare QCoreApplication leaves null: the first action creation would
    // segfault before any test could run
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // One process per test function — the same rationale as engine_stack_main's
    // fork per class, taken one level down: the suites decode images, which
    // makes QtGui spawn and clockjoin-join its own internal pooled threads, and
    // each of those joins leaves a registry-live pthread_t behind (TSan does not
    // intercept clockjoin — LLVM #239326).
    //
    // The fixture is constructed INSIDE the child, not before the fork: a
    // fixture built in the parent would hand every child an IsolatedHome whose
    // tmpDir dies with the first child to exit.
    if(argc > 1) {
        TestSlideshow t;
        return slot_harness::runFiltered(t, argc, argv) != 0 ? 1 : 0;
    }
    return slot_harness::run_units_one_process_each(
        argc, argv, slot_harness::testSlots(&TestSlideshow::staticMetaObject),
        "slideshow_tests",
        [argv](const QString &slot) -> int {
            coretest::setChildCoreLimit();
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            TestSlideshow t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        });
}

#include "slideshow_tests.moc"
