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
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("other"));
    QFile other(m_fixtureDir + "/other/z.jpg");
    EXPECT_TRUE(other.open(QIODevice::WriteOnly));

    EngineBridge bridgeA;
    EngineBridge bridgeB;
    QSignalSpy listedA(&bridgeA, &EngineBridge::dirListed);
    QSignalSpy listedB(&bridgeB, &EngineBridge::dirListed);
    EXPECT_TRUE(listedA.isValid());
    EXPECT_TRUE(listedB.isValid());

    EXPECT_TRUE(bridgeA.openDir(m_fixtureDir, 0, false));
    EXPECT_TRUE(bridgeB.openDir(m_fixtureDir + "/other", 0, false));

    ASSERT_TRY_EQ_TIMEOUT(listedA.count(), 1, 10000);
    ASSERT_TRY_EQ_TIMEOUT(listedB.count(), 1, 10000);
    EXPECT_EQ(listedA.at(0).at(0).toString(), m_fixtureDir);
    EXPECT_EQ(listedB.at(0).at(0).toString(), m_fixtureDir + "/other");
    // and the listings do not cross: each instance reports its own engine,
    // including which directory its reads describe
    EXPECT_EQ(bridgeA.listedDirectory(), m_fixtureDir);
    EXPECT_EQ(bridgeB.listedDirectory(), m_fixtureDir + "/other");
    EXPECT_EQ(bridgeA.fileCount(), 3);
    EXPECT_EQ(bridgeB.fileCount(), 1);
}

TEST_F(TestInfra, watcherBurstsKeepTheMirrorCurrent) {
    resetFixture();
    EngineBridge bridge;
    DirectoryManager mirror(bridge);
    EXPECT_TRUE(mirror.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(mirror.isSettled(), 10000);
    const auto before = mirror.fileCount();
    for(int burst = 0; burst < 3; ++burst) {
        for(int file = 0; file < 2; ++file)
            makeFile(QStringLiteral("wake%1-%2.jpg").arg(burst).arg(file));
        ASSERT_TRY_EQ_TIMEOUT(mirror.fileCount(), before + 2 * (burst + 1), 10000);
    }
}

TEST_F(TestInfra, aBridgeCanBeDestroyedWhileItsEngineIsBusy) {
    resetFixture();
    {
        EngineBridge bridge;
        EXPECT_TRUE(bridge.openDir(m_fixtureDir, 0, false));
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
    EXPECT_TRUE(after.openDir(m_fixtureDir, 0, false));
    ASSERT_TRY_EQ_TIMEOUT(listed.count(), 1, 10000);
}

TEST_F(TestInfra, extensionListForJoinsFormatsAndEmptiesToAcceptAll) {
    // the seam where an empty whitelist once meant "accept nothing"
    // against the header's documented NULL contract
    EXPECT_EQ(EngineBridge::extensionListFor({QByteArray("jpg"), QByteArray("png")}), QByteArray("jpg\npng"));
    EXPECT_EQ(EngineBridge::extensionListFor({}), QByteArray());
}
