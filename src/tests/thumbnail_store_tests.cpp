// The shared freedesktop thumbnail store: naming, validity keys, permissions,
// expiry, and what it refuses to touch. Split out of machinery_tests.cpp, which
// had grown to 1365 lines over 47 tests spanning six unrelated subsystems.
//
// These tests own a ThumbnailCache pointed at a QTemporaryDir, never the real
// store: the sweep deletes files, and a test that ran against ~/.cache/thumbnails
// would be trimming the user's library.
#include "gtest_qt.h"
#include "engine_stack_fixture.h"
#include "components/cache/thumbnailcache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QUrl>
#ifdef Q_OS_WIN
#include <qt_windows.h> // CreateFileW: the exclusive lock is the platform's denial channel
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

// The mtime the cache's clocks read, set to whole-second epochs: utimensat on
// POSIX; Qt's setFileTime over a ReadWrite open on Windows (utimensat is
// POSIX-only, and a WriteOnly open would truncate). Returns success -- the
// callers QVERIFY it, so a silent no-op cannot fake an aged file.
static bool setMTimeSecs(const QString &path, qint64 secs) {
#ifdef Q_OS_WIN
    QFile f(path);
    if(!f.open(QIODevice::ReadWrite))
        return false;
    return f.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime);
#else
    const struct timespec times[2] = {{0, UTIME_OMIT}, {secs, 0}};
    return ::utimensat(AT_FDCWD, path.toLocal8Bit().constData(), times, 0) == 0;
#endif
}

class TestThumbnailStore : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestThumbnailStore() : EngineStackFixture("thumbnail-store") {
    }
};
// The store is shared with every other thumbnail-aware program on the
// desktop, so the properties that matter are the spec's, not this app's:
// the file name is md5(URI) and nothing else, the validity test is the
// spec's three keys, an unreadable original is neither read nor written,
// and a purge may only ever delete entries this app wrote.
TEST_F(TestThumbnailStore, thumbnailStoreKeysOnTheUriAlone) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("store"));
    const QString source = m_fixtureDir + "/img1.jpg";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(64, 64)));

    // md5 of the canonical URI, over the URI and nothing else — no size,
    // no crop flag, no app name. This is what makes a thumbnail another
    // program wrote a hit here on first sight.
    const QByteArray digest =
        QCryptographicHash::hash(QUrl::fromLocalFile(source).toString(QUrl::FullyEncoded).toUtf8(),
                                 QCryptographicHash::Md5)
            .toHex();
    EXPECT_EQ(cache.thumbnailPath(source, 128),
              QDir(own.filePath("store"))
                  .filePath(QStringLiteral("normal/") + QString::fromLatin1(digest) + QStringLiteral(".png")));
    // and the bucket is a directory, not part of the name: one source has
    // one identity per bucket and only the box differs
    EXPECT_EQ(QFileInfo(cache.thumbnailPath(source, 256)).fileName(),
              QFileInfo(cache.thumbnailPath(source, 128)).fileName());
    EXPECT_TRUE(cache.thumbnailPath(source, 256).contains("/large/"));

    // percent-encoding is the part a hand-rolled encoder would get wrong:
    // space and non-ASCII are the bytes a live KIO store contains
    EXPECT_EQ(ThumbnailCache::uriFor(QStringLiteral("/tmp/a b/日本語.png")),
              QStringLiteral("file:///tmp/a%20b/%E6%97%A5%E6%9C%AC%E8%AA%9E.png"));
}

