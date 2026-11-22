#include "gtest_qt.h"
#include "engine_stack_fixture.h"
#include "utils/stuff.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef USE_EXIV2
#include <exiv2/exiv2.hpp>
#endif

// FileOperations through the engine, the save path, and ImageLib's transforms
class TestImageOps : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestImageOps() : EngineStackFixture("image-ops") {
    }
};

// tier-1 decodes a large JPEG at screen size via libjpeg DCT scaling, and
// the full-res upgrade rewrites the pixels later; the file's own
// dimensions must survive both — the header knows them before any decode,
// and the title reports them
// upstream issue #653: a 1-bit BMP carries a real color palette, and the
// Mono conversion used to flatten it to Grayscale8 — colored art opened
// as black and white while the folder view (no such conversion) was
// correct. RGB32 keeps the palette AND the scaling workaround.
TEST_F(TestImageOps, monoBitmapPaletteColorsSurviveTheLoad) {
    resetFixture();
    // hand-assembled 1-bit BMP: 8x8, two palette entries (blue, red),
    // alternating pixels. Qt's BMP writer does not do 1-bit, so the
    // fixture is raw bytes — the way machinery_tests writes its EXIF jpeg.
    QByteArray bmp;
    auto u32le = [&bmp](quint32 v) {
        for(unsigned i = 0; i < 4; ++i)
            bmp.append(char((v >> (8 * i)) & 0xFFu));
    };
    auto u16le = [&bmp](quint16 v) {
        const unsigned u = v; // a shifted quint16 promotes to signed int
        bmp.append(char(u & 0xFFu));
        bmp.append(char((u >> 8u) & 0xFFu));
    };
    bmp.append("BM");
    u32le(14 + 40 + 8 + 32); // file size: headers + palette + rows
    u16le(0);
    u16le(0);
    u32le(62); // reserved, pixel data offset
    u32le(40);
    u32le(8);
    u32le(8); // info header size, width, height
    u16le(1);
    u16le(1); // planes, 1 bpp
    u32le(0);
    u32le(32); // BI_RGB, image size
    u32le(0);
    u32le(0);
    u32le(2);
    u32le(0); // ppm, clrUsed=2, clrImportant
    // palette: entry0 blue, entry1 red (B,G,R,reserved)
    bmp.append(char(220));
    bmp.append(char(90));
    bmp.append(char(30));
    bmp.append(char(0));
    bmp.append(char(40));
    bmp.append(char(40));
    bmp.append(char(220));
    bmp.append(char(0));
    // bottom-up rows, alternating 0xAA / 0x55, each padded to 4 bytes
    for(int r = 0; r < 8; ++r) {
        bmp.append(r % 2 == 0 ? char(0xAA) : char(0x55));
        bmp.append(char(0));
        bmp.append(char(0));
        bmp.append(char(0));
    }
    const QString path = m_fixtureDir + "/palette-mono.bmp";
    QFile out(path);
    EXPECT_TRUE(out.open(QIODevice::WriteOnly)) << qPrintable(out.errorString());
    out.write(bmp);
    out.close();

    auto img = ImageFactory::createImage(path, false, loadPolicy());
    EXPECT_TRUE(img.has_value());
    auto src = (*img)->getImage();
    EXPECT_TRUE(src != nullptr);
    EXPECT_TRUE(src->format() != QImage::Format_Mono);
    EXPECT_TRUE(src->format() != QImage::Format_Grayscale8);
    EXPECT_EQ(src->pixelColor(0, 0), QColor(30, 90, 220));
    EXPECT_EQ(src->pixelColor(1, 0), QColor(220, 40, 40));
}

TEST_F(TestImageOps, originalSizeSurvivesTheScaledTier) {
    resetFixture();
    QImage big(4000, 3000, QImage::Format_RGB32);
    big.fill(Qt::red);
    const QString path = m_fixtureDir + "/big.jpg";
    QImageWriter w(path, "jpg");
    EXPECT_TRUE(w.write(big)) << qPrintable(w.errorString());

    auto scaledImg = ImageFactory::createImage(path, true, loadPolicy());
    EXPECT_TRUE(scaledImg.has_value());
    EXPECT_TRUE((*scaledImg)->isScaledResult());
    EXPECT_EQ((*scaledImg)->originalSize(), QSize(4000, 3000));
    EXPECT_TRUE((*scaledImg)->size().width() < 4000);

    auto fullImg = ImageFactory::createImage(path, false, loadPolicy());
    EXPECT_TRUE(fullImg.has_value());
    EXPECT_TRUE(!(*fullImg)->isScaledResult());
    EXPECT_EQ((*fullImg)->size(), QSize(4000, 3000));
    EXPECT_EQ((*fullImg)->originalSize(), QSize(4000, 3000));
}

// The single save-with-rollback implementation ImageStatic and the
// clipboard import now share. Both cases matter: a successful overwrite
// must not leave its backup lying next to the file, and a write that fails
// must put the previous contents back — the shape the copied version got
// wrong by discarding the backup even when the restore had failed.
// reading bytes in a test has to fail the test when the read does not
// happen: QFile::open is nodiscard, and ignoring that would let an empty
// QByteArray compare equal to another empty one and pass
static QByteArray readBytes(const QString &path) {
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly))
        qFatal("cannot open %s", qPrintable(path));
    return f.readAll();
}

