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

    // The wake hop: every notification must turn into exactly one delivery of
    // the event behind it. The eventfd era got this for free (writes coalesce,
    // one drain empties the queue); the queued-invoke era relies on drainEvents
    // being idempotent instead, and that is a property nobody had pinned. It
    // matters because the engine fires one notify per thumb DISPATCHED, and the
    // number dispatched depends on the in-flight cap — a fact the window is
    // allowed to change.
    //
    // The cap is 2 and a slot frees only on thumbDone, so this drives the pump
    // through several notify rounds rather than one burst, which is the case
    // where a coalescing bug would show as a stuck queue.
    void everyThumbNotificationDeliversExactlyOnce() {
        resetFixture();
        EngineBridge bridge;
        QSignalSpy next(&bridge, &EngineBridge::thumbNext);
        QVERIFY(next.isValid());
        QVERIFY(bridge.openDir(fixtureDir, 0, false));

        QStringList asked;
        for(int i = 0; i < 6; ++i) {
            const QString name = QStringLiteral("wake%1.jpg").arg(i);
            makeFile(name);
            const QString path = fixtureDir + "/" + name;
            QVERIFY(bridge.thumbRequest(path, 128, false, false));
            asked << path;
        }

        // consume as the real Thumbnailer does: free a slot for each delivery,
        // so the pump keeps dispatching. Every delivery is accounted for.
        QSet<QString> seen;
        QElapsedTimer deadline;
        deadline.start();
        while(seen.size() < asked.size() && deadline.elapsed() < 15000) {
            if(!next.wait(100))
                continue;
            for(const auto &args : next) {
                const QString path = args.at(0).toString();
                QVERIFY2(!seen.contains(path), qPrintable(QStringLiteral("thumbnail announced twice: ") + path));
                seen.insert(path);
                bridge.thumbDone(path, 128, false);
            }
            next.clear();
        }
        // each of the six was announced, once — and the last batch could not
        // have arrived without the previous done() calls, which is the
        // ordering the in-flight cap exists to enforce
        QCOMPARE(seen.size(), asked.size());
        // const so the range-for does not detach (clazy range-loop-detach)
        const QStringList requested = asked;
        for(const QString &path : requested)
            QVERIFY2(seen.contains(path), qPrintable(QStringLiteral("thumbnail never announced: ") + path));
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
            // leave requests in flight: no drain turn is awaited before the
            // object goes out of scope
            for(int i = 0; i < 3; ++i) {
                const QString name = QStringLiteral("busy%1.jpg").arg(i);
                makeFile(name);
                bridge.thumbRequest(fixtureDir + "/" + name, 128, false, false);
            }
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