TEST_F(TestThumbnailStore, thumbnailStoreValidityFollowsTheSpecKeys) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("validity"));
    const QString source = m_fixtureDir + "/img2.jpg";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(64, 64)));

    QImage picture(128, 128, QImage::Format_ARGB32);
    picture.fill(Qt::black);
    const QSize original(640, 480);
    EXPECT_TRUE(cache.save(picture, source, 128, QFileInfo(source), original, /*animated=*/true, /*video=*/false));

    auto hit = cache.read(source, 128, QFileInfo(source));
    EXPECT_TRUE(hit.has_value()) << "a freshly written thumbnail must be a hit";
    EXPECT_EQ(hit->originalSize, original);
    EXPECT_TRUE(hit->animated);
    EXPECT_TRUE(!hit->video);
    // the spec's mandatory keys are on disk, with whole-second mtime
    QImage storedImage(QFileInfo(cache.thumbnailPath(source, 128)).filePath());
    EXPECT_EQ(storedImage.text(QStringLiteral("Thumb::URI")), ThumbnailCache::uriFor(source));
    EXPECT_EQ(storedImage.text(QStringLiteral("Thumb::MTime")),
              QString::number(QFileInfo(source).lastModified().toSecsSinceEpoch()));
    EXPECT_EQ(storedImage.text(QStringLiteral("Thumb::Size")), QString::number(QFileInfo(source).size()));

    // mtime moved: the spec says the thumbnail "must be recreated", which
    // for a reader means it is a miss, never that the old picture is shown.
    // Set explicitly rather than by writing bytes: the spec's clock is
    // whole seconds, so a rewrite that lands in the same second is, by
    // design, invisible to Thumb::MTime (see ThumbnailCache's header).
    const QDateTime future = QDateTime::currentDateTime().addSecs(120);
    EXPECT_TRUE(setMTimeSecs(source, future.toSecsSinceEpoch()));
    EXPECT_TRUE(!cache.read(source, 128, QFileInfo(source)).has_value())
        << "a source whose mtime moved past Thumb::MTime must be a miss";

    // a thumbnail without Thumb::MTime is not trustworthy at all
    const QString noMTime = cache.thumbnailPath(source, 128);
    QImage stripped(128, 128, QImage::Format_ARGB32);
    stripped.fill(Qt::black);
    stripped.setText(QStringLiteral("Thumb::URI"), ThumbnailCache::uriFor(source));
    EXPECT_TRUE(stripped.save(noMTime, "png"));
    EXPECT_TRUE(!cache.read(source, 128, QFileInfo(source)).has_value());
}

// The spec's second key, and the reason it exists: a copy or a swap can
// leave the mtime untouched. Thumb::Size is what still catches a different
// file behind the same name, so it is not decorative — it is the only check
// that survives a clock that cannot see the rewrite.

// The spec's second key, and the reason it exists: a copy or a swap can
// leave the mtime untouched. Thumb::Size is what still catches a different
// file behind the same name, so it is not decorative — it is the only check
// that survives a clock that cannot see the rewrite.
TEST_F(TestThumbnailStore, thumbnailStoreComparesTheSizeKeyWhenTheClockCannot) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("sizekey"));
    const QString source = m_fixtureDir + "/img10.jpg";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(64, 64)));
    QImage picture(128, 128, QImage::Format_ARGB32);
    picture.fill(Qt::black);
    EXPECT_TRUE(cache.save(picture, source, 128, QFileInfo(source), QSize(64, 64), false, false));
    EXPECT_TRUE(cache.read(source, 128, QFileInfo(source)).has_value());

    // replace the content under an unchanged mtime — the shape a swap or a
    // copied-over file leaves behind
    const qint64 stamp = QFileInfo(source).lastModified().toSecsSinceEpoch();
    EXPECT_TRUE(writeBigImage(source, "png", QSize(1024, 1024)));
    EXPECT_TRUE(setMTimeSecs(source, stamp));
    const QImage storedImage(cache.thumbnailPath(source, 128));
    EXPECT_TRUE(storedImage.text(QStringLiteral("Thumb::Size")) != QString::number(QFileInfo(source).size()))
        << "fixture degenerate: the replacement must differ in size for this test to mean anything";
    EXPECT_TRUE(!cache.read(source, 128, QFileInfo(source)).has_value())
        << "a same-mtime file of a different size must not be served the old thumbnail";
}

// Spec 6.1: an original the user cannot read is neither looked up nor
// written — otherwise the cache keeps serving an image the permission was
// meant to hide, and thumbnailing never resumes when it is restored.

// Spec 6.1: an original the user cannot read is neither looked up nor
// written — otherwise the cache keeps serving an image the permission was
// meant to hide, and thumbnailing never resumes when it is restored.
TEST_F(TestThumbnailStore, anUnreadableSourceIsNeitherReadNorWritten) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("unreadable"));
    const QString source = m_fixtureDir + "/secret.png";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(64, 64)));
    QImage picture(128, 128, QImage::Format_ARGB32);
    picture.fill(Qt::black);
    EXPECT_TRUE(cache.save(picture, source, 128, QFileInfo(source), QSize(64, 64), false, false));
    EXPECT_TRUE(cache.exists(source, 128));

    QFileInfo locked(source);
