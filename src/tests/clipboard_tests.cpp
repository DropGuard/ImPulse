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

class TestClipboard : public ::testing::Test {
  protected:
    IsolatedHome home;
    coretest::AppSingletons singletons;
    QString base, alpha, beta;

    void writeImage(const QString &path) {
        EXPECT_TRUE(makeImageFile(path, QSize(8, 8)));
    }
    QString uniqueDir(const QString &tag) {
        const QString dir = QDir(base).filePath(tag + QStringLiteral("-{}").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

    void SetUp() override {
        EXPECT_TRUE(home.path() != nullptr);
        base = QDir(home.path()).filePath("clipboard");
        alpha = QDir(base).filePath("alpha");
        beta = QDir(base).filePath("beta");
        EXPECT_TRUE(QDir().mkpath(alpha));
        writeImage(QDir(alpha).filePath("a.png"));
        EXPECT_TRUE(QDir().mkpath(beta));
    }

    // the QTRY_* vocabulary's gtest form: pump events until the condition
    // holds or the deadline expires. QTRY_VERIFY_WITH_TIMEOUT wraps QVERIFY,
    // whose failure path is a bare return that reports nothing outside a
    // QTest::qExec run — measured in this migration.
    static bool waitLoaded(Core &core) {
        return QTest::qWaitFor(
            [&core] {
                return core.isSettled();
            },
            10000);
    }
};

TEST_F(TestClipboard, dropIntoTheAppOpensTheDroppedFileAndIgnoresItself) {
    const QString dir = uniqueDir("dropin");
    const QString target = dir + QStringLiteral("/dropped.png");
    writeImage(target);

    Core core;
    EXPECT_TRUE(core.loadPath(QDir(alpha).filePath("a.png")));
    EXPECT_TRUE(waitLoaded(core));
    const QString before = core.currentFilePath();

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(target)});

    // the self-drop guard: a drag that started in this app must not re-open
    core.onDropIn(&mime, &core);
    EXPECT_EQ(core.currentFilePath(), before);

    core.onDropIn(&mime, nullptr);
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.currentFilePath() == target;
        },
        10000));
    EXPECT_EQ(core.currentFilePath(), target);
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
TEST_F(TestClipboard, clipboardUrlOpensThatFileAndOtherFormsAreRefused) {
    const QString dir = uniqueDir("clip");
    const QString target = dir + QStringLiteral("/clip-target.png");
    writeImage(target);

    Core core;
    EXPECT_TRUE(core.loadPath(QDir(alpha).filePath("a.png")));
    EXPECT_TRUE(waitLoaded(core));
    const QString before = core.currentFilePath();

    // 1. a local file url is opened
    QMimeData *local = new QMimeData();
    local->setUrls({QUrl::fromLocalFile(target)});
    QApplication::clipboard()->setMimeData(local);
    core.openFromClipboard();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.currentFilePath() == target;
        },
        10000));
    EXPECT_EQ(core.currentFilePath(), target);

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
    EXPECT_EQ(core.currentFilePath(), target);

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
    EXPECT_EQ(core.currentFilePath(), target);
    EXPECT_TRUE(!before.isEmpty());
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
TEST_F(TestClipboard, clipboardFileCopyAdvertisesTheFileInEveryFormat) {
    const QString dir = uniqueDir("clipcopy");
    const QString img = dir + QStringLiteral("/one.png");
    writeImage(img);

    Core core;
    EXPECT_TRUE(core.loadPath(img));
    EXPECT_TRUE(waitLoaded(core));

    core.copyFileClipboard();
    const QMimeData *cb = QApplication::clipboard()->mimeData();
    EXPECT_TRUE(cb->hasUrls()) << "a copied file offered no urls";
    EXPECT_EQ(cb->urls().constFirst(), QUrl::fromLocalFile(img));
    qDebug() << "[clipcopy] text() =" << cb->text()
             << "gnome-copied-files =" << cb->data("x-special/gnome-copied-files");
    EXPECT_TRUE(cb->data("x-special/gnome-copied-files").contains("file://"))
        << qPrintable(QStringLiteral("the gnome companion format carries no file url, so a "
                                     "file manager cannot paste it; payload was \"") +
                      QString::fromUtf8(cb->data("x-special/gnome-copied-files")) + '\"');
    EXPECT_EQ(cb->data("application/x-kde-cutselection"), QByteArray("0"));

    // an edited image is written to a uniquely-named temp file and the url
    // points at what was written — never at the one fixed name two
    // concurrent consumers used to share
    core.flipH();
    EXPECT_TRUE(waitLoaded(core));
    core.copyFileClipboard();
    const QUrl editedUrl = QApplication::clipboard()->mimeData()->urls().constFirst();
    const QString editedPath = editedUrl.toLocalFile();
    EXPECT_TRUE(editedPath.startsWith(settings->tmpDir()))
        << qPrintable(QStringLiteral("the edited copy advertised a file outside the tmp dir: ") + editedPath);
    EXPECT_TRUE(editedPath != settings->tmpDir() + QStringLiteral("image.png"))
        << qPrintable(QStringLiteral("the edited copy was handed the old fixed name: ") + editedPath);
    EXPECT_TRUE(QFileInfo::exists(editedPath))
        << qPrintable(QStringLiteral("the edited copy advertised a temp file that was never "
                                     "written: ") +
                      editedPath);
    EXPECT_TRUE(QFileInfo(editedPath).size() > 0) << "the edited temp image was written empty";

    // copying again replaces the clipboard entry, and the entry the file is
    // owned by dies with the replacement: a new unique name, and the old
    // file autoremoved — unique names without accumulating litter
    core.flipV();
    EXPECT_TRUE(waitLoaded(core));
    core.copyFileClipboard();
    const QString secondPath = QApplication::clipboard()->mimeData()->urls().constFirst().toLocalFile();
    EXPECT_TRUE(secondPath != editedPath) << "two clipboard entries shared one temp name";
    EXPECT_TRUE(!QFileInfo::exists(editedPath))
        << qPrintable(QStringLiteral("the replaced clipboard entry left its temp file behind: ") + editedPath);
    EXPECT_TRUE(QFileInfo::exists(secondPath));
}

