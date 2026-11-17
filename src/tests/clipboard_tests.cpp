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

class TestClipboard : public QObject {
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
        base = QDir(home.path()).filePath("clipboard");
        alpha = QDir(base).filePath("alpha");
        beta = QDir(base).filePath("beta");
        QVERIFY(QDir().mkpath(alpha));
        writeImage(QDir(alpha).filePath("a.png"));
        QVERIFY(QDir().mkpath(beta));
    }

    void dropIntoTheAppOpensTheDroppedFileAndIgnoresItself() {
        const QString dir = uniqueDir("dropin");
        const QString target = dir + QStringLiteral("/dropped.png");
        writeImage(target);

        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const QString before = core.currentFilePath();

        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(target)});

        // the self-drop guard: a drag that started in this app must not re-open
        core.onDropIn(&mime, &core);
        QCOMPARE(core.currentFilePath(), before);

        core.onDropIn(&mime, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), target, 10000);
    }

    // Core::openFromClipboard is 84 lines that gcov had never entered. The URL
    // branch is the whole of it that needs no dialog (the image branch stops at
    // MW::getSaveFileName), and the offscreen platform does provide a working
    // clipboard.
    //
    // (Suite note: this whole suite stays on QApplication-under-offscreen on
    // purpose — two of its tests have the platform clipboard as their very
    // subject, and QClipboard needs a QGuiApplication. The suite builds no
    // widgets; the app type is the platform minimum, not a window dependency.) Each of the three ways in is pinned: a local file url opens it,
    // a non-local url is refused with the current file untouched, and a
    // text-only clipboard does nothing at all.
    void clipboardUrlOpensThatFileAndOtherFormsAreRefused() {
        const QString dir = uniqueDir("clip");
        const QString target = dir + QStringLiteral("/clip-target.png");
        writeImage(target);

        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const QString before = core.currentFilePath();

        // 1. a local file url is opened
        QMimeData *local = new QMimeData();
        local->setUrls({QUrl::fromLocalFile(target)});
        QApplication::clipboard()->setMimeData(local);
        core.openFromClipboard();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), target, 10000);

        // 2. a remote url: toLocalFile() is empty, so nothing is opened and the
        // screen keeps showing what it showed
        QMimeData *remote = new QMimeData();
        remote->setUrls({QUrl(QStringLiteral("https://example.com/notlocal.png"))});
        QApplication::clipboard()->setMimeData(remote);
        core.openFromClipboard();
        // "nothing happens" is checked after the queue is drained, not after an
        // arbitrary 200 ms: a sleep would pass for the wrong reason on a fast
        // machine and fail on a slow one, and neither says anything about the
        // behavior
        coretest::drainEvents();
        QCOMPARE(core.currentFilePath(), target);

        // 3. text only: no urls, no image, no action
        QMimeData *textOnly = new QMimeData();
        textOnly->setText(QStringLiteral("just a sentence"));
        QApplication::clipboard()->setMimeData(textOnly);
        core.openFromClipboard();
        // "nothing happens" is checked after the queue is drained, not after an
        // arbitrary 200 ms: a sleep would pass for the wrong reason on a fast
        // machine and fail on a slow one, and neither says anything about the
        // behavior
        coretest::drainEvents();
        QCOMPARE(core.currentFilePath(), target);
        QVERIFY(!before.isEmpty());
    }

    // copyFileClipboard builds the GNOME/KDE companion formats out of
    // mimeData->text(), while getMimeDataForImage only ever sets urls. Whether
    // text() sees the url list at all is a Qt behaviour, so this test decides it
    // by measurement rather than by reading the comment that claims it. If the
    // companion format ends up carrying no url, a file manager is handed the
    // bare word "copy" and cannot paste the file.
    // The edited-image branch is pinned in the same run because it is the one
    // that writes: the temp file is uniquely named per clipboard entry and
    // owned by the mime data it serves, so it autoremoves when that entry is
    // replaced — unique names without accumulating litter.
    void clipboardFileCopyAdvertisesTheFileInEveryFormat() {
        const QString dir = uniqueDir("clipcopy");
        const QString img = dir + QStringLiteral("/one.png");
        writeImage(img);

        Core core;
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.copyFileClipboard();
        const QMimeData *cb = QApplication::clipboard()->mimeData();
        QVERIFY2(cb->hasUrls(), "a copied file offered no urls");
        QCOMPARE(cb->urls().constFirst(), QUrl::fromLocalFile(img));
        qDebug() << "[clipcopy] text() =" << cb->text()
                 << "gnome-copied-files =" << cb->data("x-special/gnome-copied-files");
        QVERIFY2(cb->data("x-special/gnome-copied-files").contains("file://"),
                 qPrintable(QStringLiteral("the gnome companion format carries no file url, so a "
                                           "file manager cannot paste it; payload was \"") +
                            QString::fromUtf8(cb->data("x-special/gnome-copied-files")) + '\"'));
        QCOMPARE(cb->data("application/x-kde-cutselection"), QByteArray("0"));

        // an edited image is written to a uniquely-named temp file and the url
        // points at what was written — never at the one fixed name two
        // concurrent consumers used to share
        core.flipH();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.copyFileClipboard();
        const QUrl editedUrl = QApplication::clipboard()->mimeData()->urls().constFirst();
        const QString editedPath = editedUrl.toLocalFile();
        QVERIFY2(editedPath.startsWith(settings->tmpDir()),
                 qPrintable(QStringLiteral("the edited copy advertised a file outside the tmp dir: ") + editedPath));
        QVERIFY2(editedPath != settings->tmpDir() + QStringLiteral("image.png"),
                 qPrintable(QStringLiteral("the edited copy was handed the old fixed name: ") + editedPath));
        QVERIFY2(QFileInfo::exists(editedPath),
                 qPrintable(QStringLiteral("the edited copy advertised a temp file that was never "
                                           "written: ") +
                            editedPath));
        QVERIFY2(QFileInfo(editedPath).size() > 0, "the edited temp image was written empty");

        // copying again replaces the clipboard entry, and the entry the file is
        // owned by dies with the replacement: a new unique name, and the old
        // file autoremoved — unique names without accumulating litter
        core.flipV();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.copyFileClipboard();
        const QString secondPath = QApplication::clipboard()->mimeData()->urls().constFirst().toLocalFile();
        QVERIFY2(secondPath != editedPath, "two clipboard entries shared one temp name");
        QVERIFY2(!QFileInfo::exists(editedPath),
                 qPrintable(QStringLiteral("the replaced clipboard entry left its temp file behind: ") + editedPath));
        QVERIFY(QFileInfo::exists(secondPath));
    }

    void draggingAnUndecodedEntryStillOffersTheFile() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        // far.png sits beyond the preload neighborhood: a.png preloads its
        // immediate neighbor (mid.png), never the far one — undecoded
        writeImage(QDir(alpha).filePath("mid.png"));
        const QString farPath = QDir(alpha).filePath("far.png");
        writeImage(farPath);
        QVERIFY2(!core.directoryModel()->isLoaded(farPath),
                 "precondition broken: the far image is decoded, the test measures nothing");

        std::unique_ptr<QMimeData> mime(core.mimeDataForPaths({farPath}));

        QVERIFY2(mime->hasUrls(), "dragging an undecoded entry produced a mime without urls");
        QCOMPARE(mime->urls().constFirst(), QUrl::fromLocalFile(farPath));
    }

    // An image whose decoded size exceeds the allocation limit: the Image
    // object EXISTS (the entry is listed) but its QImage was refused by
    // QImageReader's allocation limit — and the drag must STILL offer the FILE
    // on disk. Measured: the mime carries the file path regardless of the
    // decode refusal (the refusal lives at the decode layer, the mime at the
    // path layer — two different floors of the same building).
    void draggingAnOverLimitImageStillOffersTheFile() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        // the user-side limit knob, tightened for the test: 8MB refuses the
        // ~70MB decode deterministically, whatever Qt 6.10's own default is
        settings->setMemoryAllocationLimit(8);
        const QString huge = QDir(alpha).filePath("huge.png");
        QImage big(4200, 4200, QImage::Format_ARGB32); // ~70MB decoded, over the limit
        big.fill(Qt::red);
        QVERIFY(big.save(huge, "png"));

        std::unique_ptr<QMimeData> mime(core.mimeDataForPaths({huge}));

        QVERIFY2(mime->hasUrls(), "an over-limit entry produced a mime without urls");
        QCOMPARE(mime->urls().constFirst(), QUrl::fromLocalFile(huge));
        QVERIFY(QFileInfo::exists(huge)); // the offered payload is the real file
    }

    // A drag is an OPERATION, not a session object: QDrag::exec() blocks until
    // the gesture ends, so nothing it carries may outlive the call that started
    // it. The old shape (mDrag = new QDrag(this)) overwrote the member on every
    // gesture and left the previous QDrag — plus the QMimeData it owns, which
    // for image drags can carry pixels — parented to Core for the whole session.
    // Pin: repeated drag-outs retain nothing on Core.
    void repeatedDragOutsRetainNothing() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const QString file = QDir(alpha).filePath("a.png");
        const int before = core.children().size();

        for(int i = 0; i < 8; ++i)
            core.onDraggedOut({file});

        int drags = 0;
        for(QObject *child : core.children())
            if(child->metaObject()->className() == QLatin1String("QDrag"))
                ++drags;
        QVERIFY2(drags == 0, qPrintable(QStringLiteral("QDrag objects retained by Core after 8 drag-outs")));
        QCOMPARE(core.children().size(), before);
    }

    // The folder view's Ctrl+V: the clipboard holds file paths, the browsing
    // directory is the destination. This is a different command from
    // openFromClipboard (which SHOWS the clipboard's file), and the whole
    // point of two tables declaring Ctrl+V is that each does its own kind of
    // paste. The assertions are on the filesystem, not on the tables: what
    // matters is that the files arrive and that nothing is copied onto itself.
    void pastingFileUrlsPlacesThemInTheBrowsingDirectory() {
        const QString source = uniqueDir("paste-src");
        const QString dest = uniqueDir("paste-dest");
        const QString srcFile = source + QStringLiteral("/copied.png");
        writeImage(srcFile);

        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        // make the destination the browsed directory
        QVERIFY(core.loadPath(dest)); // loadPath on a directory switches there
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        QMimeData *mime = new QMimeData();
        mime->setUrls({QUrl::fromLocalFile(srcFile)});
        QApplication::clipboard()->setMimeData(mime);

        core.pasteFilesToCurrentDirectory();

        const QString pasted = dest + QStringLiteral("/copied.png");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(pasted), 10000);
        QVERIFY2(QFileInfo::exists(srcFile), "paste removed the source — that is a move");
    }

    // A file already in the destination is skipped BEFORE interactiveCopy
    // runs. copyFileTo would answer NOTHING_TO_DO on it, and interactiveCopy
    // reports every non-SUCCESS result through showError — so without the
    // filter, pressing Ctrl+V on a file that is already here surfaces
    // "Nothing to do." as if it were a failure. The filesystem cannot show
    // that difference (NOTHING_TO_DO changes nothing), so the assertion is on
    // the overlay: a silent no-op, not an error.
    void pastingAFileAlreadyInTheDirectoryIsSilent() {
        const QString dest = uniqueDir("paste-self");
        const QString file = dest + QStringLiteral("/here.png");
        writeImage(file);

        coretest::FakeOverlayHost overlayFake;
        Core::Deps deps;
        deps.overlays = &overlayFake;
        Core core(deps);
        QVERIFY(core.loadPath(file));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        QMimeData *mime = new QMimeData();
        mime->setUrls({QUrl::fromLocalFile(file)});
        QApplication::clipboard()->setMimeData(mime);

        core.pasteFilesToCurrentDirectory();
        coretest::drainEvents();

        QVERIFY2(overlayFake.errorCalls == 0,
                 qPrintable(QStringLiteral("pasting into the same directory reported: ") + overlayFake.lastError));
        QVERIFY(QFileInfo::exists(file));
        QVERIFY2(!QFileInfo::exists(dest + QStringLiteral("/here (1).png")),
                 "pasting a file into its own directory created a copy");
    }

    // A remote url has no local path and is refused the same way
    // openFromClipboard refuses it.
    void pastingARemoteUrlDoesNothing() {
        const QString dest = uniqueDir("paste-remote");
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(core.loadPath(dest));

        QMimeData *mime = new QMimeData();
        mime->setUrls({QUrl(QStringLiteral("https://example.com/x.png"))});
        QApplication::clipboard()->setMimeData(mime);

        core.pasteFilesToCurrentDirectory();
        coretest::drainEvents();
        QVERIFY(QDir(dest).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
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
        TestClipboard t;
        return slot_harness::runFiltered(t, argc, argv) != 0 ? 1 : 0;
    }
    return slot_harness::run_units_one_process_each(
        argc, argv, slot_harness::testSlots(&TestClipboard::staticMetaObject), "clipboard_tests",
        [argv](const QString &slot) -> int {
            coretest::setChildCoreLimit();
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            TestClipboard t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        });
}

#include "clipboard_tests.moc"