#ifdef Q_OS_WIN
    // mode bits do not exist here — Qt's ACL-derived isReadable() answers
    // true for anything that exists. The platform's denial channel is
    // sharing: a file held open with no sharing cannot be opened for read
    // by anyone, and the cache's open-probe is what refuses.
    HANDLE held = CreateFileW(reinterpret_cast<const WCHAR *>(source.utf16()), GENERIC_READ | GENERIC_WRITE,
                              0 /* no sharing */, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(held != INVALID_HANDLE_VALUE) << "the lock fixture failed to open the source";
#else
    EXPECT_TRUE(::chmod(source.toLocal8Bit().constData(), 0) == 0);
    EXPECT_TRUE(!locked.isReadable());
#endif
    EXPECT_TRUE(!cache.read(source, 128, locked).has_value())
        << "an unreadable original must not be served from the cache";
    EXPECT_TRUE(!cache.save(picture, source, 128, locked, QSize(64, 64), false, false))
        << "nothing may be written for an unreadable original, the failure marker included";
#ifdef Q_OS_WIN
    EXPECT_TRUE(CloseHandle(held));
#else
    ::chmod(source.toLocal8Bit().constData(), 0644);
#endif
}

// The store is shared, so this app's sweep policy may only reach this app's
// files: Dolphin's thumbnails are Dolphin's to expire, and deleting one is a
// forced regeneration on its side.

// The store is shared, so this app's sweep policy may only reach this app's
// files: Dolphin's thumbnails are Dolphin's to expire, and deleting one is a
// forced regeneration on its side.
TEST_F(TestThumbnailStore, theSweepNeverDeletesAnotherProgramsThumbnails) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("shared"));
    const QString source = m_fixtureDir + "/img10.jpg";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(64, 64)));
    QImage picture(128, 128, QImage::Format_ARGB32);
    picture.fill(Qt::black);
    EXPECT_TRUE(cache.save(picture, source, 128, QFileInfo(source), QSize(64, 64), false, false));

    // a foreign entry in the same bucket, written the way KIO writes: the
    // same keys, a different Software
    const QString foreign = QDir(own.filePath("shared")).filePath("normal/foreign.png");
    EXPECT_TRUE(QDir().mkpath(QFileInfo(foreign).absolutePath()));
    QImage theirs(128, 128, QImage::Format_ARGB32);
    theirs.fill(Qt::red);
    theirs.setText(QStringLiteral("Thumb::URI"), QStringLiteral("file:///tmp/theirs.png"));
    theirs.setText(QStringLiteral("Thumb::MTime"), QStringLiteral("1600000000"));
    theirs.setText(QStringLiteral("Thumb::Size"), QStringLiteral("123"));
    theirs.setText(QStringLiteral("Software"), QStringLiteral("KDE Thumbnail Generator Images"));
    EXPECT_TRUE(theirs.save(foreign, "png"));

    const QDateTime now = QDateTime::currentDateTime();
    auto age = [&now](const QString &path, int days) {
        return setMTimeSecs(path, now.addDays(-days).toSecsSinceEpoch());
    };
    EXPECT_TRUE(age(foreign, 40));
    EXPECT_TRUE(age(cache.thumbnailPath(source, 128), 40));

    auto report = cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes);
    EXPECT_EQ(report.idleDropped, 1); // ours, and only ours
    EXPECT_TRUE(!cache.exists(source, 128));
    EXPECT_TRUE(QFile::exists(foreign)) << "another program's thumbnail is not this app's to expire";
}

// The whole point of adopting the spec, pinned against a real KIO entry
// rather than a hand-written one: a thumbnail Dolphin wrote must be a cache
// hit here on first sight (no decode, no duplicate file), and the sweep must
// read its keys correctly even though KIO stores most of them compressed.
// The fixture is the store's own measured shape — zTXt for the long values,
// tEXt for the short ones — so a chunk-walker that only handles tEXt fails
// here rather than silently in production.

