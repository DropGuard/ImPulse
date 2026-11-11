#include "thumbnailcache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QtEndian>
#include <QImageWriter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QVector>

#include <algorithm>

#include <zlib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

namespace {

// The spec's directory modes: 700 for every directory in the store (including
// the root) and 600 for every thumbnail. QDir::mkpath respects the umask and
// lands on 775 under the common 002, so both have to be forced — the spec's
// reason is that a thumbnail of a file only its owner may read must not be
// readable by anyone else through the cache.
constexpr mode_t dirMode = 0700;
constexpr mode_t fileMode = 0600;

// The two PNG keys this app adds. The spec reserves X- for application data and
// requires it to be prefixed, which is what keeps a foreign reader from
// interpreting them.
constexpr auto keyAnimated = "X-ImPulse-Animated";
constexpr auto keyVideo = "X-ImPulse-Video";

bool ensureDir(const QString &path) {
    QDir dir(path);
    if(!dir.exists() && !QDir().mkpath(path))
        return false;
    ::chmod(path.toLocal8Bit().constData(), dirMode);
    return true;
}

// A PNG text value, inflated when the chunk stored it compressed. zlib's
// uncompress() rather than qUncompress(): the latter expects Qt's own 4-byte
// length prefix, and a zTXt payload is a bare zlib stream — feeding one to it
// fails with "Input data is corrupted", which is how this was found.
QString inflateText(const QByteArray &packed) {
    if(packed.isEmpty())
        return QString();
    // a thumbnail key is a URI or a small number; this bound only stops a
    // malformed stream from allocating without limit
    constexpr uLongf maxText = 64 * 1024;
    QByteArray out(int(maxText), Qt::Uninitialized);
    uLongf outLen = maxText;
    if(::uncompress(reinterpret_cast<Bytef *>(out.data()), &outLen, reinterpret_cast<const Bytef *>(packed.constData()),
                    uLong(packed.size())) != Z_OK)
        return QString();
    out.truncate(int(outLen));
    return QString::fromUtf8(out);
}

// The text of one chunk, given where its keyword ends. Returns a null QString
// when the chunk's layout does not describe a readable value, which the caller
// treats as "this key is not present" — the sweep only ever acts on what it
// could actually read.
QString textValueOf(const QByteArray &type, const QByteArray &body, int keywordEnd) {
    if(type == "tEXt")
        return QString::fromUtf8(body.mid(keywordEnd + 1));

    if(type == "zTXt") {
        // keyword NUL, compression method byte (only 0 is defined), then the
        // zlib stream
        return inflateText(body.mid(keywordEnd + 2));
    }

    if(type != "iTXt")
        return QString();

    // keyword NUL, compression flag, method, language tag NUL, translated
    // keyword NUL, then the text. Thumbnail keys are ASCII, so the
    // language/translation fields are consumed and discarded.
    const int method = keywordEnd + 1;
    if(body.size() <= method + 2)
        return QString();
    const bool compressed = body.at(method) != 0;
    const int language = body.indexOf('\0', method + 2);
    if(language < 0)
        return QString();
    const int translated = body.indexOf('\0', language + 1);
    if(translated < 0)
        return QString();
    const QByteArray text = body.mid(translated + 1);
    return compressed ? inflateText(text) : QString::fromUtf8(text);
}

// The text keys of a PNG, read by walking chunks up to IDAT and never decoding
// pixels: the sweep runs over every entry, so it may cost one open and a few
// hundred bytes, not a decode.
//
// All three chunk types have to be handled, and that is not a detail: libpng
// stores a value COMPRESSED once it is long enough, and Qt reads that path
// back transparently, so a writer that only looks at tEXt sees a file missing
// the very key it is looking for — measured here as a thumbnail whose
// Thumb::URI sits in zTXt while its mtime sits in tEXt, which made the whole
// entry look foreign. Anything that still cannot be read is skipped; both
// callers treat "cannot read it" as "leave the file alone".
QHash<QString, QString> pngTextKeys(const QString &path) {
    QHash<QString, QString> keys;
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly))
        return keys;
    if(file.read(8) != QByteArrayLiteral("\x89PNG\r\n\x1a\n"))
        return keys;
    for(;;) {
        const QByteArray header = file.read(8); // length + type
        if(header.size() != 8)
            break;
        const quint32 length = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(header.constData()));
        const QByteArray type = header.mid(4);
        if(type == "IDAT" || type == "IEND")
            break;
        // A thumbnail's text chunks are a few hundred bytes; this bound keeps a
        // malformed length field from making the sweep read a huge file.
        constexpr quint32 maxChunk = 64 * 1024;
        if(length > maxChunk)
            break;
        const QByteArray body = file.read(length);
        file.seek(file.pos() + 4); // CRC
        if(body.size() != int(length))
            break;
        const int nul = body.indexOf('\0');
        if(nul <= 0)
            continue;
        const QString value = textValueOf(type, body, nul);
        if(!value.isNull())
            keys.insert(QString::fromLatin1(body.left(nul)), value);
    }
    return keys;
}

