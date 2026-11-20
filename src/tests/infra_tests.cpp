#include "gtest_qt.h"
#include "engine_stack_fixture.h"

// the seams the test suite itself stands on: bridge instance isolation and the
// extension list the FFI is handed
class TestInfra : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestInfra() : EngineStackFixture("test-infra") {
    }
};

TEST_F(TestInfra, bridgeInstancesAreIsolated) {
    // the old singleton forced every test through one engine, one watch
    // target and one session epoch — the per-slot fixture dirs exist only
    // because of that. Two plain instances now get independent engines:
    resetFixture();
    EXPECT_TRUE(QDir(fixtureDir).mkdir("other"));
    QFile other(fixtureDir + "/other/z.jpg");
    EXPECT_TRUE(other.open(QIODevice::WriteOnly));

    EngineBridge bridgeA;
    EngineBridge bridgeB;
    QSignalSpy listedA(&bridgeA, &EngineBridge::dirListed);
    QSignalSpy listedB(&bridgeB, &EngineBridge::dirListed);
    EXPECT_TRUE(listedA.isValid());
    EXPECT_TRUE(listedB.isValid());

    EXPECT_TRUE(bridgeA.openDir(fixtureDir, 0, false));
    EXPECT_TRUE(bridgeB.openDir(fixtureDir + "/other", 0, false));

    ASSERT_TRY_EQ_TIMEOUT(listedA.count(), 1, 10000);
    ASSERT_TRY_EQ_TIMEOUT(listedB.count(), 1, 10000);
    EXPECT_EQ(listedA.at(0).at(0).toString(), fixtureDir);
    EXPECT_EQ(listedB.at(0).at(0).toString(), fixtureDir + "/other");
    // and the listings do not cross: each instance reports its own engine,
    // including which directory its reads describe
    EXPECT_EQ(bridgeA.listedDirectory(), fixtureDir);
    EXPECT_EQ(bridgeB.listedDirectory(), fixtureDir + "/other");
    EXPECT_EQ(bridgeA.fileCount(), 3);
    EXPECT_EQ(bridgeB.fileCount(), 1);
}

// The wake hop: every notification must turn into a delivery of the events
// queued behind it, and no event may be lost between rounds. The eventfd
// era got this for free (writes coalesce, one drain empties the queue); the
// queued-invoke era relies on drainEvents being idempotent instead, and
// that is a property nobody had pinned. The engine fires one notify per
// event it emits, so watcher churn under one watch is the many-rounds
// shape: each apply fires its own notify, and the drain runs whenever one
// lands.
//
// The exactness half of the old pump-era pin ("exactly one delivery per
// event") died with its driver: a created file can legitimately produce
// more than one mutation (a create and a modify in separate poll batches),
// so per-path once-ness is not a property mutations have. What stays
// pinnable and matters is the loss half — every file's arrival survives
// however many notify rounds the churn produces.
TEST_F(TestInfra, everyMutationIsDeliveredAcrossManyNotifyRounds) {
    resetFixture();
    EngineBridge bridge;
    QSignalSpy listed(&bridge, &EngineBridge::dirListed);
    EXPECT_TRUE(listed.isValid());
    QSignalSpy mutated(&bridge, &EngineBridge::dirMutated);
    EXPECT_TRUE(mutated.isValid());
    EXPECT_TRUE(bridge.openDir(fixtureDir, 0, false));
    // settle the initial listing first: a file created before it lands
    // becomes part of the listing, not a mutation of it
    ASSERT_TRY_EQ_TIMEOUT(listed.count(), 1, 10000);
    mutated.clear();

    const int fileCount = 6;
    for(int i = 0; i < fileCount; ++i)
        makeFile(QStringLiteral("wake%1.jpg").arg(i));

    // every file must arrive, however many rounds the churn takes
    QSet<QString> seen;
    QElapsedTimer deadline;
    deadline.start();
    while(seen.size() < fileCount && deadline.elapsed() < 15000) {
        if(!mutated.wait(100))
            continue;
        for(const auto &args : mutated)
            seen.insert(args.at(1).toString());
        mutated.clear();
    }
    // const so the range-for does not detach (clazy range-loop-detach)
    QStringList built;
    for(int i = 0; i < fileCount; ++i)
        built << fixtureDir + QStringLiteral("/wake%1.jpg").arg(i);
    const QStringList expected = built;
    for(const QString &path : expected)
        EXPECT_TRUE(seen.contains(path)) << qPrintable(QStringLiteral("mutation never delivered: ") + path);
}

// Destruction must stop the engine's threads before this object is gone.
// The handle's destructor is the only free, and qimgv_engine_free joins
// the loop thread (which joins the watcher), so the ordering is
// structural — this pins that a bridge can be torn down while its engine
// is mid-work without crashing, and that the destruction path is the one
// that owns the free.
TEST_F(TestInfra, aBridgeCanBeDestroyedWhileItsEngineIsBusy) {
    resetFixture();
    {
        EngineBridge bridge;
        EXPECT_TRUE(bridge.openDir(fixtureDir, 0, false));
        // leave the watcher mid-churn: files created after the watch is
        // installed arrive as mutations, and nothing drains them before
        // the object goes out of scope
        for(int i = 0; i < 20; ++i)
            makeFile(QStringLiteral("busy%1.jpg").arg(i));
    } // ~EngineBridge -> ~EngineHandle -> qimgv_engine_free (joins)
    // reaching here without a crash or a hang is the assertion; a second
    // bridge must still work, which proves the free did not poison state
    EngineBridge after;
    QSignalSpy listed(&after, &EngineBridge::dirListed);
    EXPECT_TRUE(after.openDir(fixtureDir, 0, false));
    ASSERT_TRY_EQ_TIMEOUT(listed.count(), 1, 10000);
}

TEST_F(TestInfra, extensionListForJoinsFormatsAndEmptiesToAcceptAll) {
    // the seam where an empty whitelist once meant "accept nothing"
    // against the header's documented NULL contract
    EXPECT_EQ(EngineBridge::extensionListFor({QByteArray("jpg"), QByteArray("png")}), QByteArray("jpg\npng"));
    EXPECT_EQ(EngineBridge::extensionListFor({}), QByteArray());
}
