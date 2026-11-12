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

class TestPresenter : public QObject {
    Q_OBJECT

  private:
    IsolatedHome home;
    coretest::AppSingletons singletons;
    QString base, alpha, beta;

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
        base = QDir(home.path()).filePath("presenter");
        alpha = QDir(base).filePath("alpha");
        beta = QDir(base).filePath("beta");
        QVERIFY(QDir().mkpath(alpha));
        QVERIFY(QDir().mkpath(beta));
        writeImage(QDir(alpha).filePath("a.png"));
        writeImage(QDir(beta).filePath("b.png"));
        QVERIFY(QDir().mkpath(base));
    }

    void presenterPopulatesTheViewWhenModelArrives() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy proxy;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);

        presenter.setView(&proxy);
        presenter.setModel(model);
        proxy.show(); // the strip materializes on showEvent, not on populate

        QVERIFY2(proxy.isInitialized(), "show never materialized the strip");
    }

    void presenterSetViewTwiceKeepsTheFirstBinding() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy first, second;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);

        presenter.setView(&first);
        first.show();
        presenter.setView(&second); // rejected: the binding is already live
        presenter.setModel(model);

        QVERIFY(first.isInitialized());
        QVERIFY2(!second.isInitialized(), "the second setView silently replaced the first");
    }

    void presenterForwardsActivationToTheFilePath() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy proxy;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&proxy);
        proxy.show(); // materialize the strip so the forwarding path is the real one
        presenter.setModel(model);
        QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);
        QVERIFY(activated.isValid());

        QMetaObject::invokeMethod(&proxy, "itemActivated", Q_ARG(int, 0));

        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.at(0).at(0).toString(), QDir(alpha).filePath("a.png"));
    }

    void presenterDisconnectViewStopsForwardingAndAllowsRebinding() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy first, second;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&first);
        presenter.setModel(model);
        QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);

        QMetaObject::invokeMethod(&first, "itemActivated", Q_ARG(int, 0));
        QCOMPARE(activated.count(), 1);

        presenter.disconnectView();
        QMetaObject::invokeMethod(&first, "itemActivated", Q_ARG(int, 0));
        QCOMPARE(activated.count(), 1); // the detached view no longer reaches us

        presenter.setView(&second); // rebinding was impossible before disconnectView
        second.show();
        presenter.setModel(model);
        QVERIFY2(second.isInitialized(), "rebinding after disconnectView does not populate");
        QMetaObject::invokeMethod(&second, "itemActivated", Q_ARG(int, 0));
        QCOMPARE(activated.count(), 2);
        QCOMPARE(activated.at(1).at(0).toString(), QDir(alpha).filePath("a.png"));
    }

    void presenterUnsetModelKeepsTheViewBinding() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy proxy;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&proxy);
        proxy.show();
        presenter.setModel(model);

        presenter.unsetModel(); // detaches the model, keeps the view on purpose

        QVERIFY(proxy.isInitialized()); // the binding survived the model swap
        auto model2 = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model2->setDirectory(beta));
        QTRY_COMPARE_WITH_TIMEOUT(model2->fileCount(), 1, 10000);
        presenter.setModel(model2);
        QVERIFY2(proxy.isInitialized(), "re-setting a model on a live binding must repopulate");
    }

    // The presenter's five view connections are compiled against the concrete
    // proxy type (setView is a template: see directorypresenter.h), so a signal
    // or slot that changed shape fails the build instead of silently disconnect
    // at runtime — which is what the SIGNAL()/SLOT() strings this replaced used
    // to allow. What a compiler cannot check is delivery: whether a signal the
    // view really emits reaches the handler and produces the effect. The tests
    // in this cluster pin delivery per endpoint — itemActivated both ways,
    // draggedOut and droppedInto below.
    // The presenter's two remaining string-based view connections. Neither has
    // a signal at the far end — thumbnailsRequested drives the thumbnailer and
    // the result comes back as a view *method call*, draggedOver reaches the
    // view only while directories are listed — so both are pinned by following
    // the call back into the view rather than by spying on a signal.
    void presenterRoutesThumbnailsAndDragHoverToTheView() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        coretest::CountingStripProxy view;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&view);
        view.show(); // materialize so the forwarding path is the real one
        presenter.setModel(model);

        // the strip requests thumbnails for itself once it materializes; wait
        // that out, then clear the counter so the number below is attributable
        // only to the invoke that drives the presenter's connection
        QTRY_VERIFY_WITH_TIMEOUT(view.thumbnailCalls > 0, 10000);
        view.thumbnailCalls = 0;
        view.lastThumbnailPos = -1;

        QMetaObject::invokeMethod(&view, "thumbnailsRequested", Q_ARG(QList<int>, {0}), Q_ARG(int, 128),
                                  Q_ARG(bool, true), Q_ARG(bool, false));
        QTRY_COMPARE_WITH_TIMEOUT(view.thumbnailCalls, 1, 10000);
        QCOMPARE(view.lastThumbnailPos, 0);

        // the draggedOver branch needs a directory index while dirs are shown,
        // and onDraggedOver skips anything already selected — populate selects
        // index 0, so the hover target is the second directory
        QVERIFY(QDir(alpha).mkpath("subdir"));
        QVERIFY(QDir(alpha).mkpath("subdir2"));
        presenter.setShowDirs(true);
        presenter.reloadModel();
        QTRY_COMPARE_WITH_TIMEOUT(model->dirCount(), 2, 10000);
        // onDraggedOver has three guards (model, selection, dirs-visible) and
        // a hover index that must name a directory: each is asserted right
        // before the invoke so a lost delivery names the guard it hit instead
        // of reporting a bare zero.
        QVERIFY2(presenter.showDirs(), "precondition broken: the presenter is in files-only mode");
        QVERIFY2(model->dirCount() > 1,
                 qPrintable(QStringLiteral("precondition broken: only %1 directories listed").arg(model->dirCount())));
        QVERIFY2(!view.selection().contains(1),
                 "precondition broken: index 1 is selected, onDraggedOver returns early");

        QMetaObject::invokeMethod(&view, "draggedOver", Q_ARG(int, 1));
        QVERIFY2(view.hoverCalls == 1,
                 qPrintable(QStringLiteral("setDragHover never reached the view (dirCount=%1, "
                                           "showDirs=%2, selection=%3)")
                                .arg(model->dirCount())
                                .arg(presenter.showDirs())
                                .arg(view.selection().isEmpty() ? QStringLiteral("empty")
                                                                : QStringLiteral("%1").arg(view.selection().at(0)))));
    }

    void presenterForwardsDragFromTheView() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy proxy;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&proxy);
        proxy.show(); // materialize so the forwarding path is the real one
        presenter.setModel(model);

        QSignalSpy out(&presenter, &DirectoryPresenter::draggedOut);
        QSignalSpy into(&presenter, &DirectoryPresenter::droppedInto);
        QVERIFY(out.isValid() && into.isValid());

        // view draggedOut() -> presenter onDraggedOut -> presenter draggedOut(paths)
        // Qt6 signals are public, so the forwarding chain is driven directly:
        // a by-name invoke would put the signature in a string for no service
        emit proxy.draggedOut();
        QCOMPARE(out.count(), 1);

        // view droppedInto(mime, source, index) -> presenter droppedInto(paths, dir)
        QMimeData data;
        data.setUrls({QUrl::fromLocalFile(QDir(alpha).filePath("a.png"))});
        emit proxy.droppedInto(&data, nullptr, 0);
        QCOMPARE(into.count(), 1);
    }

    // The presenter is the only place that turns an engine-side index — files
    // and directories counted in separate lists, dirs first — into the view's
    // single flat grid index. Nothing else checks that arithmetic, and a wrong
    // offset is silent: the grid still splices, just one row away from the
    // entry the event named. The events are emitted by hand because the
    // contract under test is the translation, not the arrival — arrival and
    // index provenance are pinned by the mirror-contract test.
    void presenterTranslatesEngineIndicesIntoGridIndices() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        const QString dir = uniqueDir("splice");
        QVERIFY(QDir(dir).mkpath("d1"));
        QVERIFY(QDir(dir).mkpath("d2"));
        writeImage(QDir(dir).filePath("f1.png"));
        writeImage(QDir(dir).filePath("f2.png"));
        QVERIFY(model->setDirectory(dir));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(model->dirCount(), 2, 10000);
        const QString f0 = model->filePathAt(0);
        const QString f1 = model->filePathAt(1);
        const QString d0 = model->dirPathAt(0);
        const QString d1 = model->dirPathAt(1);
        const int dirCount = model->dirCount();
        QVERIFY2(dirCount > 0, "precondition broken: no directories to offset by");

        coretest::CountingStripProxy view;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&view);
        presenter.setModel(model);

        // files-only: the engine index goes to the grid untranslated, and the
        // directory handlers refuse the event outright
        QMetaObject::invokeMethod(model.get(), "fileModified", Q_ARG(QString, f0));
        QCOMPARE(view.reloaded, QList<int>({0}));
        QMetaObject::invokeMethod(model.get(), "fileAdded", Q_ARG(QString, f1));
        QCOMPARE(view.inserted, QList<int>({1}));
        QMetaObject::invokeMethod(model.get(), "fileRemoved", Q_ARG(QString, f0), Q_ARG(int, 0));
        QCOMPARE(view.removed, QList<int>({0}));
        QMetaObject::invokeMethod(model.get(), "dirRemoved", Q_ARG(QString, d0), Q_ARG(int, 0));
        QMetaObject::invokeMethod(model.get(), "dirAdded", Q_ARG(QString, d0));
        QCOMPARE(view.removed.count(), 1); // dirs are not in the grid: no splice
        QCOMPARE(view.inserted.count(), 1);

        // A rename is one call, not a remove plus an insert: the old row comes
        // from the event (the mirror has already spliced, so the listing cannot
        // answer "where was it") and the new row and the name come from the
        // listing. In files-only mode a directory rename is refused outright.
        QMetaObject::invokeMethod(model.get(), "fileRenamed", Q_ARG(QString, f0), Q_ARG(QString, f1), Q_ARG(int, 0));
        QCOMPARE(view.renamed, QList<int>({0, 1}));
        QCOMPARE(view.renamedTo, QList<QString>({QFileInfo(f1).fileName()}));
        QMetaObject::invokeMethod(model.get(), "dirRenamed", Q_ARG(QString, d0), Q_ARG(QString, d1), Q_ARG(int, 0));
        QCOMPARE(view.renamed.count(), 2);

        view.removed.clear();
        view.inserted.clear();
        view.reloaded.clear();
        view.renamed.clear();
        view.renamedTo.clear();

        // dirs shown: every file index shifts by dirCount, dir indices do not
        presenter.setShowDirs(true);
        QMetaObject::invokeMethod(model.get(), "fileModified", Q_ARG(QString, f0));
        QCOMPARE(view.reloaded, QList<int>({dirCount}));
        QMetaObject::invokeMethod(model.get(), "fileAdded", Q_ARG(QString, f1));
        QCOMPARE(view.inserted, QList<int>({dirCount + 1}));
        QMetaObject::invokeMethod(model.get(), "fileRemoved", Q_ARG(QString, f0), Q_ARG(int, 0));
        QCOMPARE(view.removed, QList<int>({dirCount}));
        QMetaObject::invokeMethod(model.get(), "dirRemoved", Q_ARG(QString, d0), Q_ARG(int, 0));
        QMetaObject::invokeMethod(model.get(), "dirAdded", Q_ARG(QString, d0));
        QCOMPARE(view.removed, QList<int>({dirCount, 0}));
        QCOMPARE(view.inserted, QList<int>({dirCount + 1, 0}));

        // with dirs in front, both rename sides of a *file* shift by dirCount
        // while a dir row — already in the leading block — does not shift at all
        view.renamed.clear();
        view.renamedTo.clear();
        QMetaObject::invokeMethod(model.get(), "fileRenamed", Q_ARG(QString, f0), Q_ARG(QString, f1), Q_ARG(int, 0));
        QMetaObject::invokeMethod(model.get(), "dirRenamed", Q_ARG(QString, d0), Q_ARG(QString, d1), Q_ARG(int, 0));
        QCOMPARE(view.renamed, QList<int>({dirCount, dirCount + 1, 0, 1}));
        QCOMPARE(view.renamedTo, QList<QString>({QFileInfo(f1).fileName(), QFileInfo(d1).fileName()}));
    }

    void viewlessProxyRemapsItsBufferedSelectionOnRename() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        const QString dir = uniqueDir("proxyrename");
        writeImage(QDir(dir).filePath("a.png"));
        writeImage(QDir(dir).filePath("b.png"));
        writeImage(QDir(dir).filePath("c.png"));
        QVERIFY(model->setDirectory(dir));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 3, 10000);

        coretest::CountingStripProxy view; // never shown: the buffer is all there is
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&view);
        presenter.setModel(model); // populate + select(0)
        QCOMPARE(view.selection(), QList<int>({0}));
        QCOMPARE(model->fileNameAt(0), QStringLiteral("a.png"));

        QVERIFY(QFile::rename(QDir(dir).filePath("a.png"), QDir(dir).filePath("z.png")));

        QTRY_COMPARE_WITH_TIMEOUT(view.renamed.count(), 2, 10000);
        // a.png was row 0 and z.png is the last row: the buffer moved with it,
        // so a strip materialized later replays the highlight onto the renamed row
        QCOMPARE(view.renamed, QList<int>({0, 2}));
        QCOMPARE(view.selection(), QList<int>({2}));
    }

    void disconnectViewAfterTheViewDiedIsSafe() {
        WorkerPool pool(2);
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        {
            auto *proxy = new ThumbnailStripProxy();
            presenter.setView(proxy);
            delete proxy; // the view dies while the presenter lives
        }
        presenter.disconnectView(); // must not reach the freed sender
        // and rebinding to a fresh view still works afterwards
        ThumbnailStripProxy second;
        presenter.setView(&second);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        presenter.setModel(model);
        QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);
        QVERIFY(activated.isValid());
        QMetaObject::invokeMethod(&second, "itemActivated", Q_ARG(int, 0));
        QCOMPARE(activated.count(), 1);
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
        TestPresenter t;
        return QTest::qExec(&t, argc, argv) != 0 ? 1 : 0;
    }
    const QMetaObject *mo = &TestPresenter::staticMetaObject;
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
            TestPresenter t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        // a crashed child loses its buffered output entirely — without naming
        // the slot here, the evidence is a red ctest with nothing in the log
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "presenter_tests: slot '%s' exited abnormally (exit=%d, sig=%d)\n",
                    slot.toUtf8().constData(), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "presenter_tests.moc"
