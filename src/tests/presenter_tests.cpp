#include "gtest_qt.h"
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

class TestPresenter : public ::testing::Test {
  protected:
    IsolatedHome m_home;
    coretest::AppSingletons m_singletons;
    QString m_base, m_alpha, m_beta;

    void writeImage(const QString &path) {
        EXPECT_TRUE(makeImageFile(path, QSize(8, 8)));
    }
    QString uniqueDir(const QString &tag) {
        const QString dir = QDir(m_base).filePath(tag + QStringLiteral("-{}").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

    void SetUp() override {
        EXPECT_TRUE(m_home.path() != nullptr);
        m_base = QDir(m_home.path()).filePath("presenter");
        m_alpha = QDir(m_base).filePath("alpha");
        m_beta = QDir(m_base).filePath("beta");
        EXPECT_TRUE(QDir().mkpath(m_alpha));
        EXPECT_TRUE(QDir().mkpath(m_beta));
        writeImage(QDir(m_alpha).filePath("a.png"));
        writeImage(QDir(m_beta).filePath("b.png"));
        EXPECT_TRUE(QDir().mkpath(m_base));
    }
};

TEST_F(TestPresenter, presenterPopulatesTheViewWhenModelArrives) {
    WorkerPool pool(2);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    EXPECT_TRUE(model->setDirectory(m_alpha));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model->listing().fileCount() == 1;
        },
        10000));
    EXPECT_EQ(model->listing().fileCount(), 1);
    ThumbnailStrip proxy;
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);

    presenter.setView(&proxy);
    presenter.setModel(model);

    // the eager view populates straight from a listing in hand
    EXPECT_EQ(proxy.itemCount(), 1);
}

TEST_F(TestPresenter, presenterSetViewTwiceKeepsTheFirstBinding) {
    WorkerPool pool(2);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    EXPECT_TRUE(model->setDirectory(m_alpha));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model->listing().fileCount() == 1;
        },
        10000));
    EXPECT_EQ(model->listing().fileCount(), 1);
    ThumbnailStrip first, second;
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);

    presenter.setView(&first);
    presenter.setView(&second); // rejected: the binding is already live
    presenter.setModel(model);

    EXPECT_EQ(first.itemCount(), 1);
    EXPECT_EQ(second.itemCount(), 0) << "the second setView was rejected, not replaced";
}

TEST_F(TestPresenter, presenterForwardsActivationToTheFilePath) {
    WorkerPool pool(2);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    EXPECT_TRUE(model->setDirectory(m_alpha));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model->listing().fileCount() == 1;
        },
        10000));
    EXPECT_EQ(model->listing().fileCount(), 1);
    ThumbnailStrip proxy;
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);
    presenter.setView(&proxy);
    proxy.show(); // materialize the strip so the forwarding path is the real one
    presenter.setModel(model);
    QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);
    EXPECT_TRUE(activated.isValid());

    QMetaObject::invokeMethod(&proxy, "itemActivated", Q_ARG(int, 0));

    EXPECT_EQ(activated.count(), 1);
    EXPECT_EQ(activated.at(0).at(0).toString(), QDir(m_alpha).filePath("a.png"));
}

TEST_F(TestPresenter, presenterDisconnectViewStopsForwardingAndAllowsRebinding) {
    WorkerPool pool(2);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    EXPECT_TRUE(model->setDirectory(m_alpha));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model->listing().fileCount() == 1;
        },
        10000));
    EXPECT_EQ(model->listing().fileCount(), 1);
    ThumbnailStrip first, second;
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);
    presenter.setView(&first);
    presenter.setModel(model);
    QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);

    QMetaObject::invokeMethod(&first, "itemActivated", Q_ARG(int, 0));
    EXPECT_EQ(activated.count(), 1);

    presenter.disconnectView();
    QMetaObject::invokeMethod(&first, "itemActivated", Q_ARG(int, 0));
    EXPECT_EQ(activated.count(), 1); // the detached view no longer reaches us

    presenter.setView(&second); // rebinding was impossible before disconnectView
    presenter.setModel(model);
    EXPECT_EQ(second.itemCount(), 1) << "rebinding after disconnectView does not populate";
    QMetaObject::invokeMethod(&second, "itemActivated", Q_ARG(int, 0));
    EXPECT_EQ(activated.count(), 2);
    EXPECT_EQ(activated.at(1).at(0).toString(), QDir(m_alpha).filePath("a.png"));
}

