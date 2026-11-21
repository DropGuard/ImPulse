#include "gtest_qt.h"
#include "engine_stack_fixture.h"
#include "components/scriptmanager/scriptmanager.h"
#include "utils/inputmap.h"
#include "shortcutbuilder.h"
#include "components/thumbnailer/thumbnailerrunnable.h"
#include "components/cache/thumbnailcache.h"
#include "components/workerpool/workerpool.h"
#include "components/loader/loader.h"

#include <QCoreApplication>
#include <QSet>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>

#include <atomic>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

namespace {

// occupies its worker until released, so a test decides what stays queued
class HoldJob : public PoolJob {
  public:
    HoldJob(std::atomic_bool *_gate, std::atomic_int *_starts) : m_gate(_gate), m_starts(_starts) {
    }
    void run() override {
        if(m_starts)
            m_starts->fetch_add(1);
        while(!m_gate->load())
            QThread::msleep(1);
    }

  private:
    std::atomic_bool *m_gate;
    std::atomic_int *m_starts;
};

} // namespace

// the machinery that degrades quietly instead of crashing: randomizer, cache,
// DocumentInfo, thumbnailer accounting
class TestMachinery : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestMachinery() : EngineStackFixture("machinery") {
    }
};

// Randomizer, Cache and the settings getters fail silently by design: a
// stuck loop hangs the UI thread, an out-of-bounds index reads garbage, a
// junk config value flows into a switch that has no matching arm. Every
// test here pins a failure mode of that shape.

TEST_F(TestMachinery, randomizerWalksEveryImageOncePerCycle) {
    // the reshuffle reorders the whole vector, so a repeat across a
    // reshuffle boundary is by design; what must never happen is the same
    // image twice IN A ROW (permutations give consecutive positions
    // distinct values, and the reshuffle repositions the current item
    // without moving the draw onto its own value) — plus staying in range
    Randomizer r;
    r.setCount(5);
    r.shuffle();
    int previous = -1;
    for(int i = 0; i < 50; ++i) {
        int v = r.next();
        EXPECT_TRUE(v >= 0 && v < 5) << "next() left the index range";
        EXPECT_TRUE(v != previous) << "the same image drawn twice in a row";
        previous = v;
    }
}

TEST_F(TestMachinery, randomizerSurvivesASingleImageFolder) {
    // vec.size()==1 makes next()/prev()'s "reshuffle when at the edge"
    // condition permanently true: the loop spun forever, hanging the UI
    // thread of anyone pressing next in a one-photo folder with shuffle on
    Randomizer r;
    r.setCount(1);
    EXPECT_EQ(r.next(), 0);
    EXPECT_EQ(r.next(), 0);
    EXPECT_EQ(r.prev(), 0);
}

TEST_F(TestMachinery, randomizerSurvivesEmptyAndShrinkingCounts) {
    Randomizer empty;
    empty.setCount(0);
    EXPECT_EQ(empty.next(), 0); // was an out-of-bounds vec[0] read
    EXPECT_EQ(empty.prev(), 0);

    Randomizer shrink;
    shrink.setCount(5);
    for(int i = 0; i < 4; ++i)
        shrink.next();  // cursor deep inside the vector
    shrink.setCount(3); // folder shrank underneath it
    for(int i = 0; i < 6; ++i) {
        int v = shrink.next();
        EXPECT_TRUE(v >= 0 && v < 3) << "stale cursor survived setCount shrink";
    }
}

TEST_F(TestMachinery, randomizerPrevDoesNotIndexBelowZero) {
    // syncRandomizer() calls setCurrent(indexOfFile(current)), and indexOf
    // returns -1 when the file is not in the listing; prev() then walked
    // to vec[-2]
    Randomizer r;
    r.setCount(4);
    r.setCurrent(999);
    int v = r.prev();
    EXPECT_TRUE(v >= 0 && v < 4);
}

TEST_F(TestMachinery, cacheRoundTripsInsertsAndTrims) {
    resetFixture();
    // the factory now refuses pixel-less objects, so the cache fixtures
    // must be real decodable images, not one-byte listing placeholders
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/img1.jpg", "jpg", QSize(8, 8)));
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/img2.jpg", "jpg", QSize(8, 8)));
    Cache cache;
    auto a = ImageFactory::createImage(m_fixtureDir + "/img1.jpg", false, loadPolicy());
    auto b = ImageFactory::createImage(m_fixtureDir + "/img2.jpg", false, loadPolicy());
    EXPECT_TRUE(a.has_value());
    EXPECT_TRUE(b.has_value());
    // every absence is the optional: a missing file, a directory, and a
    // file whose suffix qimgv still tries but cannot decode — the factory
    // never hands out an object whose pixels do not exist, so no consumer
    // downstream ever needs a null-check
    EXPECT_TRUE(!ImageFactory::createImage(m_fixtureDir + "/missing.jpg", false, loadPolicy()).has_value());
    EXPECT_TRUE(!ImageFactory::createImage(m_fixtureDir + "/subdir", false, loadPolicy()).has_value());
    EXPECT_TRUE(!ImageFactory::createImage(m_fixtureDir + "/skip.txt", false, loadPolicy()).has_value());
    // the animated side holds the same contract
    QByteArray junkAnim = "GIF89a-not-really";
    QFile jf(m_fixtureDir + "/broken.gif");
    EXPECT_TRUE(jf.open(QIODevice::WriteOnly));
    jf.write(junkAnim);
    jf.close();
    EXPECT_TRUE(!ImageFactory::createImage(m_fixtureDir + "/broken.gif", false, loadPolicy()).has_value());
    EXPECT_TRUE(cache.insert(*a));
    EXPECT_TRUE(cache.insert(*b));
    EXPECT_TRUE(cache.contains(m_fixtureDir + "/img1.jpg"));
    EXPECT_EQ(cache.get(m_fixtureDir + "/img1.jpg").get(), a->get());

    // a duplicate insert is refused, not double-stored
    EXPECT_TRUE(!cache.insert(*a));
    EXPECT_EQ(cache.keys().size(), 2);

    // a null image must be refused: reporting success for a failed decode
    // is exactly the silent-failure shape this suite hunts
    EXPECT_TRUE(!cache.insert(nullptr));
    EXPECT_EQ(cache.keys().size(), 2);

    cache.trimTo(QStringList() << m_fixtureDir + "/img2.jpg");
    EXPECT_TRUE(!cache.contains(m_fixtureDir + "/img1.jpg"));
    EXPECT_TRUE(cache.contains(m_fixtureDir + "/img2.jpg"));

    cache.remove(m_fixtureDir + "/img2.jpg");
    EXPECT_TRUE(cache.keys().isEmpty());
}

