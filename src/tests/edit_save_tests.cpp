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

// A presenter view without a widget. DirectoryPresenter::setView is a template
// duck-type seam (IDirectoryView is not a QObject, so the five signal connects
// need a concrete type); this is the concrete type a headless suite hands it —
// the same shape FolderViewProxy gives production, minus the widget. The
// selection is staged by the test, exactly what the grid holds in production.
class FakePresenterView : public QObject, public IDirectoryView {
    Q_OBJECT
  public:
    QList<int> stagedSelection;
    void populate(int) override {
    }
    void setThumbnail(int, std::shared_ptr<Thumbnail>) override {
    }
    void select(QList<int> sel) override {
        stagedSelection = sel;
    }
    void select(int index) override {
        stagedSelection = QList<int>{index};
    }
    void focusOn(int) override {
    }
    void focusOnSelection() override {
    }
    QList<int> selection() override {
        return stagedSelection;
    }
    void setDirectoryPath(QString) override {
    }
    void insertItem(int) override {
    }
    void removeItem(int) override {
    }
    void reloadItem(int) override {
    }
    void renameItem(int, int, const QString &) override {
    }
    void setDragHover(int) override {
    }
  signals:
    void itemActivated(int) override;
    void thumbnailsRequested(QList<int>, int, bool, bool) override;
    void draggedOut() override;
    void draggedOver(int) override;
    void droppedInto(const QMimeData *, QObject *, int) override;
};

class TestEditSave : public QObject {
    Q_OBJECT