TEST_F(TestPresenter, presenterUnsetModelKeepsTheViewBinding) {
    WorkerPool pool(2);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    EXPECT_TRUE(model->setDirectory(m_alpha));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model->listing().fileCount() == 1;
        },
        10000));
    EXPECT_EQ(model->listing().fileCount(), 1);
    ThumbnailStrip proxy;
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);
    presenter.setView(&proxy);
    proxy.show();
    presenter.setModel(model);

    presenter.unsetModel(); // detaches the model, keeps the view on purpose

    EXPECT_EQ(proxy.itemCount(), 1); // the binding survived the model swap
    auto model2 = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    EXPECT_TRUE(model2->setDirectory(m_beta));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model2->listing().fileCount() == 1;
        },
        10000));
    EXPECT_EQ(model2->listing().fileCount(), 1);
    presenter.setModel(model2);
    EXPECT_EQ(proxy.itemCount(), 1) << "re-setting a model on a live binding must repopulate";
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
TEST_F(TestPresenter, presenterRoutesThumbnailsAndDragHoverToTheView) {
    WorkerPool pool(2);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    EXPECT_TRUE(model->setDirectory(m_alpha));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model->listing().fileCount() == 1;
        },
        10000));
    EXPECT_EQ(model->listing().fileCount(), 1);
    coretest::CountingStripProxy view;
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);
    presenter.setView(&view);
    view.show(); // materialize so the forwarding path is the real one
    presenter.setModel(model);

    // The strip requests thumbnails for itself once it materializes, and it
    // may ask more than once as rows arrive. Waiting for "at least one" and
    // then clearing the counter left any still-queued request to land after
    // the clear, so the count below read 2 instead of 1 — measured as a
    // failure only under ctest -j4, where the event queue is slower. Wait for
    // the count to STOP moving instead, then clear.
    {
        int lastSeen = -1;
        EXPECT_TRUE(QTest::qWaitFor(
            [&] {
                const bool stable = view.thumbnailCalls > 0 && view.thumbnailCalls == lastSeen;
                lastSeen = view.thumbnailCalls;
                return stable;
            },
            10000));
    }
    // anything the strip queued for itself is dispatched by now
    QCoreApplication::processEvents();
    view.thumbnailCalls = 0;
    view.lastThumbnailPos = -1;

    QMetaObject::invokeMethod(&view, "thumbnailsRequested", Q_ARG(QList<int>, {0}), Q_ARG(int, 128), Q_ARG(bool, true),
                              Q_ARG(bool, false));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return view.thumbnailCalls == 1;
        },
        10000));
    EXPECT_EQ(view.thumbnailCalls, 1);
    EXPECT_EQ(view.lastThumbnailPos, 0);

    // the draggedOver branch needs a directory index while dirs are shown,
    // and onDraggedOver skips anything already selected — populate selects
    // index 0, so the hover target is the second directory
    EXPECT_TRUE(QDir(m_alpha).mkpath("subdir"));
    EXPECT_TRUE(QDir(m_alpha).mkpath("subdir2"));
    presenter.setShowDirs(true);
    // the two new folders arrive through the engine's own watch; the
    // presenter syncs on the publication, so there is nothing to re-populate
    // by hand
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model->listing().dirCount() == 2;
        },
        10000));
    EXPECT_EQ(model->listing().dirCount(), 2);
    // onDraggedOver has three guards (model, selection, dirs-visible) and
    // a hover index that must name a directory: each is asserted right
    // before the invoke so a lost delivery names the guard it hit instead
    // of reporting a bare zero.
    EXPECT_TRUE(presenter.showDirs()) << "precondition broken: the presenter is in files-only mode";
    EXPECT_TRUE(model->listing().dirCount() > 1) << qPrintable(
        QStringLiteral("precondition broken: only %1 directories listed").arg(model->listing().dirCount()));
    EXPECT_TRUE(!view.selection().contains(1))
        << "precondition broken: index 1 is selected, onDraggedOver returns early";

    QMetaObject::invokeMethod(&view, "draggedOver", Q_ARG(int, 1));
    EXPECT_TRUE(view.hoverCalls == 1) << qPrintable(QStringLiteral("setDragHover never reached the view (dirCount=%1, "
                                                                   "showDirs=%2, selection=%3)")
                                                        .arg(model->listing().dirCount())
                                                        .arg(presenter.showDirs())
                                                        .arg(view.selection().isEmpty()
                                                                 ? QStringLiteral("empty")
                                                                 : QStringLiteral("%1").arg(view.selection().at(0))));
}

