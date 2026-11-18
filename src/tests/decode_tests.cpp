#include "engine_stack_fixture.h"

#include <atomic>
#include "components/workerpool/workerpool.h"

// Occupies the pool's only worker so Loader jobs stay deterministically
// queued: the flight window's drop-and-promote is a queue operation, and a
// queue operation cannot be asserted against a pool that drains itself.
// Local rather than shared with machinery_tests: each suite owns its
// fixtures (the same reason the engine_stack units are separate binaries).
class DecodeHoldJob : public PoolJob {
  public:
    DecodeHoldJob(std::atomic_bool *_gate, std::atomic_int *_starts) : gate(_gate), starts(_starts) {
    }
    void run() override {
        starts->fetch_add(1);
        while(!gate->load())
            QThread::msleep(1);
    }

  private:
    std::atomic_bool *gate;
    std::atomic_int *starts;
};

// the two-tier decode pipeline: DirectoryModel plus Loader
// Slice 4's promises where they live: tier-1 is a screen-sized decode and
// JPEG-only (Qt scales every other format AFTER a full decode, which costs
// more than it saves — measured, and the reason for the gate); tier-2 is
// the full-res upgrade of a tier-1 cached image. The queued upgrade that
// the next flip's window drain kills is deliberately not asserted: an idle
// pool thread can start the decode before the flip lands, so that behavior
// is a race by construction and belongs to the real-machine logs.
class TestDecode : public QObject, protected EngineStackFixture {
    Q_OBJECT
  public:
    TestDecode() : EngineStackFixture("decode") {
    }

  private slots:
    void initTestCase() {
    }