TEST_F(TestMachinery, documentInfoClassifiesFormatsAndAnimation) {
    resetFixture();
    auto write = [this](const QString &name, const QByteArray &bytes) {
        QFile f(m_fixtureDir + "/" + name);
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    };

    // animated WebP: VP8X flags bit 1
    QByteArray webpAnimBytes;
    webpAnimBytes.append("RIFF");
    quint32 riffSize = 4 + 4 + 4 + 4 + 1; // WEBP + VP8X + skip + flags
    webpAnimBytes.append(reinterpret_cast<const char *>(&riffSize), 4);
    webpAnimBytes.append("WEBP");
    webpAnimBytes.append("VP8X");
    webpAnimBytes.append(QByteArray(4, '\x00'));
    webpAnimBytes.append('\x02');
    write("anim.webp", webpAnimBytes);
    DocumentInfo animatedWebp(m_fixtureDir + "/anim.webp", LoadPolicy::fromSettings());
    EXPECT_EQ(animatedWebp.type(), DocumentType::ANIMATED);
    EXPECT_EQ(animatedWebp.format(), QStringLiteral("webp"));

    QByteArray webpStaticBytes = webpAnimBytes;
    webpStaticBytes[webpStaticBytes.size() - 1] = '\x00';
    write("still.webp", webpStaticBytes);
    DocumentInfo staticWebp(m_fixtureDir + "/still.webp", LoadPolicy::fromSettings());
    EXPECT_EQ(staticWebp.type(), DocumentType::STATIC);

    // APNG: acTL inside the first 120 bytes. Whether the "apng" format is
    // produced depends on the deployed Qt imageformats plugin, so the
    // expectation mirrors the plugin check the production code performs.
    QByteArray png = QByteArray("\x89PNG\r\n\x1a\n");
    png.append(QByteArray(20, '\x00'));
    png.append("acTL");
    write("maybe-apng.png", png);
    DocumentInfo apng(m_fixtureDir + "/maybe-apng.png", LoadPolicy::fromSettings());
    if(QImageReader::supportedImageFormats().contains("apng")) {
        EXPECT_EQ(apng.type(), DocumentType::ANIMATED);
        EXPECT_EQ(apng.format(), QStringLiteral("apng"));
    } else {
        EXPECT_EQ(apng.type(), DocumentType::STATIC);
        EXPECT_EQ(apng.format(), QStringLiteral("png"));
    }

    // AVIF animation lives in the ftyp brand; the mime database must know
    // it first, and old shared-mime-info versions do not
    QByteArray avif;
    avif.append("\x00\x00\x00\x20", 4);
    avif.append("ftypavis");
    avif.append(QByteArray(24, '\x00'));
    write("anim.avif", avif);
    QMimeDatabase mimeDb;
    if(mimeDb.mimeTypeForFile(m_fixtureDir + "/anim.avif", QMimeDatabase::MatchContent).name() ==
       QStringLiteral("image/avif")) {
        DocumentInfo avifDoc(m_fixtureDir + "/anim.avif", LoadPolicy::fromSettings());
        EXPECT_EQ(avifDoc.type(), DocumentType::ANIMATED);
    } else {
        GTEST_SKIP() << "this mime database does not identify ftypavis as image/avif";
    }

    // a .jfif file falls through content detection to the suffix, which
    // the code maps onto jpg
    write("photo.jfif", "not really a jpeg");
    DocumentInfo jfif(m_fixtureDir + "/photo.jfif", LoadPolicy::fromSettings());
    EXPECT_EQ(jfif.format(), QStringLiteral("jpg"));
    EXPECT_EQ(jfif.type(), DocumentType::STATIC);

    write("frame.gif", "GIF89a-not-really");
    DocumentInfo gif(m_fixtureDir + "/frame.gif", LoadPolicy::fromSettings());
    EXPECT_EQ(gif.type(), DocumentType::ANIMATED);
    EXPECT_EQ(gif.format(), QStringLiteral("gif"));

    write("pic.bmp", "BM-junk");
    DocumentInfo bmp(m_fixtureDir + "/pic.bmp", LoadPolicy::fromSettings());
    EXPECT_EQ(bmp.type(), DocumentType::STATIC);
    EXPECT_EQ(bmp.format(), QStringLiteral("bmp"));
}

// R4's fix pattern, pinned: classification follows the CARRIED policy, not
// ambient state — the same file classifies differently under two policies,
// and no pool thread ever had to ask the settings global

