#include "gtest_qt.h"
#include "engine_stack_fixture.h"

#include "components/editdraftstore.h"
#include "components/workerpool/workerpool.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSignalSpy>

// EditDraftStore is the crash-recovery half of editing: an edit lives in
// memory against a path, and a draft on disk lets the next launch offer it
// back. It arrived with the editing rework and had no test — a persistence
// subsystem whose failure mode is "the user's uncommitted work is gone" and
// "the draft directory keeps someone else's data".
//
// The store is driven through its public surface only: write/remove/flush,
// then recoverable()/read() as the next launch would.
class TestEditDraftStore : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestEditDraftStore() : EngineStackFixture("edit-draft-store") {
    }

    void SetUp() override {
        resetFixture();
        // the store reads its directory from QIMGV_DRAFT_DIR on construction,
        // so the override has to be in place before the store exists
        m_draftDir = m_fixtureDir + "/drafts";
        qputenv("QIMGV_DRAFT_DIR", m_draftDir.toUtf8());
    }

    void TearDown() override {
        qunsetenv("QIMGV_DRAFT_DIR");
    }

    // A small deterministic image: the draft round-trip has to preserve pixels,
    // so "not null" is not enough — the corners differ.
    static QImage sample(int seed) {
        QImage img(4, 4, QImage::Format_RGB32);
        img.fill(QColor((seed * 60) % 256, 128, 64));
        img.setPixel(0, 0, qRgb(255, 255, 255));
        return img;
    }

    QString m_draftDir;
};

// The round trip a crash recovery depends on: what was written comes back with
// its source path and its pixels.
TEST_F(TestEditDraftStore, aWrittenDraftComesBackWithItsPathAndPixels) {
    WorkerPool pool(1);
    EditDraftStore store(pool);
    const QUuid id = QUuid::createUuid();
    const QString source = m_fixtureDir + "/photo.jpg";
    const QImage pixels = sample(1);

    store.write(id, source, std::make_shared<const QImage>(pixels));
    ASSERT_TRUE(store.flush()) << "flush reported a failed write";

    const QList<RecoverableEdit> found = store.recoverable();
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found.at(0).id, id);
    EXPECT_EQ(found.at(0).sourcePath, source);

    const QImage back = store.read(id);
    ASSERT_FALSE(back.isNull());
    EXPECT_EQ(back.size(), pixels.size());
    EXPECT_EQ(back.pixel(0, 0), pixels.pixel(0, 0));
    EXPECT_EQ(back.pixel(3, 3), pixels.pixel(3, 3));
}

// remove() is the commit path: a saved or discarded edit must not be offered
// back on the next launch.
TEST_F(TestEditDraftStore, aRemovedDraftIsGoneFromTheNextLaunch) {
    WorkerPool pool(1);
    EditDraftStore store(pool);
    const QUuid id = QUuid::createUuid();
    store.write(id, m_fixtureDir + "/a.jpg", std::make_shared<const QImage>(sample(2)));
    ASSERT_TRUE(store.flush());
    ASSERT_EQ(store.recoverable().size(), 1);

    store.remove(id);
    ASSERT_TRUE(store.flush());
    EXPECT_TRUE(store.recoverable().isEmpty()) << "a removed draft came back";
    EXPECT_TRUE(store.read(id).isNull());
}

// A second write for the same id replaces the first rather than queueing a
// second file: the id is the draft's identity, and two files for it would make
// the next launch's choice arbitrary.
TEST_F(TestEditDraftStore, rewritingTheSameIdReplacesTheDraft) {
    WorkerPool pool(1);
    EditDraftStore store(pool);
    const QUuid id = QUuid::createUuid();
    store.write(id, m_fixtureDir + "/b.jpg", std::make_shared<const QImage>(sample(3)));
    ASSERT_TRUE(store.flush());
    store.write(id, m_fixtureDir + "/b.jpg", std::make_shared<const QImage>(sample(4)));
    ASSERT_TRUE(store.flush());

    EXPECT_EQ(store.recoverable().size(), 1);
    const QImage back = store.read(id);
    ASSERT_FALSE(back.isNull());
    EXPECT_EQ(back.pixel(0, 0), sample(4).pixel(0, 0)) << "the older pixels survived the rewrite";
}

// The store is keyed by id, and ids are per-edit rather than per-path: two
// edits of one file (an earlier abandoned one and the current) are two drafts,
// and recovery hands both back for the caller to judge.
TEST_F(TestEditDraftStore, distinctIdsOnOnePathStayDistinctDrafts) {
    WorkerPool pool(1);
    EditDraftStore store(pool);
    const QString path = m_fixtureDir + "/c.jpg";
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();
    store.write(first, path, std::make_shared<const QImage>(sample(5)));
    store.write(second, path, std::make_shared<const QImage>(sample(6)));
    ASSERT_TRUE(store.flush());

    EXPECT_EQ(store.recoverable().size(), 2);
}

// An unwritable draft directory is a write failure, and the store says so
// rather than dropping the edit silently — the caller decides what to tell the
// user.
TEST_F(TestEditDraftStore, anUnwritableDirectoryReportsAFailure) {
    // a regular file where the directory should be: mkpath cannot make one
    QFile blocker(m_draftDir);
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.write("not a directory");
    blocker.close();

    WorkerPool pool(1);
    EditDraftStore store(pool);
    QSignalSpy failed(&store, &EditDraftStore::writeFailed);
    store.write(QUuid::createUuid(), m_fixtureDir + "/d.jpg", std::make_shared<const QImage>(sample(7)));

    EXPECT_FALSE(store.flush()) << "flush reported success against an unwritable directory";
    ASSERT_TRY_EQ_TIMEOUT(failed.count(), 1, 10000);
}

// exportNew is the save-as path: it refuses to clobber, refuses a non-PNG
// suffix, and strips the draft metadata key so the exported file does not
// claim to be a recovery draft.
TEST_F(TestEditDraftStore, exportNewRefusesToClobberAndStripsTheDraftKey) {
    const QString dest = m_fixtureDir + "/exported.png";
    // the pixels arrive as a draft carries them: the standard Description
    // chunk holds the marker this store reads back
    QImage draft = sample(8);
    draft.setText(QStringLiteral("Description"), QStringLiteral("impulse-draft:/some/photo.jpg"));
    EXPECT_TRUE(EditDraftStore::exportNew(draft, dest));
    EXPECT_TRUE(QFile::exists(dest));

    // The store rides PNG's standard Description chunk (a custom key is
    // dropped by the writer — see editdraftstore.cpp). An exported file must
    // not carry the draft marker, so a later launch does not offer it back.
    const QImage back(dest, "png");
    ASSERT_FALSE(back.isNull());
    EXPECT_FALSE(back.text(QStringLiteral("Description")).startsWith(QStringLiteral("impulse-draft:")))
        << "the exported file carries the draft marker";

    // an existing destination is refused rather than replaced
    EXPECT_FALSE(EditDraftStore::exportNew(sample(9), dest));
    // and so is a suffix that is not png
    EXPECT_FALSE(EditDraftStore::exportNew(sample(8), m_fixtureDir + "/exported.jpg"));
}