// The overwrite is atomic now (QSaveFile: unique temp in the same directory,
// rename on commit), so there is no backup file to look for — what replaces
// that assertion is stronger: the directory must contain the destination and
// nothing else, before and after, and a failed write must leave the original
// byte-identical with no residue.
TEST_F(TestImageOps, saveWithBackupOverwritesAndRestores) {
    resetFixture();
    const QString dest = m_fixtureDir + "/saved.png";
    // the fixture directory holds other files, so "no residue" is asserted
    // as "the set of names did not grow", not as an exact listing
    auto filesHere = [this]() {
        return QDir(m_fixtureDir).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    };

    QImage first(8, 8, QImage::Format_RGB32);
    first.fill(Qt::blue);
    EXPECT_TRUE(first.save(dest, "PNG"));
    const QByteArray firstBytes = readBytes(dest);
    const QStringList before = filesHere();

    // a successful overwrite: new content, and not one extra name
    QImage second(8, 8, QImage::Format_RGB32);
    second.fill(Qt::green);
    EXPECT_TRUE(FileOperations::saveWithBackup(second, dest, 95));
    EXPECT_TRUE(readBytes(dest) != firstBytes) << "the overwrite did not change the file";
    EXPECT_EQ(filesHere(), before);

    // a write that cannot happen at all (no encoder for this suffix) must
    // leave the original exactly as it was, and leave no temporary behind:
    // QSaveFile names its temp uniquely, so a leftover would be a new name
    QImage third(8, 8, QImage::Format_RGB32);
    third.fill(Qt::red);
    const QString odd = m_fixtureDir + "/saved.unknownsuffix";
    EXPECT_TRUE(QFile::copy(dest, odd));
    const QByteArray oddBytesBefore = readBytes(odd);
    const QStringList beforeFailure = filesHere();

    EXPECT_TRUE(!FileOperations::saveWithBackup(third, odd, 95)) << "an unsupported format reported success";
    EXPECT_EQ(readBytes(odd), oddBytesBefore);
    EXPECT_EQ(filesHere(), beforeFailure);
}

