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

class TestEditSave : public ::testing::Test {
  protected:
    IsolatedHome m_home;
    coretest::AppSingletons m_singletons;
    QString m_base, m_alpha;

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
        m_base = QDir(m_home.path()).filePath("editsave");
        m_alpha = QDir(m_base).filePath("alpha");
        EXPECT_TRUE(QDir().mkpath(m_alpha));
        EXPECT_TRUE(QDir().mkpath(m_base));
    }
};

// The no-extra-arguments instantiation of Core::edit_template is the one
// flipH and flipV use, and gcov showed it had never been entered: the two
// flips were the only image operations with no test at all. What is pinned
// here is the `save` flag's contract, because that flag decides whether the
// user's file gets written: from the folder view an edit saves immediately,
// so it has to ask first; in document mode the edit stays in memory, asks
// nothing and must leave the file untouched. The fixture is 16x8 with the
// right half blue, so a horizontal flip is visible in pixels while the
// size stays the same.
// The edit contract is memory-only in every view. The folder view's
// flush-immediately mode retired with the view-scoped keymap: a thumbnail
// cannot show a transform's result, so writing behind an invisible
// confirmation was a silent rewrite. Ctrl+S is the only path to the disk,
// and this pin is the memory half — the pixels change, the disk does not.
TEST_F(TestEditSave, flipsEditInMemoryOnly) {
    const QString dir = uniqueDir("flips");
    const QString img = dir + QStringLiteral("/half.png");
    coretest::writeRectImage(img);

    coretest::FakeInteractions fake;
    coretest::FakeOverlayHost overlayFake;
    coretest::RecordingView view;
    Core::Deps deps;
    deps.headless = true;
    deps.ui = &fake;
    deps.overlays = &overlayFake;
    deps.view = &view;
    Core core(deps);
    EXPECT_TRUE(core.loadPath(img));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.directoryModel()->isLoaded(img);
        },
        10000));
    const QByteArray diskBefore = coretest::contentOf(img);

    core.flipH();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    EXPECT_EQ(fake.confirmCalls, 0);
    EXPECT_EQ(coretest::contentOf(img), diskBefore);

    auto edited = std::dynamic_pointer_cast<ImageStatic>(core.directoryModel()->getImage(img));
    ASSERT_TRUE(edited && edited->isEdited()) << "flipH left no edited image in memory";
    const QImage shown = *edited->getImage();
    EXPECT_EQ(shown.size(), QSize(16, 8));
    EXPECT_EQ(shown.pixelColor(0, 0), QColor(Qt::blue)); // mirrored: blue moved left
    EXPECT_EQ(shown.pixelColor(15, 0), QColor(Qt::red));

    core.flipV();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    EXPECT_EQ(fake.confirmCalls, 0);
    EXPECT_EQ(coretest::contentOf(img), diskBefore); // never asked, never written

    // and the folder view cannot edit at all: the key is not in its map
    core.discardEdits();
    core.enableFolderView();
    EXPECT_EQ(core.viewState().viewMode.value(), MODE_FOLDERVIEW);
    const QByteArray beforeFolderFlip = coretest::contentOf(img);
    core.flipH();
    EXPECT_EQ(fake.confirmCalls, 0); // no confirmation: no write path exists
    EXPECT_EQ(coretest::contentOf(img), beforeFolderFlip);
}
TEST_F(TestEditSave, editingInDocumentModeEditsInMemoryWithoutConfirmation) {
    coretest::FakeInteractions fake;
    coretest::FakeOverlayHost overlayFake;
    coretest::RecordingView view;
    Core::Deps deps;
    deps.headless = true;
    deps.ui = &fake;
    deps.overlays = &overlayFake;
    deps.view = &view;
    Core core(deps);
    const QString img = QDir(m_alpha).filePath("rect.png");
    coretest::writeRectImage(img);
    EXPECT_TRUE(core.loadPath(img));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.directoryModel()->isLoaded(img);
        },
        10000));
    const QByteArray diskBefore = coretest::contentOf(img);

    core.rotateLeft();

    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.directoryModel()->getImage(img)->size() == QSize(8, 16);
        },
        10000));
    EXPECT_EQ(core.directoryModel()->getImage(img)->size(), QSize(8, 16));
    EXPECT_EQ(fake.confirmCalls, 0);                 // document mode: in memory, no save, no ask
    EXPECT_EQ(coretest::contentOf(img), diskBefore); // the disk file is untouched until save
}
TEST_F(TestEditSave, resizeEditsTheLoadedImage) {
    coretest::FakeInteractions fake;
    coretest::FakeOverlayHost overlayFake;
    coretest::RecordingView view;
    Core::Deps deps;
    deps.headless = true;
    deps.ui = &fake;
    deps.overlays = &overlayFake;
    deps.view = &view;
    Core core(deps);
    const QString img = QDir(m_alpha).filePath("rect.png");
    coretest::writeRectImage(img);
    EXPECT_TRUE(core.loadPath(img));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));

    core.resize(QSize(4, 4));

    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.directoryModel()->getImage(img)->size() == QSize(4, 4);
        },
        10000));
    EXPECT_EQ(core.directoryModel()->getImage(img)->size(), QSize(4, 4));
    EXPECT_EQ(fake.confirmCalls, 0); // resize never asks
}