TEST_F(TestPresenter, presenterForwardsDragFromTheView) {
    WorkerPool pool(2);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    EXPECT_TRUE(model->setDirectory(m_alpha));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model->listing().fileCount() == 1;
        },
        10000));
    EXPECT_EQ(model->listing().fileCount(), 1);
    ThumbnailStrip proxy;
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);
    presenter.setView(&proxy);
    proxy.show(); // materialize so the forwarding path is the real one
    presenter.setModel(model);

    QSignalSpy out(&presenter, &DirectoryPresenter::draggedOut);
    QSignalSpy into(&presenter, &DirectoryPresenter::droppedInto);
    EXPECT_TRUE(out.isValid() && into.isValid());

    // view draggedOut() -> presenter onDraggedOut -> presenter draggedOut(paths)
    // Qt6 signals are public, so the forwarding chain is driven directly:
    // a by-name invoke would put the signature in a string for no service
    emit proxy.draggedOut();
    EXPECT_EQ(out.count(), 1);

    // view droppedInto(mime, source, index) -> presenter droppedInto(paths, dir)
    QMimeData data;
    data.setUrls({QUrl::fromLocalFile(QDir(m_alpha).filePath("a.png"))});
    emit proxy.droppedInto(&data, nullptr, 0);
    EXPECT_EQ(into.count(), 1);
}

// The presenter is the only place that turns an engine-side index — files
// and directories counted in separate lists, dirs first — into the view's
// single flat grid index. Nothing else checks that arithmetic, and a wrong
// offset is silent: the grid still splices, just one row away from the
// entry the event named. The events are emitted by hand because the
// contract under test is the translation, not the arrival — arrival and
// index provenance are pinned by the mirror-contract test.
TEST_F(TestPresenter, aSizeDrivenMoveKeepsSelectionAndActivationOnTheSameFile) {
    WorkerPool pool(2);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    const QDir dir(uniqueDir("size-reorder"));
    const QString selected = dir.filePath("a.png");
    const auto resize = [](const QString &path, qint64 size) {
        QFile file(path);
        return file.open(QIODevice::ReadWrite) && file.resize(size);
    };
    ASSERT_TRUE(dir.mkdir("folder"));
    ASSERT_TRUE(resize(selected, 10));
    ASSERT_TRUE(resize(dir.filePath("b.png"), 20));
    model->listing().setSortingMode(SORT_SIZE);
    EXPECT_TRUE(model->setDirectory(dir.path()));
    ASSERT_TRY_TRUE_TIMEOUT(model->listing().isSettled(), 10000);

    ThumbnailStrip view;
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);
    presenter.setShowDirs(true);
    presenter.setView(&view);
    presenter.setModel(model);
    presenter.selectAndFocus(selected);
    EXPECT_EQ(presenter.selectedPaths(), QList<QString>{selected});
    QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);

    ASSERT_TRUE(resize(selected, 30));
    ASSERT_TRY_EQ_TIMEOUT(model->listing().indexOfFile(selected), 1, 10000);
    EXPECT_EQ(view.selection(), QList<int>{2});
    EXPECT_EQ(presenter.selectedPaths(), QList<QString>{selected});
    emit view.itemActivated(2);
    ASSERT_EQ(activated.count(), 1);
    EXPECT_EQ(activated.at(0).at(0).toString(), selected);
}

TEST_F(TestPresenter, aRenameFollowsTheSelectionEvenInAnUnshownView) {
    WorkerPool pool(2);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    const QDir dir(uniqueDir("proxyrename"));
    writeImage(dir.filePath("a.png"));
    writeImage(dir.filePath("b.png"));
    writeImage(dir.filePath("c.png"));
    EXPECT_TRUE(model->setDirectory(dir.path()));
    ASSERT_TRY_TRUE_TIMEOUT(model->listing().isSettled(), 10000);

    ThumbnailStrip view;
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);
    presenter.setView(&view);
    presenter.setModel(model);
    const QString original = dir.filePath("a.png");
    const QString renamed = dir.filePath("z.png");
    EXPECT_EQ(presenter.selectedPaths(), QList<QString>{original});
    const quint64 originalId = model->listing().fileEntryAt(0).id;

    EXPECT_TRUE(QFile::rename(original, renamed));
    ASSERT_TRY_EQ_TIMEOUT(model->listing().indexOfFile(renamed), 2, 10000);
    EXPECT_EQ(model->listing().fileEntryAt(2).id, originalId);
    EXPECT_EQ(presenter.selectedPaths(), QList<QString>{renamed});
}

TEST_F(TestPresenter, disconnectViewAfterTheViewDiedIsSafe) {
    WorkerPool pool(2);
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter presenter(thumbnailer);
    {
        auto *proxy = new ThumbnailStrip();
        presenter.setView(proxy);
        delete proxy; // the view dies while the presenter lives
    }
    presenter.disconnectView(); // must not reach the freed sender
    // and rebinding to a fresh view still works afterwards
    ThumbnailStrip second;
    presenter.setView(&second);
    auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
    EXPECT_TRUE(model->setDirectory(m_alpha));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return model->listing().fileCount() == 1;
        },
        10000));
    EXPECT_EQ(model->listing().fileCount(), 1);
    presenter.setModel(model);
    QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);
    EXPECT_TRUE(activated.isValid());
    QMetaObject::invokeMethod(&second, "itemActivated", Q_ARG(int, 0));
    EXPECT_EQ(activated.count(), 1);
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