TEST_F(TestClipboard, draggingAnUndecodedEntryStillOffersTheFile) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(alpha).filePath("a.png")));
    EXPECT_TRUE(waitLoaded(core));
    // far.png sits beyond the preload neighborhood: a.png preloads its
    // immediate neighbor (mid.png), never the far one — undecoded
    writeImage(QDir(alpha).filePath("mid.png"));
    const QString farPath = QDir(alpha).filePath("far.png");
    writeImage(farPath);
    EXPECT_TRUE(!core.directoryModel()->isLoaded(farPath))
        << "precondition broken: the far image is decoded, the test measures nothing";

    std::unique_ptr<QMimeData> mime(core.mimeDataForPaths({farPath}));

    EXPECT_TRUE(mime->hasUrls()) << "dragging an undecoded entry produced a mime without urls";
    EXPECT_EQ(mime->urls().constFirst(), QUrl::fromLocalFile(farPath));
}

// An image whose decoded size exceeds the allocation limit: the Image
// object EXISTS (the entry is listed) but its QImage was refused by
// QImageReader's allocation limit — and the drag must STILL offer the FILE
// on disk. Measured: the mime carries the file path regardless of the
// decode refusal (the refusal lives at the decode layer, the mime at the
// path layer — two different floors of the same building).
TEST_F(TestClipboard, draggingAnOverLimitImageStillOffersTheFile) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(alpha).filePath("a.png")));
    EXPECT_TRUE(waitLoaded(core));
    // the user-side limit knob, tightened for the test: 8MB refuses the
    // ~70MB decode deterministically, whatever Qt 6.10's own default is
    settings->setMemoryAllocationLimit(8);
    const QString huge = QDir(alpha).filePath("huge.png");
    QImage big(4200, 4200, QImage::Format_ARGB32); // ~70MB decoded, over the limit
    big.fill(Qt::red);
    EXPECT_TRUE(big.save(huge, "png"));

    std::unique_ptr<QMimeData> mime(core.mimeDataForPaths({huge}));

    EXPECT_TRUE(mime->hasUrls()) << "an over-limit entry produced a mime without urls";
    EXPECT_EQ(mime->urls().constFirst(), QUrl::fromLocalFile(huge));
    EXPECT_TRUE(QFileInfo::exists(huge)); // the offered payload is the real file
}

