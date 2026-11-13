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
    void flipsFollowTheConditionalSaveContract() {
        const QString dir = uniqueDir("flips");
        const QString img = dir + QStringLiteral("/half.png");
        coretest::writeRectImage(img);

        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        QCOMPARE(coretest::contentOf(img), diskBefore);

        // the folder view is where an edit hits the disk, so the question has
        // to come first — and a declined question must mean nothing was written
        core.enableFolderView();
        QCOMPARE(core.mainWindow()->currentViewMode(), MODE_FOLDERVIEW);
        fake.confirmAnswer = false;
        core.flipH();
        QCOMPARE(fake.confirmCalls, 1);
        QCOMPARE(coretest::contentOf(img), diskBefore);
    }

    void folderViewEditWithConsentRewritesTheFile() {
        const QString dir = uniqueDir("consent");
        const QString img = dir + QStringLiteral("/half.png");
        coretest::writeRectImage(img);
        const QByteArray diskBefore = coretest::contentOf(img);

        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.enableFolderView();
        core.folderPresenter().selectAndFocus(img);
        QTRY_VERIFY2_WITH_TIMEOUT(!core.currentSelection().isEmpty(),
                                  "precondition broken: the folder view holds no selection", 10000);

        fake.confirmAnswer = true;
        core.flipH();

        QCOMPARE(fake.confirmCalls, 1);
        QTRY_VERIFY2_WITH_TIMEOUT(coretest::contentOf(img) != diskBefore,
                                  "a consented folder-view edit never reached the disk", 10000);
    }

    void editingInDocumentModeEditsInMemoryWithoutConfirmation() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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

    void editingInFolderViewSavesAfterScriptedConfirmation() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.toggleFolderView();

        core.rotateLeft();

        QCOMPARE(fake.confirmCalls, 1);             // folder view saves immediately, so it asks first
        QCOMPARE(QImage(img).size(), QSize(8, 16)); // the disk file is the rotated one
    }

    void editingDeclinedInFolderViewLeavesTheDiskAlone() {
        Core core;
        coretest::FakeInteractions fake;
        fake.confirmAnswer = false;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.toggleFolderView();

        core.rotateLeft();

        QCOMPARE(fake.confirmCalls, 1);
        QCOMPARE(QImage(img).size(), QSize(16, 8)); // declined: the disk is untouched
    }

    void resizeEditsTheLoadedImage() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.resize(QSize(4, 4));

        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(4, 4), 10000);
        QCOMPARE(fake.confirmCalls, 0); // resize never asks
    }

    void discardEditsRestoresTheOnDiskImage() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.rotateLeft();
        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(8, 16), 10000);

        core.discardEdits();

        QCOMPARE(core.directoryModel()->getImage(img)->size(), QSize(16, 8)); // back to the disk state
    }

    // The edited-image export used to hand every consumer one fixed path,
    // tmpDir/image.png: a drag in flight and a clipboard entry from two
    // exports shared one file and one of them read the other's pixels. The
    // file is now a uniquely-named QTemporaryFile owned by the QMimeData it
    // serves — it exists exactly as long as that mime data does, and the
    // autoremove takes it away with it.
    void theEditedImageExportGetsAUniqueTempFileOwnedByItsMime() {
        Core core;
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
        return QTest::qExec(&t, argc, argv) != 0 ? 1 : 0;
    }
    const QMetaObject *mo = &TestEditSave::staticMetaObject;
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
            TestEditSave t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        // a crashed child loses its buffered output entirely — without naming
        // the slot here, the evidence is a red ctest with nothing in the log
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "edit_save_tests: slot '%s' exited abnormally (exit=%d, sig=%d)\n",
                    slot.toUtf8().constData(), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "edit_save_tests.moc"
