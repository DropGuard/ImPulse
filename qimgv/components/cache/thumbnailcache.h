#pragma once

#include <QDateTime>
#include <QFileInfo>
#include <QImage>
#include <QSize>
#include <QString>

#include <optional>

// What a cache hit yields. The image is the stored bucket thumbnail — aspect
// preserved, never cropped — because a cropped thumbnail is not shareable: the
// freedesktop store is read by file managers that would display a square crop
// as if it were the whole picture. Cropping is a display decision and happens
// after the read.
struct CachedThumbnail {
    QImage image;
    QSize originalSize; // Thumb::Image::Width/Height; empty when the writer did not record them
    bool animated = false;
    bool video = false;
};

// The freedesktop thumbnail store, thumbnail-spec 0.8.0, shared with every other
// thumbnail-aware program on the desktop (Dolphin/KIO, GNOME Files, Tumbler).
//
//   $XDG_CACHE_HOME/thumbnails/{normal,large,x-large,xx-large}/<md5(uri)>.png
//
// Identity is the spec's, not this app's: the file name is the MD5 of the
// canonical file URI and of nothing else — no size, no crop flag, no app name.
// That is what makes a thumbnail Dolphin already made a cache hit here on first
// sight, and it is also why zooming the grid no longer writes a new file per
// pixel size: a request is answered from the smallest bucket that fits and
// scaled down. (The old scheme keyed on md5(path+size+crop), so a zoom step was
// a new key, a new decode and a new file — 2372 files where the shared store
// held 163.)
//
// Validity is the spec's too (section 7): Thumb::URI must equal the computed
// URI, Thumb::MTime must equal the source's mtime in whole seconds, and
// Thumb::Size, when present, must equal the source's size. A missing
// Thumb::MTime is a miss: a global thumbnail without it "must be recreated in
// any case". Disagreement is a miss, and the next save replaces the file —
// that is the spec's own repair path.
//
// Two consequences are deliberate and documented where they are implemented:
// sub-second mtime discrimination is gone (the spec's clock is seconds, so a
// file rewritten within one second at the same size serves the old picture),
// and the cache directory is now owned by nobody in particular, which is why
// the sweep below may only ever delete entries this app wrote.
class ThumbnailCache {
  public:
    // The spec's buckets: normal/ is capped at 128x128, large/ at 256x256,
    // x-large/ at 512x512, xx-large/ at 1024x1024. A thumbnail never exceeds
    // its bucket on either edge; consumers scale down, never up.
    static constexpr int bucketSizes[] = {128, 256, 512, 1024};
    static constexpr int bucketCount = int(sizeof(bucketSizes) / sizeof(bucketSizes[0]));

    // entries expire by IDLE time, not age: read() refreshes mtime on a hit, so
    // anything still being displayed survives while abandoned size variants
    // (and orphans of renamed/deleted sources) age out
    static constexpr int cacheMaxIdleDays = 15;
    // The byte cap is a fuse, not a policy: it is consulted only after the idle
    // sweep has already run, so a library that lives inside its idle window
    // never loses an entry to it. mtime is the eviction order because, as above,
    // mtime is the last-used clock — which makes the fuse an LRU by reuse of the
    // existing mechanism rather than a second bookkeeping system.
    static constexpr qint64 cacheMaxBytes = 500LL * 1024 * 1024;

    struct PurgeReport {
        int idleDropped = 0;   // ours, untouched for `days` or more
        int orphanDropped = 0; // ours, whose original is provably gone (spec 9)
        int sizeEvicted = 0;   // ours, sacrificed to bring the store under the cap
        qint64 bytesAfter = 0; // what the whole store holds when both passes are done
    };

    // $XDG_CACHE_HOME/thumbnails. QStandardPaths implements the spec's own rule
    // for the root ($XDG_CACHE_HOME when set and non-blank, $HOME/.cache
    // otherwise) — verified against both spellings, including a blank value.
    static QString rootDir();
    // The canonical, minimally encoded URI the spec hashes. Byte-identical to
    // what KIO stores, checked against a live 163-entry store: QUrl keeps
    // unreserved characters literal and encodes space/#/%/? the same way, so
    // there is no reason to hand-roll an encoder that would drift from it.
    static QString uriFor(const QString &localPath);
    // The bucket a fresh decode is stored at. Chosen from the request, because
    // the only thing the store knows is the URI and the box sizes are the
    // spec's: storing at the largest bucket the display can ask for would make
    // Dolphin's and the grid's reads pay for pixels neither shows.
    static int bucketForRequest(int requestedPixels, bool crop, const QSize &sourceSize);
    static QString bucketName(int bucket);

    // the directory is a parameter, not a settings lookup in the constructor:
    // a purge deletes files, so a test that measures it must own the directory
    // it points at rather than share the machine's real store with every suite
    explicit ThumbnailCache(const QString &_rootPath);

    QString thumbnailPath(const QString &sourcePath, int bucket) const;
    bool exists(const QString &sourcePath, int bucket) const;
    // std::nullopt on a miss, an unreadable source (spec 6.1: an unreadable
    // original is neither looked up nor written), or any validity disagreement
    std::optional<CachedThumbnail> read(const QString &sourcePath, int bucket, const QFileInfo &source);
    // QImage & and not const QImage &: the spec keys are written onto the image,
    // and a shared QImage would detach into a second copy of the pixels here
    bool save(QImage &image, const QString &sourcePath, int bucket, const QFileInfo &source, const QSize &originalSize,
              bool animated, bool video);
    PurgeReport purgeOlderThan(int days, qint64 byteCap);

  private:
    QString rootPath;
};