// The whole point of adopting the spec, pinned against a real KIO entry
// rather than a hand-written one: a thumbnail Dolphin wrote must be a cache
// hit here on first sight (no decode, no duplicate file), and the sweep must
// read its keys correctly even though KIO stores most of them compressed.
// The fixture is the store's own measured shape — zTXt for the long values,
// tEXt for the short ones — so a chunk-walker that only handles tEXt fails
// here rather than silently in production.
TEST_F(TestThumbnailStore, aThumbnailAnotherProgramWroteIsACacheHit) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("kio"));
    const QString source = m_fixtureDir + "/img1.jpg";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(800, 600)));
    const QFileInfo info(source);

    // exactly what KIO writes: the spec keys, its Software, and the value
    // lengths that make libpng pick zTXt
    const QString path = cache.thumbnailPath(source, 256);
    EXPECT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QImage theirs(256, 192, QImage::Format_ARGB32);
    theirs.fill(Qt::red);
    theirs.setText(QStringLiteral("Thumb::URI"), ThumbnailCache::uriFor(source));
    theirs.setText(QStringLiteral("Thumb::MTime"), QString::number(info.lastModified().toSecsSinceEpoch()));
    theirs.setText(QStringLiteral("Thumb::Size"), QString::number(info.size()));
    theirs.setText(QStringLiteral("Thumb::Image::Width"), QStringLiteral("800"));
    theirs.setText(QStringLiteral("Thumb::Image::Height"), QStringLiteral("600"));
    theirs.setText(QStringLiteral("Software"), QStringLiteral("KDE Thumbnail Generator Images"));
    EXPECT_TRUE(theirs.save(path, "png"));

    auto hit = cache.read(source, 256, info);
    EXPECT_TRUE(hit.has_value()) << "a spec-conforming thumbnail from another program must be a hit";
    EXPECT_EQ(hit->image.size(), QSize(256, 192));
    EXPECT_EQ(hit->originalSize, QSize(800, 600)); // read back from the spec keys
    EXPECT_TRUE(!hit->animated);

    // The sweep must READ those keys, not merely leave the file alone. An
    // unreadable entry is also "not ours", so "it survived" proves nothing
    // on its own — this is the difference a break-test exposed: with the
    // zTXt path disabled the survival assertion still passed. So make this
    // entry look like OUR orphan (same Keys, our Software, original gone)
    // and require the sweep to delete it: only a correct chunk walk can see
    // the URI, and only a seen URI can be declared orphaned.
    EXPECT_TRUE(QFile::remove(source)) << qPrintable(QStringLiteral("fixture broken: the source must be removable: ") +
                                                     source);
    QImage orphan(256, 192, QImage::Format_ARGB32);
    orphan.fill(Qt::red);
    orphan.setText(QStringLiteral("Thumb::URI"), ThumbnailCache::uriFor(source));
    orphan.setText(QStringLiteral("Thumb::MTime"), QStringLiteral("1"));
    orphan.setText(QStringLiteral("Software"), QStringLiteral("ImPulse"));
    EXPECT_TRUE(orphan.save(path, "png")); // a long URI: Qt stores this one compressed
    {
        QFile raw(path);
        EXPECT_TRUE(raw.open(QIODevice::ReadOnly));
        EXPECT_TRUE(raw.readAll().contains("zTXt"))
            << "fixture degenerate: this entry must exercise the compressed path";
    }
    // the probe's handle closes above, before the sweep: Windows refuses
    // DeleteFile on an open handle (QFile shares read+write but never
    // delete), so an open fixture file turns the sweep's removal into a
    // silent failure — measured as orphanDropped stuck at 0
    EXPECT_EQ(cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes).orphanDropped, 1);
    EXPECT_TRUE(!QFile::exists(path));

    // and the older own-file assertion still holds on a foreign entry:
    // re-write it as someone else's, age it 90 days, require survival
    EXPECT_TRUE(theirs.save(path, "png"));
    const QDateTime now = QDateTime::currentDateTime();
    EXPECT_TRUE(setMTimeSecs(path, now.addDays(-90).toSecsSinceEpoch()));
    EXPECT_EQ(cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes).idleDropped, 0);
    EXPECT_TRUE(QFile::exists(path)) << "a foreign thumbnail must survive the idle sweep no matter its age";
}

// Saved files have to be private: the spec's 700/600 rule exists so that a
// thumbnail of a file only its owner can read cannot be read by anyone else
// through the cache. QSaveFile and mkpath both follow the umask (664/775 on
// a stock system), so the modes are forced.