  private:
    IsolatedHome home;
    coretest::AppSingletons singletons;
    QString base, alpha;

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
        base = QDir(home.path()).filePath("editsave");
        alpha = QDir(base).filePath("alpha");
        QVERIFY(QDir().mkpath(alpha));
        QVERIFY(QDir().mkpath(base));
    }

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
    void flipsEditInMemoryOnly() {
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
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->isLoaded(img), 10000);
        const QByteArray diskBefore = coretest::contentOf(img);

        core.flipH();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(fake.confirmCalls, 0);
        QCOMPARE(coretest::contentOf(img), diskBefore);

        auto edited = std::dynamic_pointer_cast<ImageStatic>(core.directoryModel()->getImage(img));
        QVERIFY2(edited && edited->isEdited(), "flipH left no edited image in memory");
        const QImage shown = *edited->getImage();
        QCOMPARE(shown.size(), QSize(16, 8));
        QCOMPARE(shown.pixelColor(0, 0), QColor(Qt::blue)); // mirrored: blue moved left
        QCOMPARE(shown.pixelColor(15, 0), QColor(Qt::red));

        core.flipV();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(fake.confirmCalls, 0);
        QCOMPARE(coretest::contentOf(img), diskBefore); // never asked, never written

        // and the folder view cannot edit at all: the key is not in its map
        core.discardEdits();
        core.enableFolderView();
        QCOMPARE(core.viewState().viewMode.value(), MODE_FOLDERVIEW);
        const QByteArray beforeFolderFlip = coretest::contentOf(img);
        core.flipH();
        QCOMPARE(fake.confirmCalls, 0); // no confirmation: no write path exists
        QCOMPARE(coretest::contentOf(img), beforeFolderFlip);
    }
    void editingInDocumentModeEditsInMemoryWithoutConfirmation() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->isLoaded(img), 10000);
        const QByteArray diskBefore = coretest::contentOf(img);

        core.rotateLeft();

        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(8, 16), 10000);
        QCOMPARE(fake.confirmCalls, 0);                 // document mode: in memory, no save, no ask
        QCOMPARE(coretest::contentOf(img), diskBefore); // the disk file is untouched until save
    }
    void resizeEditsTheLoadedImage() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.resize(QSize(4, 4));

        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(4, 4), 10000);
        QCOMPARE(fake.confirmCalls, 0); // resize never asks
    }

    // resize commits to MEMORY only, so it needs a surface that can show the
    // result and a later Ctrl+S. A thumbnail grid shows neither. The
    // separation is structural now: resize's key is not in the folder view's
    // map at all, so there is nothing to refuse and no guard to forget.
    void resizeBelongsToTheDocumentViewOnly() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const QSize before = core.directoryModel()->getImage(img)->size();
        const int messagesBefore = overlayFake.messageCalls;

        core.enableFolderView();
        QCOMPARE(core.viewState().viewMode.value(), MODE_FOLDERVIEW);

        // the key does not exist in the folder view's map: R names nothing,
        // so there is nothing to press and nothing to refuse — no message,
        // and the image is untouched
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_R)), QString());
        QCOMPARE(overlayFake.messageCalls, messagesBefore);
        QCOMPARE(core.directoryModel()->getImage(img)->size(), before);

        // and the command's own entry point has no view-state guard left —
        // DialogHost::resizeRequested connects straight to it, so a caller
        // that reaches it still edits. The gate is the binding's existence.
        core.toggleFolderView();
        QCOMPARE(core.viewState().viewMode.value(), MODE_DOCUMENT);
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_R)), QStringLiteral("resize"));
        core.resize(QSize(4, 4));
        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(4, 4), 10000);
    }

    void discardEditsRestoresTheOnDiskImage() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.rotateLeft();
        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(8, 16), 10000);

        core.discardEdits();

        QCOMPARE(core.directoryModel()->getImage(img)->size(), QSize(16, 8)); // back to the disk state
    }

    // The crop surface is a seam now (ICropSurface, Deps.crop): a headless
    // Core can hold one, toggle it under Core's own policy, and take its
    // intent through Core's public edit legs — all of which used to be
    // unreachable without a window (the old path dereferenced mw->crop()).
    void cropSurfaceTogglesThroughTheSeamAndIntentReachesTheEditLeg() {
        const QString img = QDir(alpha).filePath("rect.png");
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
        QCOMPARE(crop.toggleCalls, 0); // the no-image guard, now testable headless

        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.toggleCropPanel();
        QCOMPARE(crop.toggleCalls, 1);
        QVERIFY(crop.isActive());
        core.toggleCropPanel();
        QCOMPARE(crop.toggleCalls, 2);
        QVERIFY(!crop.isActive());

        // the intent leg: the surface's crop lands in Core's edit pipeline.
        // The fixture is 16x8; cropping to the left half leaves 8x8 in memory.
        core.toggleCropPanel();
        QObject::connect(&crop, &coretest::FakeCropSurface::cropRequested, &core, &Core::crop);
        emit crop.cropRequested(QRect(0, 0, 8, 8));
        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(8, 8), 10000);
        QVERIFY(core.directoryModel()->getImage(img)->isEdited()); // in-memory: document mode never asks
        QCOMPARE(fake.confirmCalls, 0);
    }

    // The edited-image export used to hand every consumer one fixed path,
    // tmpDir/image.png: a drag in flight and a clipboard entry from two
    // exports shared one file and one of them read the other's pixels. The
    // file is now a uniquely-named QTemporaryFile owned by the QMimeData it
    // serves — it exists exactly as long as that mime data does, and the
    // autoremove takes it away with it.
    void theEditedImageExportGetsAUniqueTempFileOwnedByItsMime() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->isLoaded(img), 10000);
        core.rotateLeft();
        auto image = core.directoryModel()->getImage(img);
        QVERIFY(image->isEdited());

        QMimeData *first = core.getMimeDataForImage(image, TARGET_DROP);
        QMimeData *second = core.getMimeDataForImage(image, TARGET_DROP);
        const QString firstPath = QUrl(first->urls().value(0)).toLocalFile();
        const QString secondPath = QUrl(second->urls().value(0)).toLocalFile();
        QVERIFY2(firstPath != secondPath, "two exports were handed one fixed temp name");
        QVERIFY(QFile::exists(firstPath));
        QVERIFY(QFile::exists(secondPath));

        delete first; // the drag machinery does this when the gesture ends
        QVERIFY2(!QFile::exists(firstPath), "the temp file outlived its mime data");
        QVERIFY(QFile::exists(secondPath));
        delete second;
        QVERIFY2(!QFile::exists(secondPath), "the temp file outlived its mime data");
    }
};

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
        TestEditSave t;
        return slot_harness::runFiltered(t, argc, argv) != 0 ? 1 : 0;
    }
    return slot_harness::run_units_one_process_each(
        argc, argv, slot_harness::testSlots(&TestEditSave::staticMetaObject), "edit_save_tests");
}

#include "edit_save_tests.moc"