// resize commits to MEMORY only, so it needs a surface that can show the
// result and a later Ctrl+S. A thumbnail grid shows neither. The
// separation is structural now: resize's key is not in the folder view's
// map at all, so there is nothing to refuse and no guard to forget.
TEST_F(TestEditSave, resizeBelongsToTheDocumentViewOnly) {
    coretest::FakeInteractions fake;
    coretest::FakeOverlayHost overlayFake;
    coretest::RecordingView view;
    Core::Deps deps;
    deps.headless = true;
    deps.ui = &fake;
    deps.overlays = &overlayFake;
    deps.view = &view;
    Core core(deps);
    const QString img = QDir(m_alpha).filePath("rect.png");
    coretest::writeRectImage(img);
    EXPECT_TRUE(core.loadPath(img));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    const QSize before = core.directoryModel()->getImage(img)->size();
    const int messagesBefore = overlayFake.messageCalls;

    core.enableFolderView();
    EXPECT_EQ(core.viewState().viewMode.value(), MODE_FOLDERVIEW);

    // the key does not exist in the folder view's map: R names nothing,
    // so there is nothing to press and nothing to refuse — no message,
    // and the image is untouched
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_R)), QString());
    EXPECT_EQ(overlayFake.messageCalls, messagesBefore);
    EXPECT_EQ(core.directoryModel()->getImage(img)->size(), before);

    // and the command's own entry point has no view-state guard left —
    // DialogHost::resizeRequested connects straight to it, so a caller
    // that reaches it still edits. The gate is the binding's existence.
    core.toggleFolderView();
    EXPECT_EQ(core.viewState().viewMode.value(), MODE_DOCUMENT);
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_R)), QStringLiteral("resize"));
    core.resize(QSize(4, 4));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.directoryModel()->getImage(img)->size() == QSize(4, 4);
        },
        10000));
    EXPECT_EQ(core.directoryModel()->getImage(img)->size(), QSize(4, 4));
}

TEST_F(TestEditSave, discardEditsRestoresTheOnDiskImage) {
    coretest::FakeInteractions fake;
    coretest::FakeOverlayHost overlayFake;
    coretest::RecordingView view;
    Core::Deps deps;
    deps.headless = true;
    deps.ui = &fake;
    deps.overlays = &overlayFake;
    deps.view = &view;
    Core core(deps);
    const QString img = QDir(m_alpha).filePath("rect.png");
    coretest::writeRectImage(img);
    EXPECT_TRUE(core.loadPath(img));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));

    core.rotateLeft();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.directoryModel()->getImage(img)->size() == QSize(8, 16);
        },
        10000));
    EXPECT_EQ(core.directoryModel()->getImage(img)->size(), QSize(8, 16));

    core.discardEdits();

    EXPECT_EQ(core.directoryModel()->getImage(img)->size(), QSize(16, 8)); // back to the disk state
}