// Ownership is decided by the Software key, because the store is shared: the
// idle sweep and the byte cap are this app's policy, and imposing them on a
// thumbnail Dolphin wrote would force Dolphin to regenerate it. An entry whose
// Software cannot be read is never deleted — the failure direction is "we keep
// someone else's file", never "we delete it".
bool writtenByUs(const QHash<QString, QString> &keys) {
    return keys.value(QStringLiteral("Software")).startsWith(QLatin1String("ImPulse"));
}

// Spec 9's first rule: "a thumbnail should be deleted if the original file
// doesn't exist anymore". Only file: URIs are checked — a network or
// removable-media original that is merely off-line looks exactly like a gone
// one, and the spec says to age those out instead. A URI that cannot be read is
// not proof of an orphan, so it is left alone.
bool originalIsGone(const QHash<QString, QString> &keys) {
    const QString uri = keys.value(QStringLiteral("Thumb::URI"));
    if(!uri.startsWith(QLatin1String("file://")))
        return false;
    const QString local = QUrl(uri).toLocalFile();
    return !local.isEmpty() && !QFileInfo::exists(local);
}

// Why an entry may be removed, decided in one place. Orphan wins over idle
// because it is the stronger fact: a thumbnail whose original is gone is
// garbage whatever its age, and reporting it as "expired" would hide the
// difference in the log.
enum class DropReason {
    Keep,
    Orphan,
    Idle
};

DropReason dropReasonFor(const QHash<QString, QString> &keys, const QFileInfo &info, const QDateTime &cutoff) {
    // the store is shared: another program's thumbnail is not ours to expire,
    // even when its original is gone
    if(!writtenByUs(keys))
        return DropReason::Keep;
    if(originalIsGone(keys))
        return DropReason::Orphan;
    if(info.lastModified() < cutoff)
        return DropReason::Idle;
    return DropReason::Keep;
}

} // namespace

QString ThumbnailCache::rootDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/thumbnails");
}

QString ThumbnailCache::uriFor(const QString &localPath) {
    // FullyEncoded, because that is the canonical minimally-encoded form the
    // spec requires writers and readers to agree on. Checked byte for byte
    // against a live store: KIO stores percent-escaped UTF-8 with spaces as
    // %20 and unreserved punctuation literal, which is exactly what this gives.
    return QUrl::fromLocalFile(localPath).toString(QUrl::FullyEncoded);
}

QString ThumbnailCache::bucketName(int bucket) {
    switch(bucket) {
    case 128: return QStringLiteral("normal");
    case 256: return QStringLiteral("large");
    case 512: return QStringLiteral("x-large");
    default: return QStringLiteral("xx-large");
    }
}

int ThumbnailCache::bucketForRequest(int requestedPixels, bool crop, const QSize &sourceSize) {
    // normal/ is the spec's floor: a smaller request has no bucket of its own,
    // and the spec's answer for one is "read the 128x128 thumbnail and scale it
    // down", so the request is clamped up into the smallest bucket.
    qint64 needed = std::max(requestedPixels, bucketSizes[0]);
    if(crop) {
        // Cropped display covers the square, so the long edge of the stored
        // image has to reach requested * long/short; the short edge survives
        // the crop at `requested` or more. Without source geometry (the probe
        // does not know every format's dimensions) assume 16:10, the commonest
        // photo shape and close to the 16:9 that dominates video.
        double aspect = 1.6;
        if(sourceSize.isValid() && sourceSize.width() > 0 && sourceSize.height() > 0)
            aspect = double(std::max(sourceSize.width(), sourceSize.height())) /
                     double(std::min(sourceSize.width(), sourceSize.height()));
        needed = qMax(needed, qRound64(double(requestedPixels) * std::max(aspect, 1.0)));
    }
    for(int i = 0; i < bucketCount; ++i)
        if(bucketSizes[i] >= needed)
            return bucketSizes[i];
    return bucketSizes[bucketCount - 1];
}