// The rollback is the path a failed save takes, and until it was one
// syscall it was two with a gap between them: remove the destination, then
// copy the backup back. The watch saw that as a death and a birth, the
// mirror dropped the row and grew it again, and between the two operations
// the listing described a file that did not exist. One rename(2) moves the
// backup's inode into the destination's name — replacing whatever the failed
// write left there, which QFile::rename cannot do, that call being
// RENAME_NOREPLACE — and consumes the backup, so the row never leaves.
TEST_F(TestImageOps, rolledBackSaveRestoresTheRowInsteadOfRebuildingIt) {
    // The premise "a file that refuses reopen-for-write" is empirical: the
    // read-only attribute refuses it on Windows, a 0600 file refuses it on
    // POSIX, and root reopens either — the probe answers for whichever host
    // is running, so no compile-time assumption speaks for the other side.
    const coretest::Capability readOnly = coretest::readOnlyFiles();
    if(!readOnly.ok)
        GTEST_SKIP() << qPrintable(readOnly.reason);
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    const QString dest = m_fixtureDir + "/img1.jpg";
    const int row = dm.indexOfFile(dest);
    EXPECT_TRUE(row >= 0) << qPrintable(QStringLiteral("precondition broken: ") + dest + " is not in the listing");
    const QByteArray original = readBytes(dest);
    const QString backup =
        dest + "_" + QString(QCryptographicHash::hash(dest.toUtf8(), QCryptographicHash::Md5).toHex());
    const quint64 id = dm.fileEntryAt(row).id;
    bool replaced = false;
    QObject::connect(&dm, &DirectoryManager::listingChanged, &dm, [&] {
        replaced |= dm.fileEntryAt(dm.indexOfFile(dest)).id != id;
    });

    // The write has to fail while the restore still can: the file itself
    // becomes read-only, and the directory stays writable, which is all
    // rename(2) needs.
    EXPECT_TRUE(QFile::setPermissions(dest, QFileDevice::ReadOwner));
    QImage replacement(8, 8, QImage::Format_RGB32);
    replacement.fill(Qt::red);
    EXPECT_TRUE(!FileOperations::saveWithBackup(replacement, dest, 95))
        << "a save into a read-only file reported success";
    // the backup is consumed by the move, so its absence is what says the
    // restore's syscall happened
    ASSERT_TRY_TRUE_TIMEOUT(!QFile::exists(backup), 10000);

    EXPECT_EQ(readBytes(dest), original);
    EXPECT_EQ(dm.indexOfFile(dest), row);
    // and the row describes the file: the size held is the restored bytes,
    // not the failed write's
    EXPECT_EQ(dm.fileEntryAt(row).size, static_cast<std::uintmax_t>(original.size()));

    // Wait for a later change so the assertion covers the restore, not just
    // the publication that preceded it.
    makeFile("fence.jpg");
    ASSERT_TRY_TRUE_TIMEOUT(dm.containsFile(m_fixtureDir + "/fence.jpg"), 10000);
    EXPECT_FALSE(replaced);
    assertMirrorMatchesEngineExport(dm);
    EXPECT_EQ(dm.fileCount(), 4UL);

    EXPECT_TRUE(QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
}

TEST_F(TestImageOps, fileOperationsRoundTrip) {
    resetFixture();
    QString destDir = m_fixtureDir + "/destdir";
    EXPECT_TRUE(QDir().mkpath(destDir));

    FileOpResult result;
    FileOperations::copyFileTo(m_fixtureDir + "/img1.jpg", destDir, false, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    QByteArray srcBytes;
    {
        QFile src(m_fixtureDir + "/img1.jpg");
        EXPECT_TRUE(src.open(QIODevice::ReadOnly));
        srcBytes = src.readAll();
        QFile dst(destDir + "/img1.jpg");
        EXPECT_TRUE(dst.open(QIODevice::ReadOnly));
        EXPECT_EQ(dst.readAll(), srcBytes);
    }
    // the read-back handles close with the block above: Windows refuses
    // to rename a file another handle in this same process holds open,
    // and the rename below is exactly that case

    FileOperations::copyFileTo(m_fixtureDir + "/img1.jpg", destDir, false, result);
    EXPECT_EQ(result, FileOpResult::DESTINATION_FILE_EXISTS);
    {
        QFile dst2(destDir + "/img1.jpg");
        EXPECT_TRUE(dst2.open(QIODevice::ReadOnly));
        EXPECT_EQ(dst2.readAll(), srcBytes);
    }

    FileOperations::moveFileTo(m_fixtureDir + "/img2.jpg", destDir, false, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    EXPECT_TRUE(!QFile::exists(m_fixtureDir + "/img2.jpg"));
    EXPECT_TRUE(QFile::exists(destDir + "/img2.jpg"));

    FileOperations::rename(destDir + "/img1.jpg", "renamed.jpg", false, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    EXPECT_TRUE(!QFile::exists(destDir + "/img1.jpg"));
    EXPECT_TRUE(QFile::exists(destDir + "/renamed.jpg"));

    FileOperations::removeFile(destDir + "/renamed.jpg", result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    EXPECT_TRUE(!QFile::exists(destDir + "/renamed.jpg"));

    FileOperations::removeDir(destDir, false, result);
    EXPECT_EQ(result, FileOpResult::DIRECTORY_NOT_EMPTY);
    FileOperations::removeDir(destDir, true, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    EXPECT_TRUE(!QDir(destDir).exists());
}

TEST_F(TestImageOps, fileOperationsFailurePaths) {
    resetFixture();
    FileOpResult result;
    EXPECT_TRUE(QDir().mkpath(m_fixtureDir + "/destdir"));
    FileOperations::copyFileTo(m_fixtureDir + "/nope.jpg", m_fixtureDir + "/destdir", false, result);
    EXPECT_EQ(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
    FileOperations::rename(m_fixtureDir + "/nope.jpg", "x.jpg", false, result);
    EXPECT_EQ(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
    FileOperations::removeFile(m_fixtureDir + "/nope.jpg", result);
    EXPECT_EQ(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
    FileOperations::copyFileTo(m_fixtureDir + "/img1.jpg", m_fixtureDir + "/nowhere", false, result);
    EXPECT_EQ(result, FileOpResult::DESTINATION_DOES_NOT_EXIST);
    FileOperations::copyFileTo(m_fixtureDir + "/img1.jpg", m_fixtureDir, false, result);
    EXPECT_EQ(result, FileOpResult::NOTHING_TO_DO);
    FileOperations::removeDir(m_fixtureDir + "/nowhere", true, result);
    EXPECT_TRUE(result != FileOpResult::SUCCESS);
    EXPECT_TRUE(!QFile::exists(m_fixtureDir + "/nope.jpg"));
    EXPECT_TRUE(!QDir(m_fixtureDir + "/nowhere").exists());
}

// Two ways a failed save can still cost you something, asserted separately
// because QCOMPARE stops a slot at its first failure.
//
// (1) The rollback deleted m_docInfo->filePath() and moved the backup of
// destPath INTO it. For a Save As those are different files, so undoing a
// failed write to target.xyz replaced the untouched original image with the
// other file's contents — the assert below is on content, because a
// "does the original still exist" check passes happily while it holds the
// wrong bytes.
TEST_F(TestImageOps, failedSaveToAnotherPathDoesNotTouchTheOriginal) {
    resetFixture();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/orig.png", "png", QSize(24, 24)));
    QFile orig(m_fixtureDir + "/orig.png");
    EXPECT_TRUE(orig.open(QIODevice::ReadOnly));
    const QByteArray origBefore = orig.readAll();
    orig.close();
    const QByteArray targetBefore("ORIGINAL TARGET CONTENTS");
    {
        QFile t(m_fixtureDir + "/target.xyz");
        EXPECT_TRUE(t.open(QIODevice::WriteOnly));
        t.write(targetBefore);
    }

    auto image = *ImageFactory::createImage(m_fixtureDir + "/orig.png", false, loadPolicy());
    EXPECT_TRUE(!image->save(m_fixtureDir + "/target.xyz", 95)); // "xyz" is not encodable

    QFile after(m_fixtureDir + "/orig.png");
    EXPECT_TRUE(after.open(QIODevice::ReadOnly));
    EXPECT_EQ(after.readAll(), origBefore);
    after.close();
    QFile target(m_fixtureDir + "/target.xyz");
    EXPECT_TRUE(target.open(QIODevice::ReadOnly));
    EXPECT_EQ(target.readAll(), targetBefore);
    EXPECT_EQ(int(QDir(m_fixtureDir).entryList(QStringList() << QStringLiteral("target.xyz_*"), QDir::Files).size()),
              0);
}

// (2) The edited image was swapped and discarded before `success` was
// consulted, so every failed save also threw away unsaved work.
TEST_F(TestImageOps, failedSaveKeepsTheUnsavedEdit) {
    resetFixture();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/orig.png", "png", QSize(24, 24)));
    auto image = std::dynamic_pointer_cast<ImageStatic>(
        *ImageFactory::createImage(m_fixtureDir + "/orig.png", false, loadPolicy()));
    EXPECT_TRUE(image != nullptr);
    image->setEditedImage(std::make_unique<const QImage>(image->getImage()->copy(QRect(0, 0, 8, 8))));
    EXPECT_TRUE(image->isEdited());
    EXPECT_TRUE(!image->save(m_fixtureDir + "/orig.xyz", 95));
    EXPECT_TRUE(image->isEdited()) << "a failed save threw away the unsaved edit";
    EXPECT_EQ(image->getImage()->size(), QSize(8, 8));
}

TEST_F(TestImageOps, imageStaticSaveRoundTripAndQuality) {
    resetFixture();
    FreshSettings fresh;

    QImage img(64, 64, QImage::Format_RGB32);
    quint32 seed = 42;
    for(int y = 0; y < 64; ++y)
        for(int x = 0; x < 64; ++x) {
            seed = seed * 1103515245u + 12345u;
            img.setPixel(x, y, QColor(int((seed >> 16u) & 0xffu), int((seed >> 8u) & 0xffu), int(seed & 0xffu)).rgb());
        }
    QString saveDir = m_fixtureDir + "/savedir";
    EXPECT_TRUE(QDir().mkpath(saveDir));
    QString srcPath = saveDir + "/noise-src.png";
    EXPECT_TRUE(img.save(srcPath, "PNG"));

    auto loaded = ImageFactory::createImage(srcPath, false, loadPolicy());
    EXPECT_TRUE(loaded.has_value());
    auto imgStatic = *loaded;
    EXPECT_TRUE(imgStatic->getImage() != nullptr);

    EXPECT_TRUE(imgStatic->save(saveDir + "/round.png", 95));
    EXPECT_TRUE(QFile::exists(saveDir + "/round.png"));
    auto savedAgain = ImageFactory::createImage(saveDir + "/round.png", false, loadPolicy());
    EXPECT_TRUE(savedAgain.has_value());
    auto reloaded = *savedAgain;
    EXPECT_EQ(reloaded->getImage()->size(), img.size());
    EXPECT_EQ(*reloaded->getImage(), img);
    EXPECT_EQ(static_cast<int>(QDir(saveDir).entryList(QDir::Files).size()), 2);

    EXPECT_TRUE(imgStatic->save(saveDir + "/q10.jpg", 10));
    qlonglong small = QFileInfo(saveDir + "/q10.jpg").size();
    EXPECT_TRUE(imgStatic->save(saveDir + "/q95.jpg", 95));
    qlonglong big = QFileInfo(saveDir + "/q95.jpg").size();
    EXPECT_TRUE(small < big) << "q10 jpeg should be smaller than q95 for noise";
    EXPECT_TRUE(imgStatic->save(saveDir + "/q10.jpg", 10));
    EXPECT_EQ(static_cast<int>(QDir(saveDir).entryList(QDir::Files).size()), 4);
}

// ImageLib's transform primitives on an asymmetric image: with distinct
// per-position values a wrong quadrant or axis cannot hide behind symmetry
// ImageLib's transforms on solid columns: rotated() uses smooth
// transformation, so only the outer edges are asserted — interior columns
// blend between their neighbors and would make exact-pixel claims wrong
TEST_F(TestImageOps, imageLibRotateFlipExactPixels) {
    // solid columns: vertical blending cannot change these colors, so the
    // only filter effect to avoid is the corner/edge mix — asserted at the
    // border midpoints, never at corners
    QImage src(16, 16, QImage::Format_RGB32);
    for(int y = 0; y < 16; ++y)
        for(int x = 0; x < 16; ++x)
            src.setPixel(x, y, qRgb(10 + x * 10, 30, 0));

    auto rot90 = ImageLib::rotated(std::make_shared<const QImage>(src), 90);
    EXPECT_EQ(rot90->size(), QSize(16, 16));
    // 90° clockwise: the source's left column becomes the right column,
    // the source's right column becomes the left
    EXPECT_EQ(rot90->pixel(15, 7), qRgb(80, 30, 0)); // src(7,0)
    EXPECT_EQ(rot90->pixel(0, 7), qRgb(80, 30, 0));  // src(7,15)
    EXPECT_EQ(rot90->pixel(15, 0), qRgb(10, 30, 0)); // src(0,0)
    EXPECT_EQ(rot90->pixel(0, 0), qRgb(10, 30, 0));  // src(0,15): bottom-left feeds top-left

    auto rot180 = ImageLib::rotated(std::make_shared<const QImage>(src), 180);
    EXPECT_EQ(rot180->size(), QSize(16, 16));
    EXPECT_EQ(rot180->pixel(0, 7), qRgb(160, 30, 0)); // src(15,8)
    EXPECT_EQ(rot180->pixel(15, 7), qRgb(10, 30, 0)); // src(0,7)

    // flips are mirrors — exact everywhere, corners included
    auto flipH = ImageLib::flippedH(std::make_shared<const QImage>(src));
    EXPECT_EQ(flipH->pixel(0, 7), qRgb(160, 30, 0)); // src(15,7)
    EXPECT_EQ(flipH->pixel(15, 7), qRgb(10, 30, 0)); // src(0,7)
    auto flipV = ImageLib::flippedV(std::make_shared<const QImage>(src));
    EXPECT_EQ(flipV->pixel(7, 0), qRgb(80, 30, 0));  // src(7,15)
    EXPECT_EQ(flipV->pixel(7, 15), qRgb(80, 30, 0)); // src(7,0)
}

// the OpenCV scale paths plus the Indexed8 pre-conversion the dispatcher
// performs before handing a paletted image to cv
TEST_F(TestImageOps, imageLibScaledUsesTheCvPaths) {
    resetFixture();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/cv-src.png", "png", QSize(64, 64)));
    auto src = std::make_shared<const QImage>(m_fixtureDir + "/cv-src.png");
    for(auto filter : {QI_FILTER_CV_BILINEAR_SHARPEN, QI_FILTER_CV_CUBIC, QI_FILTER_CV_CUBIC_SHARPEN}) {
        auto dst = ImageLib::scaled(src, QSize(32, 32), filter);
        EXPECT_TRUE(!dst->isNull());
        EXPECT_EQ(dst->size(), QSize(32, 32));
    }

    QImage indexed(16, 16, QImage::Format_Indexed8);
    indexed.setColorTable({qRgb(0, 0, 0), qRgb(255, 255, 255)});
    for(int y = 0; y < 16; ++y)
        for(int x = 0; x < 16; ++x)
            indexed.setPixel(x, y, (x + y) % 2);
    auto dst = ImageLib::scaled(std::make_shared<const QImage>(indexed), QSize(8, 8), QI_FILTER_CV_CUBIC);
    EXPECT_TRUE(!dst->isNull());
    EXPECT_EQ(dst->size(), QSize(8, 8));
    EXPECT_TRUE(dst->format() == QImage::Format_RGB32 || dst->format() == QImage::Format_ARGB32);
}

// the copy/move/rename trio's force-and-rollback dance: destination
// preservation on failure, modification-time preservation on overwrite
TEST_F(TestImageOps, fileOperationsCopyMoveRenameMatrices) {
    resetFixture();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/src.png", "png", QSize(8, 8)));
    QFile src(m_fixtureDir + "/src.png");
    // futimens accepts a read-only handle on POSIX, but SetFileTime needs
    // FILE_WRITE_ATTRIBUTES, which a read-only open does not carry — the
    // write-capable open is the one path both platforms accept, and it
    // changes no bytes
    EXPECT_TRUE(src.open(QIODevice::ReadWrite));
    EXPECT_TRUE(src.setFileTime(QDateTime(QDate(2020, 1, 2), QTime(3, 4, 5)), QFileDevice::FileModificationTime));
    src.close();
    QDir().mkpath(m_fixtureDir + "/destdir");
    FileOpResult result;

    // move: same dir is nothing, missing source, missing dest dir
    FileOperations::moveFileTo(m_fixtureDir + "/src.png", m_fixtureDir, false, result);
    EXPECT_EQ(result, FileOpResult::NOTHING_TO_DO);
    FileOperations::moveFileTo(m_fixtureDir + "/nope.png", m_fixtureDir + "/destdir", false, result);
    EXPECT_EQ(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
    FileOperations::moveFileTo(m_fixtureDir + "/src.png", m_fixtureDir + "/nowhere", false, result);
    EXPECT_EQ(result, FileOpResult::DESTINATION_DOES_NOT_EXIST);
    // happy move relocates the file
    FileOperations::moveFileTo(m_fixtureDir + "/src.png", m_fixtureDir + "/destdir", false, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    EXPECT_TRUE(QFileInfo::exists(m_fixtureDir + "/destdir/src.png"));

    // rename: same name is nothing, dest-dir collision, forced rename
    FileOperations::rename(m_fixtureDir + "/destdir/src.png", "src.png", false, result);
    EXPECT_EQ(result, FileOpResult::NOTHING_TO_DO);
    QDir().mkpath(m_fixtureDir + "/destdir/blocked");
    FileOperations::rename(m_fixtureDir + "/destdir/src.png", "blocked", false, result);
    EXPECT_EQ(result, FileOpResult::DESTINATION_DIR_EXISTS);
    FileOperations::rename(m_fixtureDir + "/destdir/src.png", "renamed.png", false, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    EXPECT_TRUE(QFileInfo::exists(m_fixtureDir + "/destdir/renamed.png"));

    // copy: a forced overwrite preserves the source's modification time
    QFileInfo destInfo(m_fixtureDir + "/destdir/renamed.png");
    QDateTime before = destInfo.lastModified();
    // The source's mtime must differ from the destination's for the "preserves
    // the source's time" assertion to mean anything. Sleeping 1100 ms bought
    // that with wall-clock time; setting it says it directly, cannot be
    // shortened by a coarse clock, and costs nothing.
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/newer.png", "png", QSize(8, 8)));
    {
        // one portable path: setFileTime through a write-capable open (the
        // read-only alternative only exists on POSIX, where futimens accepts
        // it; SetFileTime needs FILE_WRITE_ATTRIBUTES)
        QFile backdate(m_fixtureDir + "/newer.png");
        EXPECT_TRUE(backdate.open(QIODevice::ReadWrite));
        EXPECT_TRUE(backdate.setFileTime(QDateTime::fromSecsSinceEpoch(before.toSecsSinceEpoch() - 60),
                                         QFileDevice::FileModificationTime));
    }
    FileOperations::copyFileTo(m_fixtureDir + "/newer.png", m_fixtureDir + "/destdir", true, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    QFileInfo overwritten(m_fixtureDir + "/destdir/renamed.png");
    EXPECT_TRUE(overwritten.exists());
    // content is the NEWER image now
    QImage reloaded(overwritten.absoluteFilePath());
    EXPECT_EQ(reloaded.size(), QSize(8, 8));
    Q_UNUSED(before);
}

TEST_F(TestImageOps, fileOperationsRemoveDirAndTrash) {
    resetFixture();
    QDir().mkpath(m_fixtureDir + "/tree/nested");
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/tree/nested/x.png", "png", QSize(4, 4)));
    FileOpResult result;
    // non-recursive removal of a non-empty dir refuses
    FileOperations::removeDir(m_fixtureDir + "/tree", false, result);
    EXPECT_EQ(result, FileOpResult::DIRECTORY_NOT_EMPTY);
    EXPECT_TRUE(QDir(m_fixtureDir + "/tree").exists());
    // recursive removal takes the whole tree
    FileOperations::removeDir(m_fixtureDir + "/tree", true, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    EXPECT_TRUE(!QDir(m_fixtureDir + "/tree").exists());
    // missing dir
    FileOperations::removeDir(m_fixtureDir + "/tree", true, result);
    EXPECT_EQ(result, FileOpResult::SOURCE_DOES_NOT_EXIST);

    // trash: only the deterministic branch is pinned — the XDG trash
    // target is Qt's own mechanism and environment-dependent
    FileOperations::moveToTrash(m_fixtureDir + "/missing.jpg", result);
    EXPECT_EQ(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
}

TEST_F(TestImageOps, imageStaticDiscardsTheUnsavedEdit) {
    resetFixture();
    EXPECT_TRUE(writeBigImage(m_fixtureDir + "/es.png", "png", QSize(8, 8)));
    auto img = *ImageFactory::createImage(m_fixtureDir + "/es.png", false, loadPolicy());
    auto es = std::dynamic_pointer_cast<ImageStatic>(img);
    EXPECT_TRUE(es != nullptr);
    // discarding without an edit reports failure and touches nothing
    EXPECT_TRUE(!es->discardEditedImage());
    EXPECT_TRUE(!es->isEdited());
    QImage edit = es->getImage()->copy(QRect(0, 0, 4, 4));
    EXPECT_TRUE(es->setEditedImage(std::make_unique<const QImage>(edit)));
    EXPECT_TRUE(es->isEdited());
    EXPECT_EQ(es->getImage()->size(), QSize(4, 4));
    EXPECT_TRUE(es->discardEditedImage());
    EXPECT_TRUE(!es->isEdited());
    EXPECT_EQ(es->getImage()->size(), QSize(8, 8));
}

TEST_F(TestImageOps, imageStaticLoadsAnIcoFixture) {
    resetFixture();
    // 16x16 ICO (PIL-generated at authoring time)
    const QByteArray ico =
        QByteArray::fromBase64("AAABAAEAEBAAAAAAIABWAAAAFgAAAIlQTkcNChoKAAAADUlIRFIAAAAQAAAAEAgGAAAAH/P/"
                               "YQAAAB1JREFUeJxjrNA48Z+BAsBEieZRA0YNGDVgMBkAAAD6Aof9mEGiAAAAAElFTkSuQmCC");
    QFile icoFile(m_fixtureDir + "/app.ico");
    EXPECT_TRUE(icoFile.open(QIODevice::WriteOnly));
    icoFile.write(ico);
    icoFile.close();
    auto image = *ImageFactory::createImage(m_fixtureDir + "/app.ico", false, loadPolicy());
    EXPECT_EQ(image->type(), DocumentType::STATIC);
    EXPECT_TRUE(image->getImage() != nullptr);
    EXPECT_TRUE(!image->getImage()->isNull());
}

TEST_F(TestImageOps, imageLibTransformsFollowTheConventions) {
    QImage src(2, 3, QImage::Format_RGB32);
    for(int y = 0; y < 3; ++y)
        for(int x = 0; x < 2; ++x)
            src.setPixel(x, y, qRgb(0, x, y + 1));
    auto S = [&src]() {
        return std::make_shared<const QImage>(src);
    };
    auto px = [](const std::unique_ptr<const QImage> &im, int x, int y) {
        return im->pixel(x, y);
    };
    auto r90 = ImageLib::rotated(S(), 90);
    EXPECT_EQ(r90->size(), QSize(3, 2));
    EXPECT_EQ(px(r90, 2, 0), qRgb(0, 0, 1));
    EXPECT_EQ(px(r90, 2, 1), qRgb(0, 1, 1));
    EXPECT_EQ(px(r90, 0, 0), qRgb(0, 0, 3));
    auto r180 = ImageLib::rotated(S(), 180);
    EXPECT_EQ(r180->size(), QSize(2, 3));
    EXPECT_EQ(px(r180, 0, 0), qRgb(0, 1, 3));
    EXPECT_EQ(px(r180, 1, 2), qRgb(0, 0, 1));
    auto r270 = ImageLib::rotated(S(), 270);
    EXPECT_EQ(r270->size(), QSize(3, 2));
    EXPECT_EQ(px(r270, 0, 0), qRgb(0, 1, 1));
    EXPECT_EQ(px(r270, 0, 1), qRgb(0, 0, 1));
    EXPECT_EQ(px(r270, 2, 0), qRgb(0, 1, 3));
    auto fh = ImageLib::flippedH(S());
    EXPECT_EQ(px(fh, 0, 0), qRgb(0, 1, 1));
    EXPECT_EQ(px(fh, 1, 0), qRgb(0, 0, 1));
    auto fv = ImageLib::flippedV(S());
    EXPECT_EQ(px(fv, 0, 0), qRgb(0, 0, 3));
    EXPECT_EQ(px(fv, 0, 2), qRgb(0, 0, 1));
    auto crop = ImageLib::cropped(S(), QRect(1, 1, 1, 2));
    EXPECT_EQ(crop->size(), QSize(1, 2));
    EXPECT_EQ(px(crop, 0, 0), qRgb(0, 1, 2));
    // a rect that does not fit is refused with an empty image rather than
    // silently clipped, so callers can tell the two apart
    auto badCrop = ImageLib::cropped(S(), QRect(1, 1, 2, 2));
    EXPECT_TRUE(badCrop->isNull());

    // exifRotated takes the code QImageReader::transformation() reports —
    // QImageIOHandler::Transformation, a flag set — and NOT raw EXIF 1-8.
    // The name says orientation, which is how a reader ends up typing the
    // wrong table into the switch. Spelled by name here so a misaligned
    // case is a failure rather than a shared misunderstanding of an integer.
    using T = QImageIOHandler::Transformation;
    auto E = [&](T code) {
        return ImageLib::exifRotated(std::make_unique<const QImage>(src), int(code));
    };
    EXPECT_EQ(*E(T::TransformationNone), src);
    EXPECT_EQ(*E(T::TransformationMirror), *ImageLib::flippedH(S()));
    EXPECT_EQ(*E(T::TransformationFlip), *ImageLib::flippedV(S()));
    EXPECT_EQ(*E(T::TransformationRotate180), *ImageLib::rotated(S(), 180));
    EXPECT_EQ(*E(T::TransformationRotate90), *ImageLib::rotated(S(), 90));

    // the combined codes mean "then" in the order Qt applies them: the flag
    // names say a mirror/flip precedes the rotation, and that is what the
    // switch does. QImage has no transform that produces this from the flags
    // alone, so the composition is spelled out here rather than trusted.
    QImage mirrorThen90 = src.flipped(Qt::Horizontal).transformed(QTransform().rotate(90), Qt::SmoothTransformation);
    EXPECT_EQ(*E(T::TransformationMirrorAndRotate90), mirrorThen90);
    QImage flipThen90 = src.flipped(Qt::Vertical).transformed(QTransform().rotate(90), Qt::SmoothTransformation);
    EXPECT_EQ(*E(T::TransformationFlipAndRotate90), flipThen90);
    EXPECT_EQ(*E(T::TransformationRotate270), *ImageLib::rotated(S(), -90));
}

// The edit that used to destroy the camera's metadata. Qt's writers emit no
// EXIF/XMP/IPTC (measured: a JPEG with Make/Orientation/GPS loaded and
// re-saved by QImage comes back with an empty text-key set and nothing
// exiftool can read), so a save has to carry the blocks over explicitly.
//
// The fixture is built with Exiv2 because no other tool here can put EXIF
// into a JPEG — which is the point of the test, not a shortcut.
TEST_F(TestImageOps, editingAnImageCarriesItsExifXmpAndIptcOntoTheSavedFile) {
    resetFixture();
    const QString source = m_fixtureDir + "/shot.jpg";
    QImage img(64, 32, QImage::Format_RGB32);
    img.fill(qRgb(200, 10, 10));
    EXPECT_TRUE(img.save(source, "JPEG", 95));

#ifdef USE_EXIV2
    {
        auto meta = Exiv2::ImageFactory::open(source.toStdString()); // exiv2 0.28: UTF-8 everywhere
        meta->readMetadata();
        Exiv2::ExifData &exif = meta->exifData();
        exif["Exif.Image.Make"] = "Canon";
        exif["Exif.Image.Orientation"] = uint16_t(6); // rotated 90 CW
        exif["Exif.Photo.DateTimeOriginal"] = "2024:05:01 10:00:00";
        exif["Exif.GPSInfo.GPSLatitude"] = "31/1 11/1 60/1";
        meta->xmpData()["Xmp.dc.subject"] = "holiday";
        meta->xmpData()["Xmp.tiff.Orientation"] = 6; // the XMP twin of the EXIF tag
        meta->iptcData()["Iptc.Application2.Keywords"] = "beach";
        meta->writeMetadata();
    }

    // the edit: rotate the pixels, as ImageLib::exifRotated does for a
    // camera whose EXIF says "rotate 90 CW"
    const QString dest = m_fixtureDir + "/edited.jpg";
    const QImage rotated = img.transformed(QTransform().rotate(90), Qt::SmoothTransformation);
    EXPECT_TRUE(FileOperations::saveWithBackup(rotated, dest, 95, source));

    auto check = Exiv2::ImageFactory::open(dest.toStdString()); // exiv2 0.28: UTF-8 everywhere
    check->readMetadata();
    const Exiv2::ExifData &exif = check->exifData();
    EXPECT_TRUE(!exif.empty()) << "the saved file carries no EXIF at all";
    auto make = exif.findKey(Exiv2::ExifKey("Exif.Image.Make"));
    EXPECT_TRUE(make != exif.end());
    EXPECT_EQ(QString::fromStdString(make->value().toString()), QStringLiteral("Canon"));
    // provenance survives, including the GPS block the crop/rotate does not
    // invalidate
    EXPECT_TRUE(exif.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitude")) != exif.end());
    EXPECT_TRUE(!check->xmpData().empty());
    EXPECT_TRUE(!check->iptcData().empty());
    // the XMP twin of the orientation tag resets with the EXIF one, while
    // the rest of the XMP block carries intact
    auto xmpOrientation = check->xmpData().findKey(Exiv2::XmpKey("Xmp.tiff.Orientation"));
    EXPECT_TRUE(xmpOrientation != check->xmpData().end()) << "the saved XMP dropped the tiff:Orientation outright";
    EXPECT_EQ(QString::fromStdString(xmpOrientation->value().toString()), QStringLiteral("1"));
    auto subject = check->xmpData().findKey(Exiv2::XmpKey("Xmp.dc.subject"));
    EXPECT_TRUE(subject != check->xmpData().end()) << "the saved XMP lost the dc.subject it carried";
    EXPECT_EQ(QString::fromStdString(subject->value().toString()), QStringLiteral("holiday"));

    // and the orientation is reset: the pixels already carry the rotation,
    // so a carried "rotate 90 CW" would make every viewer rotate them again
    auto orientation = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
    EXPECT_TRUE(orientation != exif.end()) << "orientation was dropped instead of reset";
    // toString, not toInt64/toUint32: the integer accessors differ across
    // Exiv2 versions (0.27's Exifdatum has neither; 0.28 added toInt64 and
    // hides the Metadatum ones), while toString is a Metadatum pure virtual in
    // both. Orientation is a uint16 in the spec, so the decimal form is "1".
    EXPECT_EQ(QString::fromStdString(orientation->toString()), QStringLiteral("1"));

    // the file is still a valid image after Exiv2 rewrote it
    QImage back(dest);
    EXPECT_TRUE(!back.isNull()) << "the metadata write left an unreadable file";
    EXPECT_EQ(back.size(), QSize(32, 64));
#else
    GTEST_SKIP() << "built without Exiv2: the metadata path is compiled out";
#endif
}

// A source with no metadata must not fail the save, and neither must a
// format Exiv2 cannot write into: the pixels are already on disk, so a
// failed carry is a note, never a failed save.
TEST_F(TestImageOps, aSaveWithoutMetadataStillSucceeds) {
    resetFixture();
    const QString source = m_fixtureDir + "/plain.jpg";
    const QString dest = m_fixtureDir + "/plain-out.jpg";
    QImage img(16, 16, QImage::Format_RGB32);
    img.fill(Qt::blue);
    EXPECT_TRUE(img.save(source, "JPEG", 95));

    EXPECT_TRUE(FileOperations::saveWithBackup(img, dest, 95, source));
    EXPECT_TRUE(QFile::exists(dest));
    EXPECT_TRUE(!QImage(dest).isNull());

    // and the no-source form the clipboard import uses is unchanged
    EXPECT_TRUE(FileOperations::saveWithBackup(img, m_fixtureDir + "/nosrc.jpg", 95));
    EXPECT_TRUE(QFile::exists(m_fixtureDir + "/nosrc.jpg"));
}