// The crop surface is a seam now (ICropSurface, Deps.crop): a headless
// Core can hold one, toggle it under Core's own policy, and take its
// intent through Core's public edit legs — all of which used to be
// unreachable without a window (the old path dereferenced mw->crop()).
TEST_F(TestEditSave, cropSurfaceTogglesThroughTheSeamAndIntentReachesTheEditLeg) {
    const QString img = QDir(m_alpha).filePath("rect.png");
    coretest::writeRectImage(img);

    // no image on screen yet: Core's policy refuses to open the surface
    coretest::FakeInteractions fake;
    coretest::FakeOverlayHost overlayFake;
    coretest::RecordingView view;
    coretest::FakeCropSurface crop;
    Core::Deps deps;
    deps.headless = true;
    deps.ui = &fake;
    deps.overlays = &overlayFake;
    deps.view = &view;
    deps.crop = &crop;
    Core core(deps);
    core.toggleCropPanel();
    EXPECT_EQ(crop.toggleCalls, 0); // the no-image guard, now testable headless

    EXPECT_TRUE(core.loadPath(img));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    core.toggleCropPanel();
    EXPECT_EQ(crop.toggleCalls, 1);
    EXPECT_TRUE(crop.isActive());
    core.toggleCropPanel();
    EXPECT_EQ(crop.toggleCalls, 2);
    EXPECT_TRUE(!crop.isActive());

    // the intent leg: the surface's crop lands in Core's edit pipeline.
    // The fixture is 16x8; cropping to the left half leaves 8x8 in memory.
    core.toggleCropPanel();
    QObject::connect(&crop, &coretest::FakeCropSurface::cropRequested, &core, &Core::crop);
    emit crop.cropRequested(QRect(0, 0, 8, 8));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.directoryModel()->getImage(img)->size() == QSize(8, 8);
        },
        10000));
    EXPECT_EQ(core.directoryModel()->getImage(img)->size(), QSize(8, 8));
    EXPECT_TRUE(core.directoryModel()->getImage(img)->isEdited()); // in-memory: document mode never asks
    EXPECT_EQ(fake.confirmCalls, 0);
}

// The edited-image export used to hand every consumer one fixed path,
// tmpDir/image.png: a drag in flight and a clipboard entry from two
// exports shared one file and one of them read the other's pixels. The
// file is now a uniquely-named QTemporaryFile owned by the QMimeData it
// serves — it exists exactly as long as that mime data does, and the
// autoremove takes it away with it.
TEST_F(TestEditSave, theEditedImageExportGetsAUniqueTempFileOwnedByItsMime) {
    coretest::FakeInteractions fake;
    coretest::FakeOverlayHost overlayFake;
    coretest::RecordingView view;
    Core::Deps deps;
    deps.headless = true;
    deps.ui = &fake;
    deps.overlays = &overlayFake;
    deps.view = &view;
    Core core(deps);
    const QString img = QDir(m_alpha).filePath("rect.png");
    coretest::writeRectImage(img);
    EXPECT_TRUE(core.loadPath(img));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.directoryModel()->isLoaded(img);
        },
        10000));
    core.rotateLeft();
    auto image = core.directoryModel()->getImage(img);
    EXPECT_TRUE(image->isEdited());

    QMimeData *first = core.getMimeDataForImage(image, TARGET_DROP);
    QMimeData *second = core.getMimeDataForImage(image, TARGET_DROP);
    const QString firstPath = QUrl(first->urls().value(0)).toLocalFile();
    const QString secondPath = QUrl(second->urls().value(0)).toLocalFile();
    EXPECT_TRUE(firstPath != secondPath) << "two exports were handed one fixed temp name";
    EXPECT_TRUE(QFile::exists(firstPath));
    EXPECT_TRUE(QFile::exists(secondPath));

    delete first; // the drag machinery does this when the gesture ends
    EXPECT_TRUE(!QFile::exists(firstPath)) << "the temp file outlived its mime data";
    EXPECT_TRUE(QFile::exists(secondPath));
    delete second;
    EXPECT_TRUE(!QFile::exists(secondPath)) << "the temp file outlived its mime data";
}

int main(int argc, char **argv) {
    // QApplication, not QCoreApplication: the tested paths still touch no
    // widget — the save contract is pinned through FakeInteractions, the mode
    // through Core's own state — but the action registry now carries QActions,
    // and Qt's QAction constructor resolves its private-class factory through
    // QGuiApplicationPrivate::self, which a bare QCoreApplication leaves null:
    // the first action creation would segfault before any test could run.
    // Offscreen keeps the run headless, the same way the QApplication suites
    // run it.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