// Saved files have to be private: the spec's 700/600 rule exists so that a
// thumbnail of a file only its owner can read cannot be read by anyone else
// through the cache. QSaveFile and mkpath both follow the umask (664/775 on
// a stock system), so the modes are forced.
TEST_F(TestThumbnailStore, savedThumbnailsAndTheirDirectoriesArePrivate) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("private"));
    const QString source = m_fixtureDir + "/img1.jpg";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(64, 64)));
    QImage picture(128, 128, QImage::Format_ARGB32);
    picture.fill(Qt::black);
    EXPECT_TRUE(cache.save(picture, source, 128, QFileInfo(source), QSize(64, 64), false, false));

    struct stat st{};
    const QString path = cache.thumbnailPath(source, 128);
    EXPECT_TRUE(::stat(path.toLocal8Bit().constData(), &st) == 0);
    // The 600/700 question is chmod semantics of the filesystem: ACL storage
    // answers flat 0666 and the per-user store is already private, so the
    // probe screens that host out before the raw ::stat assertions run.
    const coretest::Capability bits = coretest::modeBits();
    if(!bits.ok)
        GTEST_SKIP() << qPrintable(bits.reason);
    EXPECT_EQ(int(st.st_mode & 07777), 0600);
    EXPECT_TRUE(::stat(QFileInfo(path).absolutePath().toLocal8Bit().constData(), &st) == 0);
    EXPECT_EQ(int(st.st_mode & 07777), 0700);
}

// A zoom step must not be a new file: the request is answered from a bucket
// that is at least as large, and one store entry serves every smaller
// request. Under the old md5(path+size+crop) key this was one decode and one
// file per zoom step (2372 files against the shared store's 163).

// A zoom step must not be a new file: the request is answered from a bucket
// that is at least as large, and one store entry serves every smaller
// request. Under the old md5(path+size+crop) key this was one decode and one
// file per zoom step (2372 files against the shared store's 163).
TEST_F(TestThumbnailStore, oneStoredBucketServesEverySmallerRequest) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("buckets"));
    const QString source = m_fixtureDir + "/img2.jpg";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(1024, 768)));

    EXPECT_EQ(ThumbnailCache::bucketForRequest(120, false, QSize()), 128);
    EXPECT_EQ(ThumbnailCache::bucketForRequest(200, false, QSize()), 256);
    EXPECT_EQ(ThumbnailCache::bucketForRequest(400, false, QSize()), 512);
    // the spec's floor: a 20 px panel request reads normal/ and scales down
    EXPECT_EQ(ThumbnailCache::bucketForRequest(20, false, QSize()), 128);
    // cropped display needs a long edge longer than the square it covers
    EXPECT_EQ(ThumbnailCache::bucketForRequest(120, true, QSize(1600, 1000)), 256);

    // a 128 request is answered by a stored 256, and never the reverse
    QImage picture(256, 192, QImage::Format_ARGB32);
    picture.fill(Qt::black);
    EXPECT_TRUE(cache.save(picture, source, 256, QFileInfo(source), QSize(1024, 768), false, false));
    const QStringList stored = QDir(own.filePath("buckets/large")).entryList(QDir::Files);
    EXPECT_EQ(stored.size(), 1);
    EXPECT_TRUE(!cache.exists(source, 128));
}

TEST_F(TestThumbnailStore, thumbnailCachePurgesByIdleTimeAndReadRefreshesTheClock) {
    resetFixture();
    // a purge deletes files and the byte assertions count every file in
    // the directory, so the directory belongs to this test alone — a
    // fixture shared with another suite, or with the same suite's previous
    // process, silently changes the census
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("idlecache"));
    QImage dot(4, 4, QImage::Format_ARGB32);
    dot.fill(Qt::black);
    const QDateTime now = QDateTime::currentDateTime();
    auto age = [&now](const QString &path, int days) {
        return setMTimeSecs(path, now.addDays(-days).toSecsSinceEpoch());
    };
    const QString hot = m_fixtureDir + "/img1.jpg";
    const QString stale = m_fixtureDir + "/img2.jpg";
    const QString revived = m_fixtureDir + "/img10.jpg";
    for(const QString &source : {hot, stale, revived})
        EXPECT_TRUE(writeBigImage(source, "png", QSize(64, 64)));
    for(const QString &source : {hot, stale, revived})
        EXPECT_TRUE(cache.save(dot, source, 128, QFileInfo(source), QSize(64, 64), false, false));
    EXPECT_TRUE(age(cache.thumbnailPath(hot, 128), 8));
    EXPECT_TRUE(age(cache.thumbnailPath(stale, 128), 16));
    // a cap nobody can reach: this is the idle pass alone, unchanged
    auto report = cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes);
    EXPECT_EQ(report.idleDropped, 1);
    EXPECT_EQ(report.sizeEvicted, 0);
    EXPECT_TRUE(cache.exists(hot, 128));
    EXPECT_TRUE(!cache.exists(stale, 128));

    // a read hit refreshes mtime: a 20-day-old entry that gets displayed
    // once survives the next sweep -- idle time, not age
    EXPECT_TRUE(age(cache.thumbnailPath(revived, 128), 20));
    auto hit = cache.read(revived, 128, QFileInfo(revived));
    EXPECT_TRUE(hit.has_value());
    EXPECT_EQ(cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes).idleDropped, 0);
    EXPECT_TRUE(cache.exists(revived, 128));
}