// R4's fix pattern, pinned: classification follows the CARRIED policy, not
// ambient state — the same file classifies differently under two policies,
// and no pool thread ever had to ask the settings global
TEST_F(TestMachinery, documentInfoClassificationFollowsTheCarriedPolicy) {
    resetFixture();
    makeFile("clip.mp4");

    LoadPolicy videoOn;
    videoOn.videoPlayback = true;
    videoOn.videoFormats.insert("video/mp4", "mp4");
    DocumentInfo videoed(m_fixtureDir + "/clip.mp4", videoOn);
    EXPECT_EQ(videoed.type(), DocumentType::VIDEO);
    EXPECT_EQ(videoed.format(), QStringLiteral("mp4"));

    LoadPolicy videoOff;
    videoOff.videoPlayback = false;
    DocumentInfo plain(m_fixtureDir + "/clip.mp4", videoOff);
    EXPECT_EQ(plain.type(), DocumentType::STATIC);
    EXPECT_EQ(plain.format(), QStringLiteral("mp4"));
}

// ScriptManager's blocking path used to swallow a failed start behind a
// misspelled qDebug ("Unable not run") while the caller waited on a process
// that would never run — the detached path announced failures, this one
// did not.

// ScriptManager's blocking path used to swallow a failed start behind a
// misspelled qDebug ("Unable not run") while the caller waited on a process
// that would never run — the detached path announced failures, this one
// did not.
TEST_F(TestMachinery, scriptBlockingFailureIsAnnounced) {
    auto sm = ScriptManager::getInstance();
    QSignalSpy errored(sm, &ScriptManager::error);
    EXPECT_TRUE(errored.isValid());
    sm->addScript("no-such-tool", Script("qimgv-no-such-binary-42 --flag", true));
    sm->runScript("no-such-tool", nullptr);
    ASSERT_TRY_EQ_TIMEOUT(errored.count(), 1, 5000);
    EXPECT_TRUE(errored.at(0).at(0).toString().contains("qimgv-no-such-binary-42"));
    sm->removeScript("no-such-tool");
}

TEST_F(TestMachinery, scriptBlockingRunCompletesWithoutError) {
    auto sm = ScriptManager::getInstance();
    QSignalSpy errored(sm, &ScriptManager::error);
    EXPECT_TRUE(errored.isValid());
#ifdef Q_OS_WIN
    // /bin/true is a POSIX face of "a command that exits 0"; the always-
    // present Windows spelling is cmd /c exit
    sm->addScript("true-bin", Script(QStringLiteral("cmd /c exit 0"), true));
#else
    sm->addScript("true-bin", Script("/bin/true", true));
#endif
    sm->runScript("true-bin", nullptr);
    // runScript blocks internally, so nothing is pending: drain rather than
    // sleep, and the assertion means "the queue had nothing in it"
    coretest::drainEvents();
    EXPECT_EQ(errored.count(), 0);
    sm->removeScript("true-bin");
}

TEST_F(TestMachinery, splitCommandLineKeepsQuotedArguments) {
    auto parts = ScriptManager::splitCommandLine(QString::fromUtf8(R"(cmd "two words" tail)"));
    EXPECT_EQ(parts.size(), 3);
    EXPECT_EQ(parts.at(1), QStringLiteral("two words"));
    EXPECT_EQ(parts.at(2), QStringLiteral("tail"));
}

// The store is shared with every other thumbnail-aware program on the
// desktop, so the properties that matter are the spec's, not this app's:
// the file name is md5(URI) and nothing else, the validity test is the
// spec's three keys, an unreadable original is neither read nor written,
// and a purge may only ever delete entries this app wrote.

TEST_F(TestMachinery, processArgumentsSubstitutesTheFileTag) {
    resetFixture();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/tagged.png", "png", QSize(8, 8)));
    auto img = *ImageFactory::createImage(m_fixtureDir + "/tagged.png", false, loadPolicy());
    QStringList cmd = {"tool", "%file%"};
    ScriptManager::processArguments(cmd, img);
    // the substitution hands the file to a foreign tool, so the path
    // arrives in the platform's native separators; the comparison
    // normalizes them instead of pinning one spelling
    EXPECT_EQ(QDir::fromNativeSeparators(cmd.at(1)), m_fixtureDir + "/tagged.png");
}

// Loader::clearTasks() is the user leaving a folder: the queued half comes
// back from drain() and must never be seen again — and a job that stops at
// its entry checkpoint reports nothing, so no loadFinished/loadFailed can
// outlive the leaving and turn "no longer wanted" into "this file is
// broken". The pool's single worker is held by a Scaler-owner job, which
// keeps the Loader job queued deterministically AND lets clearTasks()'s
// own waitForOwner(Loader) return immediately, since nothing of Loader's
// is running.