ThumbnailCache::ThumbnailCache(const QString &_rootPath) : rootPath(_rootPath) {
}

QString ThumbnailCache::thumbnailPath(const QString &sourcePath, int bucket) const {
    const QByteArray digest = QCryptographicHash::hash(uriFor(sourcePath).toUtf8(), QCryptographicHash::Md5).toHex();
    return QDir(rootPath).filePath(bucketName(bucket) + QLatin1Char('/') + QString::fromLatin1(digest) +
                                   QStringLiteral(".png"));
}

bool ThumbnailCache::exists(const QString &sourcePath, int bucket) const {
    const QFileInfo file(thumbnailPath(sourcePath, bucket));
    return file.exists() && file.isReadable();
}

std::optional<CachedThumbnail> ThumbnailCache::read(const QString &sourcePath, int bucket, const QFileInfo &source) {
    // Spec 6.1: an unreadable original is neither looked up nor written. Reading
    // a cached thumbnail for it would defeat the point of the permission —
    // thumbnailing must start working again when the permission is restored.
    if(!source.isReadable())
        return std::nullopt;

    const QString filePath = thumbnailPath(sourcePath, bucket);
    QFileInfo file(filePath);
    if(!file.exists() || !file.isReadable())
        return std::nullopt;

    QImageReader reader(filePath, "png");
    QImage image = reader.read();
    if(image.isNull())
        return std::nullopt;

    // Section 7's validity test, in its own order. Thumb::URI and Thumb::MTime
    // are mandatory in a global store; a thumbnail without them is not ours to
    // trust, and "must be recreated in any case".
    const QString storedUri = image.text(QStringLiteral("Thumb::URI"));
    const QString storedMTime = image.text(QStringLiteral("Thumb::MTime"));
    if(storedUri.isEmpty() || storedMTime.isEmpty() || storedUri != uriFor(sourcePath))
        return std::nullopt;
    // Whole seconds, as the spec stores it. This is the one property this fork
    // gives up against its old millisecond key: a file rewritten within the
    // same second and at the same size serves the previous picture until its
    // mtime moves again.
    if(storedMTime != QString::number(source.lastModified().toSecsSinceEpoch()))
        return std::nullopt;
    const QString storedSize = image.text(QStringLiteral("Thumb::Size"));
    if(!storedSize.isEmpty() && storedSize != QString::number(source.size()))
        return std::nullopt;

    CachedThumbnail hit;
    hit.image = std::move(image);
    const int w = hit.image.text(QStringLiteral("Thumb::Image::Width")).toInt();
    const int h = hit.image.text(QStringLiteral("Thumb::Image::Height")).toInt();
    if(w > 0 && h > 0)
        hit.originalSize = QSize(w, h);
    hit.animated = hit.image.text(QLatin1String(keyAnimated)) == QLatin1String("1");
    hit.video = hit.image.text(QLatin1String(keyVideo)) == QLatin1String("1");

    // A hit means "used today": the idle clock is the file's mtime, so a
    // thumbnail still being displayed survives the sweep. utimensat because
    // Qt's setFileTime needs the file open for write and a write-open truncates.
    const struct timespec times[2] = {{0, UTIME_OMIT}, {0, UTIME_NOW}};
    ::utimensat(AT_FDCWD, filePath.toLocal8Bit().constData(), times, 0);
    return hit;
}

