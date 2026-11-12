#include <QTest>
#include <sys/wait.h>
#include <unistd.h>
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
#include "utils/actions.h"
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
        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QSignalSpy loop(core.mainWindow(), &MW::setLoopPlayback);
        QVERIFY(loop.isValid());

        core.startSlideshow();
        QVERIFY(core.viewState().slideshow);
        QCOMPARE(loop.count(), 1);
        QCOMPARE(loop.at(0).at(0).toBool(), false);

        // a second start with the flag already set is the no-op it must be:
        // re-arming would emit a second request and re-enter enableDocumentView
        core.startSlideshow();
        QCOMPARE(loop.count(), 1);

        core.stopSlideshow();
        QVERIFY(!core.viewState().slideshow);
        QCOMPARE(loop.count(), 2);
        QCOMPARE(loop.at(1).at(0).toBool(), true);

        core.stopSlideshow();
        QCOMPARE(loop.count(), 2);
    }

    void slideshowAdvanceIsSequentialAndArmsTheTimer() {
        const QStringList files = slideshowFiles("show2");
        Core core;
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
        Core core;
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
        Core core;
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
        Core core;
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
};

int main(int argc, char **argv) {
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
        return QTest::qExec(&t, argc, argv) != 0 ? 1 : 0;
    }
    const QMetaObject *mo = &TestSlideshow::staticMetaObject;
    QStringList testSlots;
    for(int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        const QByteArray name = m.name();
        if(m.methodType() == QMetaMethod::Slot && m.access() == QMetaMethod::Private && m.parameterCount() == 0 &&
           name != "initTestCase" && name != "cleanupTestCase" && name != "init" && name != "cleanup")
            testSlots << QString::fromUtf8(name);
    }
    int failures = 0;
    for(const QString &slot : std::as_const(testSlots)) {
        fflush(nullptr); // fork would duplicate whatever is buffered in stdout
        pid_t pid = fork();
        if(pid == 0) {
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            TestSlideshow t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        // a crashed child loses its buffered output entirely — without naming
        // the slot here, the evidence is a red ctest with nothing in the log
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "slideshow_tests: slot '%s' exited abnormally (exit=%d, sig=%d)\n",
                    slot.toUtf8().constData(), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "slideshow_tests.moc"