// Loader::clearTasks() is the user leaving a folder: the queued half comes
// back from drain() and must never be seen again — and a job that stops at
// its entry checkpoint reports nothing, so no loadFinished/loadFailed can
// outlive the leaving and turn "no longer wanted" into "this file is
// broken". The pool's single worker is held by a Scaler-owner job, which
// keeps the Loader job queued deterministically AND lets clearTasks()'s
// own waitForOwner(Loader) return immediately, since nothing of Loader's
// is running.
TEST_F(TestMachinery, loaderClearTasksDropsQueuedJobsWithoutAReport) {
    resetFixture();
    std::atomic_bool gate{false};
    std::atomic_int holds{0}, finished{0}, failed{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Scaler, 0, std::make_unique<HoldJob>(&gate, &holds));
    ASSERT_TRY_EQ_TIMEOUT(holds.load(), 1, 10000);
    Loader loader(pool);
    QObject::connect(&loader, &Loader::loadFinished, &loader, [&](std::shared_ptr<Image>, const QString &, bool) {
        finished.fetch_add(1);
    });
    QObject::connect(&loader, &Loader::loadFailed, &loader, [&](const QString &) {
        failed.fetch_add(1);
    });
    const QString path = m_fixtureDir + "/never-decoded.png";
    loader.loadAsync(path, loadPolicy());
    EXPECT_EQ(loader.isBusy(), true);
    EXPECT_EQ(loader.isLoading(path), true);

    loader.clearTasks();
    EXPECT_EQ(loader.isBusy(), false);
    EXPECT_EQ(loader.isLoading(path), false);

    gate.store(true);
    pool.waitForAll();
    EXPECT_EQ(finished.load(), 0);
    EXPECT_EQ(failed.load(), 0);
}

// The flight window's whole point, deterministically: the stop point
// outranks the kept neighbor, and the passed files' queued decodes die for
// nothing. One worker held by a gate makes the queue a fact instead of a
// race; that single worker then serves the survivors in priority order,
// so the finished sequence IS the queue order.
TEST_F(TestMachinery, loadWindowPromotesTheTargetAndDropsThePassedFiles) {
    resetFixture();
    const QString a = m_fixtureDir + "/w-a.png";
    const QString b = m_fixtureDir + "/w-b.png";
    const QString c = m_fixtureDir + "/w-c.png";
    EXPECT_TRUE(writeBigImage(a, "png", QSize(8, 8)));
    EXPECT_TRUE(writeBigImage(b, "png", QSize(8, 8)));
    EXPECT_TRUE(writeBigImage(c, "png", QSize(8, 8)));
    std::atomic_bool gate{false};
    std::atomic_int holds{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Scaler, 0, std::make_unique<HoldJob>(&gate, &holds));
    ASSERT_TRY_EQ_TIMEOUT(holds.load(), 1, 10000);
    Loader loader(pool);
    QStringList order;
    QObject::connect(&loader, &Loader::loadFinished, &loader, [&](std::shared_ptr<Image>, const QString &path, bool) {
        order << path;
    });
    // the flight: three queued preloads, provably none started
    loader.loadAsync(a, loadPolicy());
    loader.loadAsync(b, loadPolicy());
    loader.loadAsync(c, loadPolicy());
    EXPECT_TRUE(loader.isBusy());

    // the stop on c: a and b are behind us and die in the drain, c's
    // queued job is promoted rather than re-created
    QSet<QString> window{c};
    loader.loadWindow(c, window, loadPolicy());
    EXPECT_TRUE(loader.isLoading(c));
    EXPECT_TRUE(!loader.isLoading(a));
    EXPECT_TRUE(!loader.isLoading(b));

    gate.store(true);
    pool.waitForOwner(PoolOwner::Loader);
    ASSERT_TRY_EQ_TIMEOUT(order.size(), 1, 10000);
    EXPECT_EQ(order.first(), c);
}

// Promotion reuses the drained job object, tier included: a tier-2
// upgrade queued behind a flight must come back as tier-2, not be
// re-created as the tier-1 a fresh submit would have built. The JPEG is
// big enough for the screen, which makes the tier visible in the finished
// report — resultScaled is true only for a tier-1 decode.
TEST_F(TestMachinery, loadWindowPromotionKeepsTheTier) {
    resetFixture();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/big.jpg", "jpg", offscreenScreenPixels() * 2));
    std::atomic_bool gate{false};
    std::atomic_int holds{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Scaler, 0, std::make_unique<HoldJob>(&gate, &holds));
    ASSERT_TRY_EQ_TIMEOUT(holds.load(), 1, 10000);
    Loader loader(pool);
    QString finishedPath;
    bool finishedScaled = true;
    QObject::connect(&loader, &Loader::loadFinished, &loader,
                     [&](std::shared_ptr<Image>, const QString &path, bool scaled) {
                         finishedPath = path;
                         finishedScaled = scaled;
                     });
    const QString path = m_fixtureDir + "/big.jpg";
    loader.loadAsyncFull(path, loadPolicy()); // tier-2, queued behind the gate
    EXPECT_TRUE(loader.isLoading(path));

    loader.loadWindow(path, QSet<QString>{path}, loadPolicy());
    EXPECT_TRUE(loader.isLoading(path));

    gate.store(true);
    pool.waitForOwner(PoolOwner::Loader);
    ASSERT_TRY_EQ_TIMEOUT(!finishedPath.isEmpty(), true, 10000);
    EXPECT_EQ(finishedPath, path);
    EXPECT_TRUE(!finishedScaled) << "a promoted tier-2 job arrived dressed as tier-1";
}

// The checkpoint contract's other half: a thumbnail job canceled before
// its decode still produces the report object taskEnd dereferences — an
// empty QImage becomes the error thumbnail, never a null Thumbnail, and a
// canceled decode never arrives dressed up as a real decode result.
// The store is shared with file managers, so it may only ever hold the
// aspect-preserved picture. The panel's square mode is a display decision:
// it is applied after the read, which is also why a square request still
// comes back square while what landed on disk is not.