// Spec 9: an entry whose original no longer exists is deleted, and that is
// a different fact from "it is old" — a fresh thumbnail of a deleted file is
// an orphan on the very next sweep, while an old one of a live file is not.

// Spec 9: an entry whose original no longer exists is deleted, and that is
// a different fact from "it is old" — a fresh thumbnail of a deleted file is
// an orphan on the very next sweep, while an old one of a live file is not.
TEST_F(TestThumbnailStore, theSweepDropsOurOwnOrphansByExistenceNotByAge) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("orphans"));
    const QString gone = m_fixtureDir + "/gone.png";
    EXPECT_TRUE(writeBigImage(gone, "png", QSize(64, 64)));
    QImage picture(128, 128, QImage::Format_ARGB32);
    picture.fill(Qt::black);
    EXPECT_TRUE(cache.save(picture, gone, 128, QFileInfo(gone), QSize(64, 64), false, false));
    EXPECT_TRUE(cache.exists(gone, 128));

    // the original goes away; the thumbnail is seconds old, so only the
    // existence rule can reach it
    EXPECT_TRUE(QFile::remove(gone));
    auto report = cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes);
    EXPECT_EQ(report.orphanDropped, 1);
    EXPECT_EQ(report.idleDropped, 0);
    EXPECT_TRUE(!cache.exists(gone, 128));

    // and a live original of the same age is not touched by that pass
    const QString alive = m_fixtureDir + "/alive.png";
    EXPECT_TRUE(writeBigImage(alive, "png", QSize(64, 64)));
    EXPECT_TRUE(cache.save(picture, alive, 128, QFileInfo(alive), QSize(64, 64), false, false));
    EXPECT_EQ(cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes).orphanDropped, 0);
    EXPECT_TRUE(cache.exists(alive, 128));
}

// The byte cap is a fuse behind the idle sweep, not a second policy: it may
// only fire on what the sweep left, it must order eviction by the same
// last-used clock the expiry uses, and the boundary has to be exact — a
// cache sitting AT the cap is a cache that is not over it.