// A drag is an OPERATION, not a session object: QDrag::exec() blocks until
// the gesture ends, so nothing it carries may outlive the call that started
// it. The old shape (mDrag = new QDrag(this)) overwrote the member on every
// gesture and left the previous QDrag — plus the QMimeData it owns, which
// for image drags can carry pixels — parented to Core for the whole session.
// Pin: repeated drag-outs retain nothing on Core.
TEST_F(TestClipboard, repeatedDragOutsRetainNothing) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(alpha).filePath("a.png")));
    EXPECT_TRUE(waitLoaded(core));
    const QString file = QDir(alpha).filePath("a.png");
    const int before = core.children().size();

    for(int i = 0; i < 8; ++i)
        core.onDraggedOut({file});

    int drags = 0;
    for(QObject *child : core.children())
        if(child->metaObject()->className() == QLatin1String("QDrag"))
            ++drags;
    EXPECT_TRUE(drags == 0) << qPrintable(QStringLiteral("QDrag objects retained by Core after 8 drag-outs"));
    EXPECT_EQ(core.children().size(), before);
}

// The folder view's Ctrl+V: the clipboard holds file paths, the browsing
// directory is the destination. This is a different command from
// openFromClipboard (which SHOWS the clipboard's file), and the whole
// point of two tables declaring Ctrl+V is that each does its own kind of
// paste. The assertions are on the filesystem, not on the tables: what
// matters is that the files arrive and that nothing is copied onto itself.
TEST_F(TestClipboard, pastingFileUrlsPlacesThemInTheBrowsingDirectory) {
    const QString source = uniqueDir("paste-src");
    const QString dest = uniqueDir("paste-dest");
    const QString srcFile = source + QStringLiteral("/copied.png");
    writeImage(srcFile);

    Core core;
    EXPECT_TRUE(core.loadPath(QDir(alpha).filePath("a.png")));
    EXPECT_TRUE(waitLoaded(core));
    // make the destination the browsed directory
    EXPECT_TRUE(core.loadPath(dest)); // loadPath on a directory switches there
    EXPECT_TRUE(waitLoaded(core));

    QMimeData *mime = new QMimeData();
    mime->setUrls({QUrl::fromLocalFile(srcFile)});
    QApplication::clipboard()->setMimeData(mime);

    core.pasteFilesToCurrentDirectory();

    const QString pasted = dest + QStringLiteral("/copied.png");
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return QFileInfo::exists(pasted);
        },
        10000));
    EXPECT_TRUE(QFileInfo::exists(srcFile)) << "paste removed the source — that is a move";
}

// A file already in the destination is skipped BEFORE interactiveCopy
// runs. copyFileTo would answer NOTHING_TO_DO on it, and interactiveCopy
// reports every non-SUCCESS result through showError — so without the
// filter, pressing Ctrl+V on a file that is already here surfaces
// "Nothing to do." as if it were a failure. The filesystem cannot show
// that difference (NOTHING_TO_DO changes nothing), so the assertion is on
// the overlay: a silent no-op, not an error.
TEST_F(TestClipboard, pastingAFileAlreadyInTheDirectoryIsSilent) {
    const QString dest = uniqueDir("paste-self");
    const QString file = dest + QStringLiteral("/here.png");
    writeImage(file);

    coretest::FakeOverlayHost overlayFake;
    Core::Deps deps;
    deps.overlays = &overlayFake;
    Core core(deps);
    EXPECT_TRUE(core.loadPath(file));
    EXPECT_TRUE(waitLoaded(core));

    QMimeData *mime = new QMimeData();
    mime->setUrls({QUrl::fromLocalFile(file)});
    QApplication::clipboard()->setMimeData(mime);

    core.pasteFilesToCurrentDirectory();
    coretest::drainEvents();

    EXPECT_TRUE(overlayFake.errorCalls == 0)
        << qPrintable(QStringLiteral("pasting into the same directory reported: ") + overlayFake.lastError);
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_TRUE(!QFileInfo::exists(dest + QStringLiteral("/here (1).png")))
        << "pasting a file into its own directory created a copy";
}

// A remote url has no local path and is refused the same way
// openFromClipboard refuses it.
TEST_F(TestClipboard, pastingARemoteUrlDoesNothing) {
    const QString dest = uniqueDir("paste-remote");
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(alpha).filePath("a.png")));
    EXPECT_TRUE(waitLoaded(core));
    EXPECT_TRUE(core.loadPath(dest));

    QMimeData *mime = new QMimeData();
    mime->setUrls({QUrl(QStringLiteral("https://example.com/x.png"))});
    QApplication::clipboard()->setMimeData(mime);

    core.pasteFilesToCurrentDirectory();
    coretest::drainEvents();
    EXPECT_TRUE(QDir(dest).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
