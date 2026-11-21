#include "gtest_qt.h"
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
    DecodeHoldJob(std::atomic_bool *_gate, std::atomic_int *_starts) : m_gate(_gate), m_starts(_starts) {
    }
    void run() override {
        m_starts->fetch_add(1);
        while(!m_gate->load())
            QThread::msleep(1);
    }

  private:
    std::atomic_bool *m_gate;
    std::atomic_int *m_starts;
};

// the two-tier decode pipeline: DirectoryModel plus Loader
// Slice 4's promises where they live: tier-1 is a screen-sized decode and
// JPEG-only (Qt scales every other format AFTER a full decode, which costs
// more than it saves — measured, and the reason for the gate); tier-2 is
// the full-res upgrade of a tier-1 cached image. The queued upgrade that
// the next flip's window drain kills is deliberately not asserted: an idle
// pool thread can start the decode before the flip lands, so that behavior
// is a race by construction and belongs to the real-machine logs.
class TestDecode : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestDecode() : EngineStackFixture("decode") {
    }
};

TEST_F(TestDecode, jpegTwoTierDecodesScaledThenUpgradesToFull) {
    resetFixture();
    const QSize screen = offscreenScreenPixels();
    const QSize orig = screen * 2;
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/big.jpg", "jpg", orig));

    WorkerPool pool(2);
    DirectoryModel model(pool, EngineBridge::instance());
    QSignalSpy ready(&model, &DirectoryModel::imageReady);
    EXPECT_TRUE(ready.isValid());
    EXPECT_TRUE(model.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(model.listing().fileCount(), 4, 10000);

    model.load(m_fixtureDir + "/big.jpg", true); // tier-1 by default
    ASSERT_TRY_TRUE_TIMEOUT(ready.count() >= 1, 10000);
    EXPECT_EQ(ready.at(0).at(2).toBool(), true);
    EXPECT_EQ(spyImage(ready, 0)->size(), screen);

    model.upgradeToFull(m_fixtureDir + "/big.jpg");
    ASSERT_TRY_TRUE_TIMEOUT(ready.count() >= 2, 10000);
    EXPECT_EQ(ready.at(1).at(2).toBool(), false);
    EXPECT_EQ(spyImage(ready, 1)->size(), orig);
}

TEST_F(TestDecode, pngDecodesFullQualityInOneTier) {
    // regression: the scaled request once applied to every format. Qt
    // scales PNG after a full decode, so the "fast" tier was slower than
    // the thing it replaced (84ms vs 46ms measured). The gate is
    // JPEG-only, and this pins it.
    resetFixture();
    const QSize orig = offscreenScreenPixels() * 2;
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/big.png", "png", orig));

    WorkerPool pool(2);
    DirectoryModel model(pool, EngineBridge::instance());
    QSignalSpy ready(&model, &DirectoryModel::imageReady);
    EXPECT_TRUE(model.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(model.listing().fileCount(), 4, 10000);

    model.load(m_fixtureDir + "/big.png", true);
    ASSERT_TRY_TRUE_TIMEOUT(ready.count() >= 1, 10000);
    waitLoaderIdle(model); // a second tier would have arrived by now
    EXPECT_EQ(ready.count(), 1);
    EXPECT_EQ(ready.at(0).at(2).toBool(), false);
    EXPECT_EQ(spyImage(ready, 0)->size(), orig);
}

TEST_F(TestDecode, syncLoadIsAlwaysFullQuality) {
    // the editing path: asyncHint=false bypasses the tiers entirely
    resetFixture();
    const QSize orig = offscreenScreenPixels() * 2;
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/big.jpg", "jpg", orig));

    WorkerPool pool(2);
    DirectoryModel model(pool, EngineBridge::instance());
    QSignalSpy ready(&model, &DirectoryModel::imageReady);
    EXPECT_TRUE(model.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(model.listing().fileCount(), 4, 10000);

    model.load(m_fixtureDir + "/big.jpg", false);
    // synchronous: the signal has fired by the time load() returned
    EXPECT_EQ(ready.count(), 1);
    EXPECT_EQ(ready.at(0).at(2).toBool(), false);
    EXPECT_EQ(spyImage(ready, 0)->size(), orig);
}

TEST_F(TestDecode, cachedFullImageReflipsWithoutRedecoding) {
    resetFixture();
    const QSize screen = offscreenScreenPixels();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/big.jpg", "jpg", screen * 2));

    WorkerPool pool(2);
    DirectoryModel model(pool, EngineBridge::instance());
    QSignalSpy ready(&model, &DirectoryModel::imageReady);
    EXPECT_TRUE(model.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(model.listing().fileCount(), 4, 10000);

    model.load(m_fixtureDir + "/big.jpg", true);
    ASSERT_TRY_TRUE_TIMEOUT(ready.count() >= 1, 10000);
    model.upgradeToFull(m_fixtureDir + "/big.jpg");
    ASSERT_TRY_TRUE_TIMEOUT(ready.count() >= 2, 10000);
    EXPECT_TRUE(!ready.at(1).at(2).toBool());

    // flipping back to a fully-cached image serves it from the cache and
    // schedules nothing: m_scaledCache forgot the path at the upgrade
    ready.clear();
    model.load(m_fixtureDir + "/big.jpg", true);
    EXPECT_EQ(ready.count(), 1);
    EXPECT_EQ(ready.at(0).at(2).toBool(), false);
    waitLoaderIdle(model);
    EXPECT_EQ(ready.count(), 1);
}

TEST_F(TestDecode, upgradeToFullRefusesWhatWasNeverScaled) {
    resetFixture();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/big.png", "png", offscreenScreenPixels() * 2));

    WorkerPool pool(2);
    DirectoryModel model(pool, EngineBridge::instance());
    QSignalSpy ready(&model, &DirectoryModel::imageReady);
    EXPECT_TRUE(model.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(model.listing().fileCount(), 4, 10000);

    model.load(m_fixtureDir + "/big.png", true);
    ASSERT_TRY_TRUE_TIMEOUT(ready.count() >= 1, 10000);
    ready.clear();

    // a full-res image has left m_scaledCache: the upgrade is a no-op,
    // not a third decode of the same file
    model.upgradeToFull(m_fixtureDir + "/big.png");
    waitLoaderIdle(model);
    EXPECT_EQ(ready.count(), 0);

    // and a path outside the listing goes nowhere
    model.upgradeToFull(m_fixtureDir + "/missing.jpg");
    waitLoaderIdle(model);
    EXPECT_EQ(ready.count(), 0);
}

// The flip window: a rapid flight queues one preload per flip, and the
// stop point used to ride the P0 queue behind every passed file's queued
// decode. The window drain drops the passed files for nothing — queued
// jobs have not started — and promotes the stop target's own queued
// preload over whatever survives. With the only worker gated, the queue
// contents are facts rather than races.
TEST_F(TestDecode, theFlipWindowPromotesTheStopPointAndDropsThePassedFiles) {
    resetFixture();
    // real decodable files (the fixture's img*.jpg are placeholders that
    // decode to nothing), named so natural order is w2 < w3 < w10: a
    // flight forward has a neighbor to keep on both stops
    const QString w2 = m_fixtureDir + "/w2.png";
    const QString w3 = m_fixtureDir + "/w3.png";
    const QString w10 = m_fixtureDir + "/w10.png";
    EXPECT_TRUE(writeBigImage(w2, "png", QSize(8, 8)));
    EXPECT_TRUE(writeBigImage(w3, "png", QSize(8, 8)));
    EXPECT_TRUE(writeBigImage(w10, "png", QSize(8, 8)));
    std::atomic_bool gate{false};
    std::atomic_int holds{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Scaler, 0, std::make_unique<DecodeHoldJob>(&gate, &holds));
    ASSERT_TRY_EQ_TIMEOUT(holds.load(), 1, 10000);

    DirectoryModel model(pool, EngineBridge::instance());
    QSignalSpy ready(&model, &DirectoryModel::imageReady);
    EXPECT_TRUE(ready.isValid());
    EXPECT_TRUE(model.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(model.listing().fileCount(), 6, 10000);

    // the flight: two flips forward, each queueing its direction
    // neighbor's preload — w2, then w3 — and with the worker gated both
    // provably sit in the queue
    model.preload(w2);
    model.preload(w3);
    EXPECT_TRUE(model.loaderBusy());

    // the stop on w3: w2 is behind us and dies in the drain, w3's queued
    // preload is promoted rather than re-created, w10 (the direction
    // neighbor) survives at background priority
    model.loadFocused(w3, true, true);

    gate.store(true);
    pool.waitForOwner(PoolOwner::Loader);
    // one worker serves the survivors in priority order, so the arrival
    // sequence IS the queue order: stop point, kept neighbor, and the
    // passed file never
    ASSERT_TRY_EQ_TIMEOUT(ready.count(), 2, 10000);
    EXPECT_EQ(ready.at(0).at(1).toString(), w3);
    EXPECT_EQ(ready.at(1).at(1).toString(), w10);
    waitLoaderIdle(model);
    EXPECT_EQ(ready.count(), 2);
    EXPECT_TRUE(!model.isLoaded(w2));
}