// The byte cap is a fuse behind the idle sweep, not a second policy: it may
// only fire on what the sweep left, it must order eviction by the same
// last-used clock the expiry uses, and the boundary has to be exact — a
// cache sitting AT the cap is a cache that is not over it.
TEST_F(TestThumbnailStore, thumbnailCacheByteCapFiresOnlyOnWhatTheIdleSweepLeaves) {
    resetFixture();
    QTemporaryDir ownCap;
    EXPECT_TRUE(ownCap.isValid());
    EXPECT_TRUE(QDir().mkpath(ownCap.filePath("capcache")));
    ThumbnailCache cache(ownCap.filePath("capcache"));
    QImage dot(4, 4, QImage::Format_ARGB32);
    dot.fill(Qt::green);
    const QDateTime now = QDateTime::currentDateTime();
    auto age = [&now](const QString &path, int days) {
        return setMTimeSecs(path, now.addDays(-days).toSecsSinceEpoch());
    };
    auto size = [](const QString &path) {
        return QFileInfo(path).size();
    };
    const QString newestSrc = m_fixtureDir + "/img1.jpg";
    const QString olderSrc = m_fixtureDir + "/img2.jpg";
    const QString older2Src = m_fixtureDir + "/img10.jpg";
    const QString oldestSrc = m_fixtureDir + "/skip.txt";
    for(const QString &source : {newestSrc, olderSrc, older2Src, oldestSrc})
        EXPECT_TRUE(writeBigImage(source, "png", QSize(64, 64)));
    const QString newest = cache.thumbnailPath(newestSrc, 128);
    const QString older = cache.thumbnailPath(olderSrc, 128);
    const QString older2 = cache.thumbnailPath(older2Src, 128);
    const QString oldest = cache.thumbnailPath(oldestSrc, 128);

    // four live entries (all inside the idle window), aged so the eviction
    // order is unambiguous: oldest is the one the LRU gives up first
    for(const QString &source : {newestSrc, olderSrc, older2Src, oldestSrc})
        EXPECT_TRUE(cache.save(dot, source, 128, QFileInfo(source), QSize(64, 64), false, false));
    EXPECT_TRUE(age(newest, 1));
    EXPECT_TRUE(age(older, 2));
    EXPECT_TRUE(age(older2, 3));
    EXPECT_TRUE(age(oldest, 4));
    const qint64 four = size(newest) + size(older) + size(older2) + size(oldest);
    const qint64 twoNewest = size(newest) + size(older);
    EXPECT_TRUE(four > twoNewest) << "fixture degenerate: entries have no size to evict";

    // at the cap: nothing fires, nothing is touched
    auto atCap = cache.purgeOlderThan(15, four);
    EXPECT_EQ(atCap.idleDropped, 0);
    EXPECT_EQ(atCap.sizeEvicted, 0);
    EXPECT_EQ(atCap.bytesAfter, four);

    // under it: evicted oldest-first, and it stops the moment the total is
    // inside the cap — the two newest are the survivors because they are
    // the two most recently used
    auto overCap = cache.purgeOlderThan(15, twoNewest);
    EXPECT_EQ(overCap.idleDropped, 0); // none of these was idle-expired
    EXPECT_EQ(overCap.sizeEvicted, 2);
    EXPECT_EQ(overCap.bytesAfter, twoNewest);
    EXPECT_TRUE(cache.exists(newestSrc, 128));
    EXPECT_TRUE(cache.exists(olderSrc, 128));
    EXPECT_TRUE(!cache.exists(older2Src, 128));
    EXPECT_TRUE(!cache.exists(oldestSrc, 128));

    // the ordering claim, and the reason the sweep runs first: a 30-day-old
    // entry is removed as expired, not as oversized, and its bytes never
    // count toward the cap — so a cache whose overage IS the junk the sweep
    // already removed evicts nothing
    EXPECT_TRUE(cache.save(dot, oldestSrc, 128, QFileInfo(oldestSrc), QSize(64, 64), false, false));
    EXPECT_TRUE(age(oldest, 30));
    const qint64 twoPlusAncient = twoNewest + size(oldest);
    auto mixed = cache.purgeOlderThan(15, twoPlusAncient - 1);
    EXPECT_EQ(mixed.idleDropped, 1); // the ancient one, by expiry
    EXPECT_EQ(mixed.sizeEvicted, 0); // and the fuse never needed to fire
    EXPECT_TRUE(cache.exists(newestSrc, 128));
    EXPECT_TRUE(cache.exists(olderSrc, 128));
    EXPECT_TRUE(!cache.exists(oldestSrc, 128));
}

// The checkpoint contract's other half: a thumbnail job canceled before
// its decode still produces the report object taskEnd dereferences — an
// empty QImage becomes the error thumbnail, never a null Thumbnail, and a
// canceled decode never arrives dressed up as a real decode result.
// The store is shared with file managers, so it may only ever hold the
// aspect-preserved picture. The panel's square mode is a display decision:
// it is applied after the read, which is also why a square request still
// comes back square while what landed on disk is not.
TEST_F(TestThumbnailStore, theStoreNeverHoldsACroppedPictureAndSquareDisplayStillWorks) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("cropstore"));
    // a 4:1 source: a crop to square is unmistakable in the arithmetic
    const QString source = m_fixtureDir + "/wide.png";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(800, 200)));
    LoadPolicy policy = loadPolicy();

    auto square = ThumbnailerRunnable::generate(&cache, source, 128, /*crop=*/true, /*force=*/true, policy);
    EXPECT_TRUE(square && square->image());
    EXPECT_EQ(square->image()->size(), QSize(128, 128));

    // what was stored is NOT the square: the bucket image keeps the 4:1
    // shape, so Dolphin shows the whole picture instead of a slice of it.
    // The bucket itself is larger than the display square because a crop
    // needs a long edge longer than the square it covers — 4:1 means 512.
    const int bucket = ThumbnailCache::bucketForRequest(128, /*crop=*/true, QSize(800, 200));
    EXPECT_EQ(bucket, 512);
    const QString stored = cache.thumbnailPath(source, bucket);
    EXPECT_TRUE(QFile::exists(stored));
    const QImage onDisk(stored);
    EXPECT_EQ(onDisk.size(), QSize(512, 128));
    EXPECT_TRUE(onDisk.width() != onDisk.height()) << "the shared store must never hold a cropped thumbnail";

    // and the same entry answers a non-cropped request without a decode:
    // force is what would bypass the store, so this one must not use it
    auto plain = ThumbnailerRunnable::generate(&cache, source, 128, /*crop=*/false, /*force=*/false, policy);
    EXPECT_TRUE(plain && plain->image());
    EXPECT_EQ(plain->image()->size(), QSize(128, 32));
    // the label still reports the SOURCE geometry, from the spec keys
    EXPECT_EQ(plain->info(), QStringLiteral("800x200"));
}

