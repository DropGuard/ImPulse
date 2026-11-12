// The shared freedesktop thumbnail store: naming, validity keys, permissions,
// expiry, and what it refuses to touch. Split out of machinery_tests.cpp, which
// had grown to 1365 lines over 47 tests spanning six unrelated subsystems.
//
// These tests own a ThumbnailCache pointed at a QTemporaryDir, never the real
// store: the sweep deletes files, and a test that ran against ~/.cache/thumbnails
// would be trimming the user's library.
#include "engine_stack_fixture.h"
#include "components/cache/thumbnailcache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QUrl>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

class TestThumbnailStore : public QObject, protected EngineStackFixture {
    Q_OBJECT
  public:
    TestThumbnailStore() : EngineStackFixture("thumbnail-store") {
    }

  private slots:
    // The store is shared with every other thumbnail-aware program on the
    // desktop, so the properties that matter are the spec's, not this app's:
    // the file name is md5(URI) and nothing else, the validity test is the
    // spec's three keys, an unreadable original is neither read nor written,
    // and a purge may only ever delete entries this app wrote.
    void thumbnailStoreKeysOnTheUriAlone() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("store"));
        const QString source = fixtureDir + "/img1.jpg";
        QVERIFY(writeBigImage(source, "png", QSize(64, 64)));

        // md5 of the canonical URI, over the URI and nothing else — no size,
        // no crop flag, no app name. This is what makes a thumbnail another
        // program wrote a hit here on first sight.
        const QByteArray digest =
            QCryptographicHash::hash(QUrl::fromLocalFile(source).toString(QUrl::FullyEncoded).toUtf8(),
                                     QCryptographicHash::Md5)
                .toHex();
        QCOMPARE(cache.thumbnailPath(source, 128),
                 QDir(own.filePath("store"))
                     .filePath(QStringLiteral("normal/") + QString::fromLatin1(digest) + QStringLiteral(".png")));
        // and the bucket is a directory, not part of the name: one source has
        // one identity per bucket and only the box differs
        QCOMPARE(QFileInfo(cache.thumbnailPath(source, 256)).fileName(),
                 QFileInfo(cache.thumbnailPath(source, 128)).fileName());
        QVERIFY(cache.thumbnailPath(source, 256).contains("/large/"));

        // percent-encoding is the part a hand-rolled encoder would get wrong:
        // space and non-ASCII are the bytes a live KIO store contains
        QCOMPARE(ThumbnailCache::uriFor(QStringLiteral("/tmp/a b/日本語.png")),
                 QStringLiteral("file:///tmp/a%20b/%E6%97%A5%E6%9C%AC%E8%AA%9E.png"));
    }

    void thumbnailStoreValidityFollowsTheSpecKeys() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("validity"));
        const QString source = fixtureDir + "/img2.jpg";
        QVERIFY(writeBigImage(source, "png", QSize(64, 64)));

        QImage picture(128, 128, QImage::Format_ARGB32);
        picture.fill(Qt::black);
        const QSize original(640, 480);
        QVERIFY(cache.save(picture, source, 128, QFileInfo(source), original, /*animated=*/true, /*video=*/false));

        auto hit = cache.read(source, 128, QFileInfo(source));
        QVERIFY2(hit.has_value(), "a freshly written thumbnail must be a hit");
        QCOMPARE(hit->originalSize, original);
        QVERIFY(hit->animated);
        QVERIFY(!hit->video);
        // the spec's mandatory keys are on disk, with whole-second mtime
        QImage storedImage(QFileInfo(cache.thumbnailPath(source, 128)).filePath());
        QCOMPARE(storedImage.text(QStringLiteral("Thumb::URI")), ThumbnailCache::uriFor(source));
        QCOMPARE(storedImage.text(QStringLiteral("Thumb::MTime")),
                 QString::number(QFileInfo(source).lastModified().toSecsSinceEpoch()));
        QCOMPARE(storedImage.text(QStringLiteral("Thumb::Size")), QString::number(QFileInfo(source).size()));

        // mtime moved: the spec says the thumbnail "must be recreated", which
        // for a reader means it is a miss, never that the old picture is shown.
        // Set explicitly rather than by writing bytes: the spec's clock is
        // whole seconds, so a rewrite that lands in the same second is, by
        // design, invisible to Thumb::MTime (see ThumbnailCache's header).
        const QDateTime future = QDateTime::currentDateTime().addSecs(120);
        const struct timespec moved[2] = {{0, UTIME_OMIT}, {future.toSecsSinceEpoch(), 0}};
        QVERIFY(::utimensat(AT_FDCWD, source.toLocal8Bit().constData(), moved, 0) == 0);
        QVERIFY2(!cache.read(source, 128, QFileInfo(source)).has_value(),
                 "a source whose mtime moved past Thumb::MTime must be a miss");

        // a thumbnail without Thumb::MTime is not trustworthy at all
        const QString noMTime = cache.thumbnailPath(source, 128);
        QImage stripped(128, 128, QImage::Format_ARGB32);
        stripped.fill(Qt::black);
        stripped.setText(QStringLiteral("Thumb::URI"), ThumbnailCache::uriFor(source));
        QVERIFY(stripped.save(noMTime, "png"));
        QVERIFY(!cache.read(source, 128, QFileInfo(source)).has_value());
    }

    // The spec's second key, and the reason it exists: a copy or a swap can
    // leave the mtime untouched. Thumb::Size is what still catches a different
    // file behind the same name, so it is not decorative — it is the only check
    // that survives a clock that cannot see the rewrite.

    // The spec's second key, and the reason it exists: a copy or a swap can
    // leave the mtime untouched. Thumb::Size is what still catches a different
    // file behind the same name, so it is not decorative — it is the only check
    // that survives a clock that cannot see the rewrite.
    void thumbnailStoreComparesTheSizeKeyWhenTheClockCannot() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("sizekey"));
        const QString source = fixtureDir + "/img10.jpg";
        QVERIFY(writeBigImage(source, "png", QSize(64, 64)));
        QImage picture(128, 128, QImage::Format_ARGB32);
        picture.fill(Qt::black);
        QVERIFY(cache.save(picture, source, 128, QFileInfo(source), QSize(64, 64), false, false));
        QVERIFY(cache.read(source, 128, QFileInfo(source)).has_value());

        // replace the content under an unchanged mtime — the shape a swap or a
        // copied-over file leaves behind
        const qint64 stamp = QFileInfo(source).lastModified().toSecsSinceEpoch();
        QVERIFY(writeBigImage(source, "png", QSize(1024, 1024)));
        const struct timespec keep[2] = {{0, UTIME_OMIT}, {stamp, 0}};
        QVERIFY(::utimensat(AT_FDCWD, source.toLocal8Bit().constData(), keep, 0) == 0);
        const QImage storedImage(cache.thumbnailPath(source, 128));
        QVERIFY2(storedImage.text(QStringLiteral("Thumb::Size")) != QString::number(QFileInfo(source).size()),
                 "fixture degenerate: the replacement must differ in size for this test to mean anything");
        QVERIFY2(!cache.read(source, 128, QFileInfo(source)).has_value(),
                 "a same-mtime file of a different size must not be served the old thumbnail");
    }

    // Spec 6.1: an original the user cannot read is neither looked up nor
    // written — otherwise the cache keeps serving an image the permission was
    // meant to hide, and thumbnailing never resumes when it is restored.

    // Spec 6.1: an original the user cannot read is neither looked up nor
    // written — otherwise the cache keeps serving an image the permission was
    // meant to hide, and thumbnailing never resumes when it is restored.
    void anUnreadableSourceIsNeitherReadNorWritten() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("unreadable"));
        const QString source = fixtureDir + "/secret.png";
        QVERIFY(writeBigImage(source, "png", QSize(64, 64)));
        QImage picture(128, 128, QImage::Format_ARGB32);
        picture.fill(Qt::black);
        QVERIFY(cache.save(picture, source, 128, QFileInfo(source), QSize(64, 64), false, false));
        QVERIFY(cache.exists(source, 128));

        QVERIFY(::chmod(source.toLocal8Bit().constData(), 0) == 0);
        QFileInfo locked(source);
        QVERIFY(!locked.isReadable());
        QVERIFY2(!cache.read(source, 128, locked).has_value(),
                 "an unreadable original must not be served from the cache");
        QVERIFY2(!cache.save(picture, source, 128, locked, QSize(64, 64), false, false),
                 "nothing may be written for an unreadable original, the failure marker included");
        ::chmod(source.toLocal8Bit().constData(), 0644);
    }

    // The store is shared, so this app's sweep policy may only reach this app's
    // files: Dolphin's thumbnails are Dolphin's to expire, and deleting one is a
    // forced regeneration on its side.

    // The store is shared, so this app's sweep policy may only reach this app's
    // files: Dolphin's thumbnails are Dolphin's to expire, and deleting one is a
    // forced regeneration on its side.
    void theSweepNeverDeletesAnotherProgramsThumbnails() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("shared"));
        const QString source = fixtureDir + "/img10.jpg";
        QVERIFY(writeBigImage(source, "png", QSize(64, 64)));
        QImage picture(128, 128, QImage::Format_ARGB32);
        picture.fill(Qt::black);
        QVERIFY(cache.save(picture, source, 128, QFileInfo(source), QSize(64, 64), false, false));

        // a foreign entry in the same bucket, written the way KIO writes: the
        // same keys, a different Software
        const QString foreign = QDir(own.filePath("shared")).filePath("normal/foreign.png");
        QVERIFY(QDir().mkpath(QFileInfo(foreign).absolutePath()));
        QImage theirs(128, 128, QImage::Format_ARGB32);
        theirs.fill(Qt::red);
        theirs.setText(QStringLiteral("Thumb::URI"), QStringLiteral("file:///tmp/theirs.png"));
        theirs.setText(QStringLiteral("Thumb::MTime"), QStringLiteral("1600000000"));
        theirs.setText(QStringLiteral("Thumb::Size"), QStringLiteral("123"));
        theirs.setText(QStringLiteral("Software"), QStringLiteral("KDE Thumbnail Generator Images"));
        QVERIFY(theirs.save(foreign, "png"));

        const QDateTime now = QDateTime::currentDateTime();
        auto age = [&now](const QString &path, int days) {
            const struct timespec times[2] = {{0, UTIME_OMIT}, {now.addDays(-days).toSecsSinceEpoch(), 0}};
            return ::utimensat(AT_FDCWD, path.toLocal8Bit().constData(), times, 0) == 0;
        };
        QVERIFY(age(foreign, 40));
        QVERIFY(age(cache.thumbnailPath(source, 128), 40));

        auto report = cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes);
        QCOMPARE(report.idleDropped, 1); // ours, and only ours
        QVERIFY(!cache.exists(source, 128));
        QVERIFY2(QFile::exists(foreign), "another program's thumbnail is not this app's to expire");
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
    void aThumbnailAnotherProgramWroteIsACacheHit() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("kio"));
        const QString source = fixtureDir + "/img1.jpg";
        QVERIFY(writeBigImage(source, "png", QSize(800, 600)));
        const QFileInfo info(source);

        // exactly what KIO writes: the spec keys, its Software, and the value
        // lengths that make libpng pick zTXt
        const QString path = cache.thumbnailPath(source, 256);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QImage theirs(256, 192, QImage::Format_ARGB32);
        theirs.fill(Qt::red);
        theirs.setText(QStringLiteral("Thumb::URI"), ThumbnailCache::uriFor(source));
        theirs.setText(QStringLiteral("Thumb::MTime"), QString::number(info.lastModified().toSecsSinceEpoch()));
        theirs.setText(QStringLiteral("Thumb::Size"), QString::number(info.size()));
        theirs.setText(QStringLiteral("Thumb::Image::Width"), QStringLiteral("800"));
        theirs.setText(QStringLiteral("Thumb::Image::Height"), QStringLiteral("600"));
        theirs.setText(QStringLiteral("Software"), QStringLiteral("KDE Thumbnail Generator Images"));
        QVERIFY(theirs.save(path, "png"));

        auto hit = cache.read(source, 256, info);
        QVERIFY2(hit.has_value(), "a spec-conforming thumbnail from another program must be a hit");
        QCOMPARE(hit->image.size(), QSize(256, 192));
        QCOMPARE(hit->originalSize, QSize(800, 600)); // read back from the spec keys
        QVERIFY(!hit->animated);

        // The sweep must READ those keys, not merely leave the file alone. An
        // unreadable entry is also "not ours", so "it survived" proves nothing
        // on its own — this is the difference a break-test exposed: with the
        // zTXt path disabled the survival assertion still passed. So make this
        // entry look like OUR orphan (same Keys, our Software, original gone)
        // and require the sweep to delete it: only a correct chunk walk can see
        // the URI, and only a seen URI can be declared orphaned.
        QFile::remove(source);
        QImage orphan(256, 192, QImage::Format_ARGB32);
        orphan.fill(Qt::red);
        orphan.setText(QStringLiteral("Thumb::URI"), ThumbnailCache::uriFor(source));
        orphan.setText(QStringLiteral("Thumb::MTime"), QStringLiteral("1"));
        orphan.setText(QStringLiteral("Software"), QStringLiteral("ImPulse"));
        QVERIFY(orphan.save(path, "png")); // a long URI: Qt stores this one compressed
        QFile raw(path);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        QVERIFY2(raw.readAll().contains("zTXt"), "fixture degenerate: this entry must exercise the compressed path");
        QCOMPARE(cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes).orphanDropped, 1);
        QVERIFY(!QFile::exists(path));

        // and the older own-file assertion still holds on a foreign entry:
        // re-write it as someone else's, age it 90 days, require survival
        QVERIFY(theirs.save(path, "png"));
        const QDateTime now = QDateTime::currentDateTime();
        const struct timespec times[2] = {{0, UTIME_OMIT}, {now.addDays(-90).toSecsSinceEpoch(), 0}};
        QVERIFY(::utimensat(AT_FDCWD, path.toLocal8Bit().constData(), times, 0) == 0);
        QCOMPARE(cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes).idleDropped, 0);
        QVERIFY2(QFile::exists(path), "a foreign thumbnail must survive the idle sweep no matter its age");
    }

    // Saved files have to be private: the spec's 700/600 rule exists so that a
    // thumbnail of a file only its owner can read cannot be read by anyone else
    // through the cache. QSaveFile and mkpath both follow the umask (664/775 on
    // a stock system), so the modes are forced.

    // Saved files have to be private: the spec's 700/600 rule exists so that a
    // thumbnail of a file only its owner can read cannot be read by anyone else
    // through the cache. QSaveFile and mkpath both follow the umask (664/775 on
    // a stock system), so the modes are forced.
    void savedThumbnailsAndTheirDirectoriesArePrivate() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("private"));
        const QString source = fixtureDir + "/img1.jpg";
        QVERIFY(writeBigImage(source, "png", QSize(64, 64)));
        QImage picture(128, 128, QImage::Format_ARGB32);
        picture.fill(Qt::black);
        QVERIFY(cache.save(picture, source, 128, QFileInfo(source), QSize(64, 64), false, false));

        struct stat st{};
        const QString path = cache.thumbnailPath(source, 128);
        QVERIFY(::stat(path.toLocal8Bit().constData(), &st) == 0);
        QCOMPARE(int(st.st_mode & 07777), 0600);
        QVERIFY(::stat(QFileInfo(path).absolutePath().toLocal8Bit().constData(), &st) == 0);
        QCOMPARE(int(st.st_mode & 07777), 0700);
    }

    // A zoom step must not be a new file: the request is answered from a bucket
    // that is at least as large, and one store entry serves every smaller
    // request. Under the old md5(path+size+crop) key this was one decode and one
    // file per zoom step (2372 files against the shared store's 163).

    // A zoom step must not be a new file: the request is answered from a bucket
    // that is at least as large, and one store entry serves every smaller
    // request. Under the old md5(path+size+crop) key this was one decode and one
    // file per zoom step (2372 files against the shared store's 163).
    void oneStoredBucketServesEverySmallerRequest() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("buckets"));
        const QString source = fixtureDir + "/img2.jpg";
        QVERIFY(writeBigImage(source, "png", QSize(1024, 768)));

        QCOMPARE(ThumbnailCache::bucketForRequest(120, false, QSize()), 128);
        QCOMPARE(ThumbnailCache::bucketForRequest(200, false, QSize()), 256);
        QCOMPARE(ThumbnailCache::bucketForRequest(400, false, QSize()), 512);
        // the spec's floor: a 20 px panel request reads normal/ and scales down
        QCOMPARE(ThumbnailCache::bucketForRequest(20, false, QSize()), 128);
        // cropped display needs a long edge longer than the square it covers
        QCOMPARE(ThumbnailCache::bucketForRequest(120, true, QSize(1600, 1000)), 256);

        // a 128 request is answered by a stored 256, and never the reverse
        QImage picture(256, 192, QImage::Format_ARGB32);
        picture.fill(Qt::black);
        QVERIFY(cache.save(picture, source, 256, QFileInfo(source), QSize(1024, 768), false, false));
        const QStringList stored = QDir(own.filePath("buckets/large")).entryList(QDir::Files);
        QCOMPARE(stored.size(), 1);
        QVERIFY(!cache.exists(source, 128));
    }

    void thumbnailCachePurgesByIdleTimeAndReadRefreshesTheClock() {
        resetFixture();
        // a purge deletes files and the byte assertions count every file in
        // the directory, so the directory belongs to this test alone — a
        // fixture shared with another suite, or with the same suite's previous
        // process, silently changes the census
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("idlecache"));
        QImage dot(4, 4, QImage::Format_ARGB32);
        dot.fill(Qt::black);
        const QDateTime now = QDateTime::currentDateTime();
        auto age = [&now](const QString &path, int days) {
            const struct timespec times[2] = {{0, UTIME_OMIT}, {now.addDays(-days).toSecsSinceEpoch(), 0}};
            return ::utimensat(AT_FDCWD, path.toLocal8Bit().constData(), times, 0) == 0;
        };
        const QString hot = fixtureDir + "/img1.jpg";
        const QString stale = fixtureDir + "/img2.jpg";
        const QString revived = fixtureDir + "/img10.jpg";
        for(const QString &source : {hot, stale, revived})
            QVERIFY(writeBigImage(source, "png", QSize(64, 64)));
        for(const QString &source : {hot, stale, revived})
            QVERIFY(cache.save(dot, source, 128, QFileInfo(source), QSize(64, 64), false, false));
        QVERIFY(age(cache.thumbnailPath(hot, 128), 8));
        QVERIFY(age(cache.thumbnailPath(stale, 128), 16));
        // a cap nobody can reach: this is the idle pass alone, unchanged
        auto report = cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes);
        QCOMPARE(report.idleDropped, 1);
        QCOMPARE(report.sizeEvicted, 0);
        QVERIFY(cache.exists(hot, 128));
        QVERIFY(!cache.exists(stale, 128));

        // a read hit refreshes mtime: a 20-day-old entry that gets displayed
        // once survives the next sweep -- idle time, not age
        QVERIFY(age(cache.thumbnailPath(revived, 128), 20));
        auto hit = cache.read(revived, 128, QFileInfo(revived));
        QVERIFY(hit.has_value());
        QCOMPARE(cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes).idleDropped, 0);
        QVERIFY(cache.exists(revived, 128));
    }

    // Spec 9: an entry whose original no longer exists is deleted, and that is
    // a different fact from "it is old" — a fresh thumbnail of a deleted file is
    // an orphan on the very next sweep, while an old one of a live file is not.

    // Spec 9: an entry whose original no longer exists is deleted, and that is
    // a different fact from "it is old" — a fresh thumbnail of a deleted file is
    // an orphan on the very next sweep, while an old one of a live file is not.
    void theSweepDropsOurOwnOrphansByExistenceNotByAge() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("orphans"));
        const QString gone = fixtureDir + "/gone.png";
        QVERIFY(writeBigImage(gone, "png", QSize(64, 64)));
        QImage picture(128, 128, QImage::Format_ARGB32);
        picture.fill(Qt::black);
        QVERIFY(cache.save(picture, gone, 128, QFileInfo(gone), QSize(64, 64), false, false));
        QVERIFY(cache.exists(gone, 128));

        // the original goes away; the thumbnail is seconds old, so only the
        // existence rule can reach it
        QVERIFY(QFile::remove(gone));
        auto report = cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes);
        QCOMPARE(report.orphanDropped, 1);
        QCOMPARE(report.idleDropped, 0);
        QVERIFY(!cache.exists(gone, 128));

        // and a live original of the same age is not touched by that pass
        const QString alive = fixtureDir + "/alive.png";
        QVERIFY(writeBigImage(alive, "png", QSize(64, 64)));
        QVERIFY(cache.save(picture, alive, 128, QFileInfo(alive), QSize(64, 64), false, false));
        QCOMPARE(cache.purgeOlderThan(15, ThumbnailCache::cacheMaxBytes).orphanDropped, 0);
        QVERIFY(cache.exists(alive, 128));
    }

    // The byte cap is a fuse behind the idle sweep, not a second policy: it may
    // only fire on what the sweep left, it must order eviction by the same
    // last-used clock the expiry uses, and the boundary has to be exact — a
    // cache sitting AT the cap is a cache that is not over it.

    // The byte cap is a fuse behind the idle sweep, not a second policy: it may
    // only fire on what the sweep left, it must order eviction by the same
    // last-used clock the expiry uses, and the boundary has to be exact — a
    // cache sitting AT the cap is a cache that is not over it.
    void thumbnailCacheByteCapFiresOnlyOnWhatTheIdleSweepLeaves() {
        resetFixture();
        QTemporaryDir ownCap;
        QVERIFY(ownCap.isValid());
        QVERIFY(QDir().mkpath(ownCap.filePath("capcache")));
        ThumbnailCache cache(ownCap.filePath("capcache"));
        QImage dot(4, 4, QImage::Format_ARGB32);
        dot.fill(Qt::green);
        const QDateTime now = QDateTime::currentDateTime();
        auto age = [&now](const QString &path, int days) {
            const struct timespec times[2] = {{0, UTIME_OMIT}, {now.addDays(-days).toSecsSinceEpoch(), 0}};
            return ::utimensat(AT_FDCWD, path.toLocal8Bit().constData(), times, 0) == 0;
        };
        auto size = [](const QString &path) {
            return QFileInfo(path).size();
        };
        const QString newestSrc = fixtureDir + "/img1.jpg";
        const QString olderSrc = fixtureDir + "/img2.jpg";
        const QString older2Src = fixtureDir + "/img10.jpg";
        const QString oldestSrc = fixtureDir + "/skip.txt";
        for(const QString &source : {newestSrc, olderSrc, older2Src, oldestSrc})
            QVERIFY(writeBigImage(source, "png", QSize(64, 64)));
        const QString newest = cache.thumbnailPath(newestSrc, 128);
        const QString older = cache.thumbnailPath(olderSrc, 128);
        const QString older2 = cache.thumbnailPath(older2Src, 128);
        const QString oldest = cache.thumbnailPath(oldestSrc, 128);

        // four live entries (all inside the idle window), aged so the eviction
        // order is unambiguous: oldest is the one the LRU gives up first
        for(const QString &source : {newestSrc, olderSrc, older2Src, oldestSrc})
            QVERIFY(cache.save(dot, source, 128, QFileInfo(source), QSize(64, 64), false, false));
        QVERIFY(age(newest, 1));
        QVERIFY(age(older, 2));
        QVERIFY(age(older2, 3));
        QVERIFY(age(oldest, 4));
        const qint64 four = size(newest) + size(older) + size(older2) + size(oldest);
        const qint64 twoNewest = size(newest) + size(older);
        QVERIFY2(four > twoNewest, "fixture degenerate: entries have no size to evict");

        // at the cap: nothing fires, nothing is touched
        auto atCap = cache.purgeOlderThan(15, four);
        QCOMPARE(atCap.idleDropped, 0);
        QCOMPARE(atCap.sizeEvicted, 0);
        QCOMPARE(atCap.bytesAfter, four);

        // under it: evicted oldest-first, and it stops the moment the total is
        // inside the cap — the two newest are the survivors because they are
        // the two most recently used
        auto overCap = cache.purgeOlderThan(15, twoNewest);
        QCOMPARE(overCap.idleDropped, 0); // none of these was idle-expired
        QCOMPARE(overCap.sizeEvicted, 2);
        QCOMPARE(overCap.bytesAfter, twoNewest);
        QVERIFY(cache.exists(newestSrc, 128));
        QVERIFY(cache.exists(olderSrc, 128));
        QVERIFY(!cache.exists(older2Src, 128));
        QVERIFY(!cache.exists(oldestSrc, 128));

        // the ordering claim, and the reason the sweep runs first: a 30-day-old
        // entry is removed as expired, not as oversized, and its bytes never
        // count toward the cap — so a cache whose overage IS the junk the sweep
        // already removed evicts nothing
        QVERIFY(cache.save(dot, oldestSrc, 128, QFileInfo(oldestSrc), QSize(64, 64), false, false));
        QVERIFY(age(oldest, 30));
        const qint64 twoPlusAncient = twoNewest + size(oldest);
        auto mixed = cache.purgeOlderThan(15, twoPlusAncient - 1);
        QCOMPARE(mixed.idleDropped, 1); // the ancient one, by expiry
        QCOMPARE(mixed.sizeEvicted, 0); // and the fuse never needed to fire
        QVERIFY(cache.exists(newestSrc, 128));
        QVERIFY(cache.exists(olderSrc, 128));
        QVERIFY(!cache.exists(oldestSrc, 128));
    }

    // The checkpoint contract's other half: a thumbnail job canceled before
    // its decode still produces the report object taskEnd dereferences — an
    // empty QImage becomes the error thumbnail, never a null Thumbnail, and a
    // canceled decode never arrives dressed up as a real decode result.
    // The store is shared with file managers, so it may only ever hold the
    // aspect-preserved picture. The panel's square mode is a display decision:
    // it is applied after the read, which is also why a square request still
    // comes back square while what landed on disk is not.
    void theStoreNeverHoldsACroppedPictureAndSquareDisplayStillWorks() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("cropstore"));
        // a 4:1 source: a crop to square is unmistakable in the arithmetic
        const QString source = fixtureDir + "/wide.png";
        QVERIFY(writeBigImage(source, "png", QSize(800, 200)));
        LoadPolicy policy = loadPolicy();

        auto square = ThumbnailerRunnable::generate(&cache, source, 128, /*crop=*/true, /*force=*/true, policy);
        QVERIFY(square && square->pixmap());
        QCOMPARE(square->pixmap()->size(), QSize(128, 128));

        // what was stored is NOT the square: the bucket image keeps the 4:1
        // shape, so Dolphin shows the whole picture instead of a slice of it.
        // The bucket itself is larger than the display square because a crop
        // needs a long edge longer than the square it covers — 4:1 means 512.
        const int bucket = ThumbnailCache::bucketForRequest(128, /*crop=*/true, QSize(800, 200));
        QCOMPARE(bucket, 512);
        const QString stored = cache.thumbnailPath(source, bucket);
        QVERIFY(QFile::exists(stored));
        const QImage onDisk(stored);
        QCOMPARE(onDisk.size(), QSize(512, 128));
        QVERIFY2(onDisk.width() != onDisk.height(), "the shared store must never hold a cropped thumbnail");

        // and the same entry answers a non-cropped request without a decode:
        // force is what would bypass the store, so this one must not use it
        auto plain = ThumbnailerRunnable::generate(&cache, source, 128, /*crop=*/false, /*force=*/false, policy);
        QVERIFY(plain && plain->pixmap());
        QCOMPARE(plain->pixmap()->size(), QSize(128, 32));
        // the label still reports the SOURCE geometry, from the spec keys
        QCOMPARE(plain->info(), QStringLiteral("800x200"));
    }

    // A smaller request is served from a larger stored bucket: the one write a
    // grid zoom makes is reused by the panel instead of decoded again.

    // A smaller request is served from a larger stored bucket: the one write a
    // grid zoom makes is reused by the panel instead of decoded again.
    void aSmallerRequestIsServedFromTheStoredLargerBucket() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("bucketreuse"));
        const QString source = fixtureDir + "/big.png";
        QVERIFY(writeBigImage(source, "png", QSize(1200, 900)));
        LoadPolicy policy = loadPolicy();

        auto grid = ThumbnailerRunnable::generate(&cache, source, 400, /*crop=*/false, /*force=*/true, policy);
        QVERIFY(grid && grid->pixmap());
        QCOMPARE(grid->pixmap()->size(), QSize(400, 300)); // 512 bucket, aspect kept
        QVERIFY(cache.exists(source, 512));

        auto panel = ThumbnailerRunnable::generate(&cache, source, 120, /*crop=*/false, /*force=*/false, policy);
        QVERIFY(panel && panel->pixmap());
        QCOMPARE(panel->pixmap()->size(), QSize(120, 90));
        QVERIFY2(!cache.exists(source, 128), "the smaller request must be answered from the stored larger bucket");
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
    void aCacheHitStillReportsTheSourceSizeWhenTheEntryCarriesNoSizeKeys() {
        resetFixture();
        QTemporaryDir own;
        QVERIFY(own.isValid());
        ThumbnailCache cache(own.filePath("sizeless"));
        const QString source = fixtureDir + "/big.png";
        QVERIFY(writeBigImage(source, "png", QSize(1200, 900)));
        LoadPolicy policy = loadPolicy();

        QVERIFY(ThumbnailerRunnable::generate(&cache, source, 128, /*crop=*/false, /*force=*/true, policy));

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
        QVERIFY(theirs.save(path, "png"));
        auto hit = cache.read(source, 128, info);
        QVERIFY2(hit.has_value(), "fixture degenerate: the entry must be a valid hit");
        QVERIFY2(!hit->originalSize.isValid(), "fixture degenerate: this entry must carry no size keys");

        auto thumb = ThumbnailerRunnable::generate(&cache, source, 128, /*crop=*/false, /*force=*/false, policy);
        QVERIFY(thumb && thumb->pixmap());
        QCOMPARE(thumb->pixmap()->size(), QSize(128, 96));
        QCOMPARE(thumb->info(), QStringLiteral("1200x900"));
    }
};

#include "thumbnail_store_tests.moc"

int runThumbnailStoreTests(int argc, char **argv) {
    TestThumbnailStore test;
    return runIfOwned(test, argc, argv);
}