bool ThumbnailCache::save(QImage &image, const QString &sourcePath, int bucket, const QFileInfo &source,
                          const QSize &originalSize, bool animated, bool video) {
    if(image.isNull() || !source.isReadable())
        return false;

    const QString filePath = thumbnailPath(sourcePath, bucket);
    if(!ensureDir(QFileInfo(filePath).absolutePath()))
        return false;

    image.setText(QStringLiteral("Thumb::URI"), uriFor(sourcePath));
    image.setText(QStringLiteral("Thumb::MTime"), QString::number(source.lastModified().toSecsSinceEpoch()));
    image.setText(QStringLiteral("Thumb::Size"), QString::number(source.size()));
    image.setText(QStringLiteral("Software"), QStringLiteral("ImPulse"));
    if(originalSize.isValid()) {
        image.setText(QStringLiteral("Thumb::Image::Width"), QString::number(originalSize.width()));
        image.setText(QStringLiteral("Thumb::Image::Height"), QString::number(originalSize.height()));
    }
    if(animated)
        image.setText(QLatin1String(keyAnimated), QLatin1String("1"));
    if(video)
        image.setText(QLatin1String(keyVideo), QLatin1String("1"));

    // Spec 6.2: write to a temporary file in the same directory, then rename.
    // QSaveFile is exactly that, and its rename is atomic, so a reader that
    // loses the race to a concurrent writer sees either the old complete file
    // or the new one — never a torn one. The mode has to be forced: QSaveFile
    // creates with 0666 & ~umask, which is 664 here.
    QSaveFile out(filePath);
    if(!out.open(QIODevice::WriteOnly))
        return false;
    ::fchmod(static_cast<int>(out.handle()), fileMode);
    QImageWriter writer;
    writer.setDevice(&out);
    writer.setFormat("png");
    if(!writer.write(image)) {
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

ThumbnailCache::PurgeReport ThumbnailCache::purgeOlderThan(int days, qint64 byteCap) {
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    PurgeReport report;

    struct Entry {
        QString path;
        qint64 size;
        QDateTime mtime;
    };
    QVector<Entry> ours;
    qint64 totalBytes = 0;

    for(int i = 0; i < bucketCount; ++i) {
        const QDir dir(QDir(rootPath).filePath(bucketName(bucketSizes[i])));
        const QStringList entries = dir.entryList(QStringList() << QStringLiteral("*.png"), QDir::Files);
        for(const QString &name : entries) {
            const QString path = dir.filePath(name);
            const QFileInfo info(path);
            totalBytes += info.size();

            // The store is shared, so one walk answers all four questions:
            // how much it holds (the fuse measures everyone), whether the entry
            // is ours to delete, whether its original is gone, and whether it is
            // old enough to expire. The decision is made once, in one place, and
            // the caller only applies it — two nested "remove() && continue"
            // sites are how the orphan and idle rules end up disagreeing about
            // what a failed removal means.
            const QHash<QString, QString> keys = pngTextKeys(path);
            const DropReason reason = dropReasonFor(keys, info, cutoff);
            if(reason == DropReason::Keep) {
                if(writtenByUs(keys))
                    ours.append({path, info.size(), info.lastModified()});
                continue;
            }
            if(QFile::remove(path)) {
                if(reason == DropReason::Orphan)
                    report.orphanDropped++;
                else
                    report.idleDropped++;
                totalBytes -= info.size();
                continue;
            }
            // removal failed (read-only, busy): it still occupies space, and an
            // entry we could not delete stays a candidate for the fuse
            if(writtenByUs(keys))
                ours.append({path, info.size(), info.lastModified()});
        }
    }

    // The fuse runs only on what the idle sweep left, and only against our own
    // entries: another program's thumbnail is not this app's to sacrifice. If
    // the overage is entirely someone else's, it stays over — reported, not
    // papered over.
    report.bytesAfter = totalBytes;
    if(totalBytes > byteCap) {
        std::sort(ours.begin(), ours.end(), [](const Entry &a, const Entry &b) {
            return a.mtime < b.mtime;
        });
        for(const Entry &e : ours) {
            if(report.bytesAfter <= byteCap)
                break;
            if(QFile::remove(e.path)) {
                report.bytesAfter -= e.size;
                report.sizeEvicted++;
            }
        }
        qDebug() << "[ThumbnailCache] byte cap" << byteCap << "fired:" << report.sizeEvicted
                 << "entries evicted least-recently-used first," << report.bytesAfter << "bytes remain";
    }
    // "the cap fired", "the library is old" and "the originals are gone" are
    // three different facts and a log line has to say which
    if(report.orphanDropped)
        qDebug() << "[ThumbnailCache] purged" << report.orphanDropped << "entries whose original no longer exists";
    if(report.idleDropped)
        qDebug() << "[ThumbnailCache] purged" << report.idleDropped << "entries idle for more than" << days << "days";
    return report;
}