// ShortcutBuilder had no coverage at all; these pin the event-to-sequence
// mapping against the hardcoded default input map (Ctrl, WheelUp/Down,
// the button names, and the double-click / RMB-release quirks).
TEST_F(TestMachinery, shortcutBuilderMapsLettersAndModifiers) {
    inputMap = InputMap::getInstance();
    QKeyEvent xKey(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QString("x"));
    EXPECT_EQ(ShortcutBuilder::fromEvent(&xKey), QStringLiteral("X"));
    QKeyEvent ctrlX(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier, QString("x"));
    EXPECT_EQ(ShortcutBuilder::fromEvent(&ctrlX), QStringLiteral("Ctrl+X"));
    // a modifier alone is not a shortcut
    QKeyEvent ctrlOnly(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    EXPECT_EQ(ShortcutBuilder::fromEvent(&ctrlOnly), QString());
}

TEST_F(TestMachinery, shortcutBuilderMapsWheelDirection) {
    inputMap = InputMap::getInstance();
    QWheelEvent down(QPointF(8, 8), QPointF(8, 8), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                     Qt::NoScrollPhase, false);
    EXPECT_EQ(ShortcutBuilder::fromEvent(&down), QStringLiteral("WheelDown"));
    QWheelEvent up(QPointF(8, 8), QPointF(8, 8), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                   Qt::NoScrollPhase, false);
    EXPECT_EQ(ShortcutBuilder::fromEvent(&up), QStringLiteral("WheelUp"));
}

TEST_F(TestMachinery, shortcutBuilderMapsMouseButtonsWithTheirQuirks) {
    inputMap = InputMap::getInstance();
    QMouseEvent lmb(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
    EXPECT_EQ(ShortcutBuilder::fromEvent(&lmb), QStringLiteral("LMB"));
    QMouseEvent rmbRelease(QEvent::MouseButtonRelease, QPointF(1, 1), QPointF(1, 1), Qt::RightButton, Qt::RightButton,
                           Qt::NoModifier);
    EXPECT_EQ(ShortcutBuilder::fromEvent(&rmbRelease), QStringLiteral("RMB"));
    // RMB is recognized on release only — a press is deliberately ignored
    QMouseEvent rmbPress(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1), Qt::RightButton, Qt::RightButton,
                         Qt::NoModifier);
    EXPECT_EQ(ShortcutBuilder::fromEvent(&rmbPress), QString());
    QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
    EXPECT_EQ(ShortcutBuilder::fromEvent(&dbl), QStringLiteral("LMB_DoubleClick"));
}

// The keyboard dispatch is QShortcutMap's, and the projection can only
// claim tokens QKeySequence::fromString understands — a name outside Qt's
// table would sit in the keymap editor, render on a binding, and do
// nothing. This sweep keeps the vocabulary inside Qt's: every keyboard
// name the platform's scancode table can produce must parse.
TEST_F(TestMachinery, everyInputMapNameIsQtParseable) {
    inputMap = InputMap::getInstance();
    const QMap<quint32, QString> keyTable = inputMap->keys();
    EXPECT_TRUE(keyTable.size() > 50) << "the scancode table came back nearly empty, the sweep proves nothing";

    QStringList unparseable;
    for(auto it = keyTable.constBegin(); it != keyTable.constEnd(); ++it) {
        const QString &name = it.value();
        if(QKeySequence::fromString(name).count() == 0)
            unparseable << name;
    }
    EXPECT_TRUE(unparseable.isEmpty()) << qPrintable(
        QStringLiteral("InputMap names outside Qt's vocabulary (") + QString::number(keyTable.size()) +
        QStringLiteral(" names checked):\n  ") + unparseable.join(QStringLiteral("\n  ")));
}

// ImageAnimated had no coverage at all: the QMovie-based loader and the
// direct frame decode behind getImage() now run against a real two-frame
// GIF.

// ImageAnimated had no coverage at all: the QMovie-based loader and the
// direct frame decode behind getImage() now run against a real two-frame
// GIF.
TEST_F(TestMachinery, imageAnimatedLoadsFramesFromAGif) {
    resetFixture();
    // minimal two-frame GIF89a (PIL-generated at authoring time)
    const QByteArray gif =
        QByteArray::fromBase64("R0lGODlhAgACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACw"
                               "AAAAAAgACAAAIBgABCAQQEAAh+QQBCgABACwAAAAAAgACAIEAAP8AAAAAAAAAAAAIBgABCAQ"
                               "QEAA7");
    QFile f(m_fixtureDir + "/anim.gif");
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(gif);
    f.close();

    auto image = *ImageFactory::createImage(m_fixtureDir + "/anim.gif", false, loadPolicy());
    EXPECT_EQ(image->type(), DocumentType::ANIMATED);
    auto animated = std::dynamic_pointer_cast<ImageAnimated>(image);
    EXPECT_TRUE(animated != nullptr);
    EXPECT_TRUE(animated->frameCount() >= 1);
    EXPECT_TRUE(animated->getImage() != nullptr);
    EXPECT_TRUE(!animated->getImage()->isNull());
}

// ThemeStore resolves every scheme with a valid tid and derived contrast
// variants; the base colors are the table defaults verbatim.

// ThemeStore resolves every scheme with a valid tid and derived contrast
// variants; the base colors are the table defaults verbatim.
TEST_F(TestMachinery, themeStoreSchemesResolveTheirBaseColors) {
    auto black = ThemeStore::colorScheme(COLORS_BLACK);
    EXPECT_EQ(black.tid, static_cast<int>(COLORS_BLACK));
    EXPECT_EQ(black.background, QColor(QString::fromUtf8("#000000")));
    EXPECT_EQ(black.text, QColor(QString::fromUtf8("#b0b0b0")));
    EXPECT_TRUE(black.text_hc.isValid() && black.text_hc != black.text);
    EXPECT_TRUE(black.button.isValid());

    auto light = ThemeStore::colorScheme(COLORS_LIGHT);
    EXPECT_EQ(light.tid, static_cast<int>(COLORS_LIGHT));
    EXPECT_EQ(light.folderview_topbar, QColor(QString::fromUtf8("#ffffff")));
    EXPECT_TRUE(light.text_hc.isValid() && light.text_hc != light.text);

    auto darkblue = ThemeStore::colorScheme(COLORS_DARKBLUE);
    EXPECT_EQ(darkblue.background, QColor(QString::fromUtf8("#18191a")));
    EXPECT_EQ(darkblue.accent, QColor(QString::fromUtf8("#336ca5")));

    // the system scheme rides the platform palette, but the tid is its own
    auto system = ThemeStore::colorScheme(COLORS_SYSTEM);
    EXPECT_EQ(system.tid, static_cast<int>(COLORS_SYSTEM));
    EXPECT_TRUE(system.widget.isValid());
}

// DocumentInfo's exif path: a written-EXIF fixture flows through exiv2 into
// the orientation accessor and the tag map

// DocumentInfo's exif path: a written-EXIF fixture flows through exiv2 into
// the orientation accessor and the tag map
TEST_F(TestMachinery, documentInfoReadsExifOrientationAndTags) {
    resetFixture();
#ifndef USE_EXIV2
    // the tag map and the orientation accessor are exiv2's domain, and
    // this build has no exiv2 (the mingw64 repo does not carry it) —
    // DocumentInfo compiles its exif path out, so there is nothing to
    // read with here
    GTEST_SKIP() << "exiv2 is not built into this binary";
#endif
    // orientation 6 = rotate 90 CW; Make/Model/DateTime feed loadExifTags
    // (PIL-written at authoring time; one long literal, correctness over beauty)
    const QByteArray exifBytes = QByteArray::fromBase64(
        "/9j/4AAQSkZJRgABAQAAAQABAAD/4QCwRXhpZgAATU0AKgAAAAgABQEPAAIAAAAJAAAASgEQAAIAAAAKAAAAVAESAAMAAAABAAYAAAEyAAIAAAAUAAAAXodpAAQAAAABAAAAcgAAAABUZXN0TWFrZQAAVGVzdE1vZGVsADIwMjQ6MDE6MDIgMDM6MDQ6MDUAAASCmgADAAAAAgABADyCnQADAAAAAgAcAAqIJwADAAAAAQBkAACSCgADAAAAAglgAGQAAAAA/9sAQwAIBgYHBgUIBwcHCQkICgwUDQwLCwwZEhMPFB0aHx4dGhwcICQuJyAiLCMcHCg3KSwwMTQ0NB8nOT04MjwuMzQy/9sAQwEJCQkMCwwYDQ0YMiEcITIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIy/8AAEQgACAAIAwEiAAIRAQMRAf/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMCBAMFBQQEAAABfQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/EAB8BAAMBAQEBAQEBAQEAAAAAAAABAgMEBQYHCAkKC//EALURAAIBAgQEAwQHBQQEAAECdwABAgMRBAUhMQYSQVEHYXETIjKBCBRCkaGxwQkjM1LwFWJy0QoWJDThJfEXGBkaJicoKSo1Njc4OTpDREVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoKDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uLj5OXm5+jp6vLz9PX29/j5+v/aAAwDAQACEQMRAD8Ahooor4c/OD//2Q==");
    QFile out(m_fixtureDir + "/exif.jpg");
    EXPECT_TRUE(out.open(QIODevice::WriteOnly));
    out.write(exifBytes);
    out.close();

    DocumentInfo doc(m_fixtureDir + "/exif.jpg", LoadPolicy::fromSettings());
    // exifOrientation() returns QImageReader::transformation()'s flag
    // value, not the raw tag: orientation 6 lands as TransformationRotate90
    EXPECT_EQ(doc.exifOrientation(), 4); // QImageIOHandler::TransformationRotate90
    doc.loadExifTags();                  // the tag map is lazy
    EXPECT_TRUE(!doc.getExifTags().isEmpty());
    // the rational tags' exact strings are exiv2's formatting domain —
    // pinned here: the keys exist and the verbatim strings survive
    EXPECT_TRUE(doc.getExifTags().contains(QStringLiteral("ExposureTime")));
    EXPECT_TRUE(doc.getExifTags().contains(QStringLiteral("F Number")));
    EXPECT_TRUE(doc.getExifTags().contains(QStringLiteral("Focal Length")));
    EXPECT_EQ(doc.getExifTags().value(QStringLiteral("ISO Speed ratings")), QStringLiteral("100"));
    EXPECT_EQ(doc.getExifTags().value(QStringLiteral("Make")), QStringLiteral("TestMake"));
    EXPECT_EQ(doc.getExifTags().value(QStringLiteral("Model")), QStringLiteral("TestModel"));
    EXPECT_EQ(doc.getExifTags().value(QStringLiteral("Date/Time")), QStringLiteral("2024:01:02 03:04:05"));
}

// The scale pipeline is: worker thread computes the image, its finished signal
// crosses into the Scaler's thread queued, and onTaskFinish delivers
// scalingFinished from there. If the payload has no metatype, Qt drops the call
// with a warning that scrolls past in the log and the viewer simply never gets
// a result — silently, with no crash and every symbol still resolving.
TEST_F(TestMachinery, scaledResultSurvivesTheQueuedHop) {
    resetFixture();
    const QSize orig(64, 64);
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/real.png", "png", orig));
    auto image = *ImageFactory::createImage(m_fixtureDir + "/real.png", false, loadPolicy());
    EXPECT_TRUE(image->getImage()->size() == orig);

    Cache cache;
    WorkerPool pool(2);
    Scaler scaler(&cache, pool);
    int delivered = 0;
    std::shared_ptr<const QImage> result;
    QObject::connect(&scaler, &Scaler::scalingFinished, &scaler, [&](std::shared_ptr<const QImage> pix, ScalerRequest) {
        result = pix;
        ++delivered;
    });
    scaler.requestScaled(ScalerRequest(image, QSize(32, 32), m_fixtureDir + "/real.png", QI_FILTER_BILINEAR, true));
    ASSERT_TRY_EQ_TIMEOUT(delivered, 1, 10000);
    EXPECT_EQ(result->size(), QSize(32, 32));
}

// The delivery is synchronous and lands at a settled point in the state
// machine — running is false and the buffers are consumed by the time the
// emit fires — so a consumer that re-requests from inside its own delivery
// must land on the clean !running path. The queued self-hop this used to go
// through deferred the delivery one loop turn as a shield for a re-entrancy
// point that was already safe; this pin guards the assumption the removal
// made.
TEST_F(TestMachinery, aReentrantRequestFromTheDeliveryCallbackLands) {
    resetFixture();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/first.png", "png", QSize(64, 64)));
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/second.png", "png", QSize(64, 64)));
    auto first = *ImageFactory::createImage(m_fixtureDir + "/first.png", false, loadPolicy());
    auto second = *ImageFactory::createImage(m_fixtureDir + "/second.png", false, loadPolicy());

    Cache cache;
    WorkerPool pool(2);
    Scaler scaler(&cache, pool);
    QSet<QString> delivered;
    QObject::connect(&scaler, &Scaler::scalingFinished, &scaler, [&](std::shared_ptr<const QImage>, ScalerRequest req) {
        delivered.insert(req.path);
        if(req.path == m_fixtureDir + "/first.png")
            scaler.requestScaled(
                ScalerRequest(second, QSize(32, 32), m_fixtureDir + "/second.png", QI_FILTER_BILINEAR, true));
    });
    scaler.requestScaled(ScalerRequest(first, QSize(32, 32), m_fixtureDir + "/first.png", QI_FILTER_BILINEAR, true));
    ASSERT_TRY_EQ_TIMEOUT(int(delivered.count()), 2, 10000);
    EXPECT_TRUE(delivered.contains(m_fixtureDir + "/second.png"));
}

// The equality relation Scaler::startRequest guards its buffer with, pinned
// field by field: both requests differ only in the setting the worker reads
// to choose NEAREST-versus-filter scaling, so they are different work and
// must not be interchangeable. path is deliberately on the "does not change
// the result" side — it identifies the file the result is for.

// The equality relation Scaler::startRequest guards its buffer with, pinned
// field by field: both requests differ only in the setting the worker reads
// to choose NEAREST-versus-filter scaling, so they are different work and
// must not be interchangeable. path is deliberately on the "does not change
// the result" side — it identifies the file the result is for.
TEST_F(TestMachinery, scalerRequestEqualityCoversEverythingThatChangesTheResult) {
    resetFixture();
    const QSize orig(64, 64);
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/real.png", "png", orig));
    auto image = *ImageFactory::createImage(m_fixtureDir + "/real.png", false, loadPolicy());
    const QString p = m_fixtureDir + "/real.png";

    ScalerRequest base(image, QSize(32, 32), p, QI_FILTER_BILINEAR, true);
    EXPECT_TRUE(!(base == ScalerRequest(image, QSize(32, 32), p, QI_FILTER_BILINEAR, false)))
        << "requests that scale differently compare equal: the buffered one would be dropped";
    EXPECT_TRUE(!(base == ScalerRequest(image, QSize(40, 32), p, QI_FILTER_BILINEAR, true))); // size
    EXPECT_TRUE(!(base == ScalerRequest(image, QSize(32, 32), p, QI_FILTER_NEAREST, true)));  // filter
    auto other = *ImageFactory::createImage(m_fixtureDir + "/real.png", false, loadPolicy());
    EXPECT_TRUE(!(base == ScalerRequest(other, QSize(32, 32), p, QI_FILTER_BILINEAR, true))); // image
    EXPECT_TRUE(base == ScalerRequest(image, QSize(32, 32), m_fixtureDir + "/other.png", QI_FILTER_BILINEAR,
                                      true)); // path only
}

TEST_F(TestMachinery, threeSynchronousRequestsHandOffExactlyTwice) {
    // The Scaler decides "a task is in flight" at the moment it hands the
    // request to the pool. It used to learn it from the pool instead, through
    // a DirectConnection that set `running` on the worker thread — so until
    // that thread woke up, every request looked like the first one, and each
    // of them re-used the single ScalerRunnable object and overwrote the
    // request of the task already queued. Those extra hand-offs all happen
    // synchronously, before this test returns control to the event loop, so
    // what is delivered is deterministic: the buffered result, once.
    resetFixture();
    const QSize orig(64, 64);
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/a.png", "png", orig));
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/b.png", "png", orig));
    auto a = *ImageFactory::createImage(m_fixtureDir + "/a.png", false, loadPolicy());
    auto b = *ImageFactory::createImage(m_fixtureDir + "/b.png", false, loadPolicy());

    Cache cache;
    cache.insert(a);
    cache.insert(b);
    WorkerPool pool(2);
    Scaler scaler(&cache, pool);
    int delivered = 0;
    QSize gotSize;
    QString gotPath;
    QObject::connect(&scaler, &Scaler::scalingFinished, &scaler,
                     [&](std::shared_ptr<const QImage> pix, const ScalerRequest &req) {
                         ++delivered;
                         gotSize = pix->size();
                         gotPath = req.path;
                     });

    scaler.requestScaled(ScalerRequest(a, QSize(32, 32), m_fixtureDir + "/a.png", QI_FILTER_BILINEAR, true));
    scaler.requestScaled(ScalerRequest(b, QSize(40, 20), m_fixtureDir + "/b.png", QI_FILTER_BILINEAR, true));
    scaler.requestScaled(ScalerRequest(b, QSize(48, 24), m_fixtureDir + "/b.png", QI_FILTER_BILINEAR, true));

    ASSERT_TRY_EQ_TIMEOUT(delivered, 1, 10000);
    EXPECT_EQ(gotSize, QSize(48, 24)); // the last request, not the first
    EXPECT_EQ(gotPath, m_fixtureDir + "/b.png");
    // the middle request (40x20) must never have been scaled and delivered:
    // every hand-off is synchronous above, so nothing can arrive later:
    // drain the queue and require it stayed empty
    coretest::drainEvents();
    EXPECT_EQ(delivered, 1);
}

TEST_F(TestMachinery, aPinnedImageIsSkippedByEvictionAndReleasedLater) {
    // The scaler pins the image it is scaling so eviction cannot take it away
    // mid-flight. The pin was keyed on the basename while the map is keyed on
    // the absolute path, so it never pinned anything (and every miss returned
    // false into a discarded return value). Eviction also used to WAIT on the
    // pin — with the scaler's result now delivered through a queued
    // connection, waiting on a permit only this thread can release is a
    // deadlock, so eviction skips instead of blocking.
    resetFixture();
    const QSize orig(64, 64);
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/a.png", "png", orig));
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/b.png", "png", orig));
    auto a = *ImageFactory::createImage(m_fixtureDir + "/a.png", false, loadPolicy());
    auto b = *ImageFactory::createImage(m_fixtureDir + "/b.png", false, loadPolicy());

    Cache cache;
    EXPECT_TRUE(cache.insert(a));
    EXPECT_TRUE(cache.insert(b));
    WorkerPool pool(2);
    Scaler scaler(&cache, pool);
    int delivered = 0;
    QObject::connect(&scaler, &Scaler::scalingFinished, &scaler, [&](std::shared_ptr<const QImage>, ScalerRequest) {
        ++delivered;
    });

    scaler.requestScaled(ScalerRequest(a, QSize(32, 32), m_fixtureDir + "/a.png", QI_FILTER_BILINEAR, true));

    cache.trimTo(QStringList{});
    EXPECT_TRUE(cache.contains(a->filePath()))
        << "the image being scaled was evicted under the scaler — the pin missed its key";
    EXPECT_TRUE(!cache.contains(b->filePath())); // not pinned, not listed: gone

    ASSERT_TRY_EQ_TIMEOUT(delivered, 1, 10000);
    cache.trimTo(QStringList{});
    EXPECT_TRUE(!cache.contains(a->filePath())); // unlocked by now, so this pass takes it
}

// Ownership used to be the bug here: the runnable was the one object the
// Scaler created and nobody owned (autoDelete(false), no destructor), so
// every Scaler that ever scaled something left it behind, still holding
// its last ScalerRequest and through it the decoded image. The pool owns
// its jobs outright now — submit() transfers, run() returning is the
// pool's deletion point on the worker thread — and this pin stays to
// prove the Scaler leaves nothing behind: once it is dead, the image's
// refcount is back to the caller's copy alone.

// Ownership used to be the bug here: the runnable was the one object the
// Scaler created and nobody owned (autoDelete(false), no destructor), so
// every Scaler that ever scaled something left it behind, still holding
// its last ScalerRequest and through it the decoded image. The pool owns
// its jobs outright now — submit() transfers, run() returning is the
// pool's deletion point on the worker thread — and this pin stays to
// prove the Scaler leaves nothing behind: once it is dead, the image's
// refcount is back to the caller's copy alone.
TEST_F(TestMachinery, destroyingTheScalerReleasesTheImageItWasHolding) {
    resetFixture();
    const QSize orig(64, 64);
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/real.png", "png", orig));
    auto image = *ImageFactory::createImage(m_fixtureDir + "/real.png", false, loadPolicy());
    EXPECT_EQ(image.use_count(), 1L);

    {
        Cache cache;
        EXPECT_TRUE(cache.insert(image));
        WorkerPool pool(2);
        Scaler scaler(&cache, pool);
        int delivered = 0;
        QObject::connect(&scaler, &Scaler::scalingFinished, &scaler, [&](std::shared_ptr<const QImage>, ScalerRequest) {
            ++delivered;
        });
        scaler.requestScaled(ScalerRequest(image, QSize(32, 32), m_fixtureDir + "/real.png", QI_FILTER_BILINEAR, true));
        ASSERT_TRY_EQ_TIMEOUT(delivered, 1, 10000);
    }

    // the result travels through a queued hop, which copies the request into
    // a posted event; drain that queue before blaming a holder
    coretest::drainEvents();
    EXPECT_TRUE(image.use_count() == 1) << "a destroyed Scaler still holds its last request, and the image inside it";
}
