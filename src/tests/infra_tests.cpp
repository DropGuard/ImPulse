#include "engine_stack_fixture.h"

// the seams the test suite itself stands on: bridge instance isolation and the
// extension list the FFI is handed
class TestInfra : public QObject, protected EngineStackFixture {
    Q_OBJECT
  public:
    TestInfra() : EngineStackFixture("test-infra") {
    }

  private slots:
    void initTestCase() {
    }

    void bridgeInstancesAreIsolated() {
        // the old singleton forced every test through one engine, one watch
        // target and one session epoch — the per-slot fixture dirs exist only
        // because of that. Two plain instances now get independent engines:
        resetFixture();
        QVERIFY(QDir(fixtureDir).mkdir("other"));
        QFile other(fixtureDir + "/other/z.jpg");
        QVERIFY(other.open(QIODevice::WriteOnly));

        EngineBridge bridgeA;
        EngineBridge bridgeB;
        QSignalSpy listedA(&bridgeA, &EngineBridge::dirListed);
        QSignalSpy listedB(&bridgeB, &EngineBridge::dirListed);
        QVERIFY(listedA.isValid());
        QVERIFY(listedB.isValid());

        QVERIFY(bridgeA.openDir(fixtureDir, 0, false));
        QVERIFY(bridgeB.openDir(fixtureDir + "/other", 0, false));

        QTRY_COMPARE_WITH_TIMEOUT(listedA.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(listedB.count(), 1, 10000);
        QCOMPARE(listedA.at(0).at(0).toString(), fixtureDir);
        QCOMPARE(listedB.at(0).at(0).toString(), fixtureDir + "/other");
        // and the listings do not cross: each instance reports its own engine,
        // including which directory its reads describe
        QCOMPARE(bridgeA.listedDirectory(), fixtureDir);
        QCOMPARE(bridgeB.listedDirectory(), fixtureDir + "/other");
        QCOMPARE(bridgeA.fileCount(), 3);
        QCOMPARE(bridgeB.fileCount(), 1);
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
    void everyMutationIsDeliveredAcrossManyNotifyRounds() {
        resetFixture();
        EngineBridge bridge;
        QSignalSpy listed(&bridge, &EngineBridge::dirListed);
        QVERIFY(listed.isValid());
        QSignalSpy mutated(&bridge, &EngineBridge::dirMutated);
        QVERIFY(mutated.isValid());
        QVERIFY(bridge.openDir(fixtureDir, 0, false));
        // settle the initial listing first: a file created before it lands
        // becomes part of the listing, not a mutation of it
        QTRY_COMPARE_WITH_TIMEOUT(listed.count(), 1, 10000);
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
            QVERIFY2(seen.contains(path), qPrintable(QStringLiteral("mutation never delivered: ") + path));
    }

    // Destruction must stop the engine's threads before this object is gone.
    // The handle's destructor is the only free, and qimgv_engine_free joins
    // the loop thread (which joins the watcher), so the ordering is
    // structural — this pins that a bridge can be torn down while its engine
    // is mid-work without crashing, and that the destruction path is the one
    // that owns the free.
    void aBridgeCanBeDestroyedWhileItsEngineIsBusy() {
        resetFixture();
        {
            EngineBridge bridge;
            QVERIFY(bridge.openDir(fixtureDir, 0, false));
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
        QVERIFY(after.openDir(fixtureDir, 0, false));
        QTRY_COMPARE_WITH_TIMEOUT(listed.count(), 1, 10000);
    }

    void extensionListForJoinsFormatsAndEmptiesToAcceptAll() {
        // the seam where an empty whitelist once meant "accept nothing"
        // against the header's documented NULL contract
        QCOMPARE(EngineBridge::extensionListFor({QByteArray("jpg"), QByteArray("png")}), QByteArray("jpg\npng"));
        QCOMPARE(EngineBridge::extensionListFor({}), QByteArray());
    }
};

#include "infra_tests.moc"

int runInfraTests(int argc, char **argv) {
    TestInfra test;
    return runIfOwned(test, argc, argv);
}