// A smaller request is served from a larger stored bucket: the one write a
// grid zoom makes is reused by the panel instead of decoded again.

// A smaller request is served from a larger stored bucket: the one write a
// grid zoom makes is reused by the panel instead of decoded again.
TEST_F(TestThumbnailStore, aSmallerRequestIsServedFromTheStoredLargerBucket) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("bucketreuse"));
    const QString source = m_fixtureDir + "/big.png";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(1200, 900)));
    LoadPolicy policy = loadPolicy();

    auto grid = ThumbnailerRunnable::generate(&cache, source, 400, /*crop=*/false, /*force=*/true, policy);
    EXPECT_TRUE(grid && grid->image());
    EXPECT_EQ(grid->image()->size(), QSize(400, 300)); // 512 bucket, aspect kept
    EXPECT_TRUE(cache.exists(source, 512));

    auto panel = ThumbnailerRunnable::generate(&cache, source, 120, /*crop=*/false, /*force=*/false, policy);
    EXPECT_TRUE(panel && panel->image());
    EXPECT_EQ(panel->image()->size(), QSize(120, 90));
    EXPECT_TRUE(!cache.exists(source, 128)) << "the smaller request must be answered from the stored larger bucket";
}

// A cache hit still has to label the row with the SOURCE's dimensions, and
// the store cannot be trusted for them: Thumb::Image::Width/Height are
// optional in the spec and KIO does not write them (measured 0 of 679
// entries in a live store). The probe ran before the lookup and knows the
// answer, so a hit uses it instead of reporting 0x0 — which is what the
// first version of this path did, and 0x0 is a number a viewer cannot tell
// from a real one.

// A cache hit still has to label the row with the SOURCE's dimensions, and
// the store cannot be trusted for them: Thumb::Image::Width/Height are
// optional in the spec and KIO does not write them (measured 0 of 679
// entries in a live store). The probe ran before the lookup and knows the
// answer, so a hit uses it instead of reporting 0x0 — which is what the
// first version of this path did, and 0x0 is a number a viewer cannot tell
// from a real one.
TEST_F(TestThumbnailStore, aCacheHitStillReportsTheSourceSizeWhenTheEntryCarriesNoSizeKeys) {
    resetFixture();
    QTemporaryDir own;
    EXPECT_TRUE(own.isValid());
    ThumbnailCache cache(own.filePath("sizeless"));
    const QString source = m_fixtureDir + "/big.png";
    EXPECT_TRUE(writeBigImage(source, "png", QSize(1200, 900)));
    LoadPolicy policy = loadPolicy();

    EXPECT_TRUE(ThumbnailerRunnable::generate(&cache, source, 128, /*crop=*/false, /*force=*/true, policy));

    // replace the entry with one shaped like KIO's: spec keys and Software,
    // but no Thumb::Image::Width/Height
    const QString path = cache.thumbnailPath(source, 128);
    const QFileInfo info(source);
    QImage theirs(128, 96, QImage::Format_ARGB32);
    theirs.fill(Qt::red);
    theirs.setText(QStringLiteral("Thumb::URI"), ThumbnailCache::uriFor(source));
    theirs.setText(QStringLiteral("Thumb::MTime"), QString::number(info.lastModified().toSecsSinceEpoch()));
    theirs.setText(QStringLiteral("Thumb::Size"), QString::number(info.size()));
    theirs.setText(QStringLiteral("Software"), QStringLiteral("KDE Thumbnail Generator Images"));
    EXPECT_TRUE(theirs.save(path, "png"));
    auto hit = cache.read(source, 128, info);
    EXPECT_TRUE(hit.has_value()) << "fixture degenerate: the entry must be a valid hit";
    EXPECT_TRUE(!hit->originalSize.isValid()) << "fixture degenerate: this entry must carry no size keys";

    auto thumb = ThumbnailerRunnable::generate(&cache, source, 128, /*crop=*/false, /*force=*/false, policy);
    EXPECT_TRUE(thumb && thumb->image());
    EXPECT_EQ(thumb->image()->size(), QSize(128, 96));
    EXPECT_EQ(thumb->info(), QStringLiteral("1200x900"));
}