    void jpegTwoTierDecodesScaledThenUpgradesToFull() {
        resetFixture();
        const QSize screen = offscreenScreenPixels();
        const QSize orig = screen * 2;
        QVERIFY(writeBigImage(fixtureDir + "/big.jpg", "jpg", orig));

        WorkerPool pool(2);
        DirectoryModel model(pool, EngineBridge::instance());
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(ready.isValid());
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.jpg", true); // tier-1 by default
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
        QCOMPARE(ready.at(0).at(2).toBool(), true);
        QCOMPARE(spyImage(ready, 0)->size(), screen);

        model.upgradeToFull(fixtureDir + "/big.jpg");
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 2, 10000);
        QCOMPARE(ready.at(1).at(2).toBool(), false);
        QCOMPARE(spyImage(ready, 1)->size(), orig);
    }

    void pngDecodesFullQualityInOneTier() {
        // regression: the scaled request once applied to every format. Qt
        // scales PNG after a full decode, so the "fast" tier was slower than
        // the thing it replaced (84ms vs 46ms measured). The gate is
        // JPEG-only, and this pins it.
        resetFixture();
        const QSize orig = offscreenScreenPixels() * 2;
        QVERIFY(writeBigImage(fixtureDir + "/big.png", "png", orig));

        WorkerPool pool(2);
        DirectoryModel model(pool, EngineBridge::instance());
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.png", true);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
        waitLoaderIdle(model); // a second tier would have arrived by now
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.at(0).at(2).toBool(), false);
        QCOMPARE(spyImage(ready, 0)->size(), orig);
    }

    void syncLoadIsAlwaysFullQuality() {
        // the editing path: asyncHint=false bypasses the tiers entirely
        resetFixture();
        const QSize orig = offscreenScreenPixels() * 2;
        QVERIFY(writeBigImage(fixtureDir + "/big.jpg", "jpg", orig));

        WorkerPool pool(2);
        DirectoryModel model(pool, EngineBridge::instance());
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.jpg", false);
        // synchronous: the signal has fired by the time load() returned
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.at(0).at(2).toBool(), false);
        QCOMPARE(spyImage(ready, 0)->size(), orig);
    }

    void cachedFullImageReflipsWithoutRedecoding() {
        resetFixture();
        const QSize screen = offscreenScreenPixels();
        QVERIFY(writeBigImage(fixtureDir + "/big.jpg", "jpg", screen * 2));

        WorkerPool pool(2);
        DirectoryModel model(pool, EngineBridge::instance());
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.jpg", true);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
        model.upgradeToFull(fixtureDir + "/big.jpg");
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 2, 10000);
        QVERIFY(!ready.at(1).at(2).toBool());

        // flipping back to a fully-cached image serves it from the cache and
        // schedules nothing: mScaledCache forgot the path at the upgrade
        ready.clear();
        model.load(fixtureDir + "/big.jpg", true);
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.at(0).at(2).toBool(), false);
        waitLoaderIdle(model);
        QCOMPARE(ready.count(), 1);
    }

    void upgradeToFullRefusesWhatWasNeverScaled() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/big.png", "png", offscreenScreenPixels() * 2));

        WorkerPool pool(2);
        DirectoryModel model(pool, EngineBridge::instance());
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 4, 10000);

        model.load(fixtureDir + "/big.png", true);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
        ready.clear();

        // a full-res image has left mScaledCache: the upgrade is a no-op,
        // not a third decode of the same file
        model.upgradeToFull(fixtureDir + "/big.png");
        waitLoaderIdle(model);
        QCOMPARE(ready.count(), 0);

        // and a path outside the listing goes nowhere
        model.upgradeToFull(fixtureDir + "/missing.jpg");
        waitLoaderIdle(model);
        QCOMPARE(ready.count(), 0);
    }

    // The flip window: a rapid flight queues one preload per flip, and the
    // stop point used to ride the P0 queue behind every passed file's queued
    // decode. The window drain drops the passed files for nothing — queued
    // jobs have not started — and promotes the stop target's own queued
    // preload over whatever survives. With the only worker gated, the queue
    // contents are facts rather than races.
    void theFlipWindowPromotesTheStopPointAndDropsThePassedFiles() {
        resetFixture();
        // real decodable files (the fixture's img*.jpg are placeholders that
        // decode to nothing), named so natural order is w2 < w3 < w10: a
        // flight forward has a neighbor to keep on both stops
        const QString w2 = fixtureDir + "/w2.png";
        const QString w3 = fixtureDir + "/w3.png";
        const QString w10 = fixtureDir + "/w10.png";
        QVERIFY(writeBigImage(w2, "png", QSize(8, 8)));
        QVERIFY(writeBigImage(w3, "png", QSize(8, 8)));
        QVERIFY(writeBigImage(w10, "png", QSize(8, 8)));
        std::atomic_bool gate{false};
        std::atomic_int holds{0};
        WorkerPool pool(1);
        pool.submit(PoolOwner::Scaler, 0, std::make_unique<DecodeHoldJob>(&gate, &holds));
        QTRY_COMPARE_WITH_TIMEOUT(holds.load(), 1, 10000);

        DirectoryModel model(pool, EngineBridge::instance());
        QSignalSpy ready(&model, &DirectoryModel::imageReady);
        QVERIFY(ready.isValid());
        QVERIFY(model.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(model.fileCount(), 6, 10000);

        // the flight: two flips forward, each queueing its direction
        // neighbor's preload — w2, then w3 — and with the worker gated both
        // provably sit in the queue
        model.preload(w2);
        model.preload(w3);
        QVERIFY(model.loaderBusy());

        // the stop on w3: w2 is behind us and dies in the drain, w3's queued
        // preload is promoted rather than re-created, w10 (the direction
        // neighbor) survives at background priority
        model.loadFocused(w3, true, true);

        gate.store(true);
        pool.waitForOwner(PoolOwner::Loader);
        // one worker serves the survivors in priority order, so the arrival
        // sequence IS the queue order: stop point, kept neighbor, and the
        // passed file never
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 10000);
        QCOMPARE(ready.at(0).at(1).toString(), w3);
        QCOMPARE(ready.at(1).at(1).toString(), w10);
        waitLoaderIdle(model);
        QCOMPARE(ready.count(), 2);
        QVERIFY(!model.isLoaded(w2));
    }
};

#include "decode_tests.moc"

int runDecodeTests(int argc, char **argv) {
    TestDecode test;
    return runIfOwned(test, argc, argv);
}
