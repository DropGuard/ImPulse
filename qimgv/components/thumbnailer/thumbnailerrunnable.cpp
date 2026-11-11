#include "thumbnailerrunnable.h"

#include "qimgv_engine.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFileInfo>
#include <utility>

namespace {

// Buckets are square boxes; a thumbnail never exceeds its box on either edge.
QSize boundedBox(const QSize &natural, int box) {
    if(natural.width() <= box && natural.height() <= box)
        return natural; // never upscale: the spec's sizes are maxima
    return natural.scaled(box, box, Qt::KeepAspectRatio);
}

} // namespace

ThumbnailerRunnable::ThumbnailerRunnable(ThumbnailCache *_cache, QString _path, int _size, bool _crop, bool _force,
                                         const LoadPolicy &_policy)
    : path(std::move(_path)), size(_size), crop(_crop), force(_force), policy(_policy), cache(_cache) {
}

void ThumbnailerRunnable::run() {
    // Before taskStart there is nothing to settle: runningTasks, dispatched and
    // the engine's in-flight count all key off the reports, so a job that stops
    // here makes none of them and leaves no entry behind. After taskStart the
    // rules reverse — the job has announced itself, so it reports taskEnd like
    // any other, and the only thing cancellation can still save is the work.
    if(cancelRequested())
        return;
    emit taskStart(path, size);
    std::shared_ptr<Thumbnail> thumbnail = generate(cache, path, size, crop, force, policy, &cancelToken());
    emit taskEnd(thumbnail, path, crop);
}

QString ThumbnailerRunnable::videoFrameTempPath(const QString &tmpDir, const QString &path, int size, bool crop) {
    // Two clips named clip.mp4 in different folders used to be handed the same
    // temp file: one worker read (and then removed) the other's frame and cached
    // it under its own correct md5(path+size) id, which makes the wrong
    // thumbnail permanent until the cache is cleared. The basename stays in the
    // name only to keep a crashed run's leftovers recognisable. Size, crop and
    // pid are what keeps two concurrent extractions of the SAME clip from
    // sharing one file: the grid and the strip each run their own thumbnailer,
    // and so do two app instances.
    return tmpDir + QFileInfo(path).fileName() + "-" + QString::number(QCoreApplication::applicationPid()) + "-" +
           tempIdFor(path, size, crop) + ".png";
}

QString ThumbnailerRunnable::tempIdFor(const QString &path, int size, bool crop) {
    QString queryStr = path + QString::number(size);
    if(crop)
        queryStr.append("s");
    queryStr = QString("%1").arg(QString(QCryptographicHash::hash(queryStr.toUtf8(), QCryptographicHash::Md5).toHex()));
    return queryStr;
}

std::unique_ptr<QImage> ThumbnailerRunnable::fitForDisplay(const QImage &source, int size, bool crop) {
    if(source.isNull())
        return std::make_unique<QImage>();
    // Upscaling is only for a source smaller than the box: a bucket image is
    // exactly this size already, and a small original must not lose its
    // picture. KeepAspectRatio so nothing is stretched.
    if(!crop)
        return std::make_unique<QImage>(source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    // Cropped display covers `size` x `size`: expand, then take the centered
    // square. This is the thumbnail strip's `squareThumbnails` mode (the grid
    // never crops) and it happens here — after the read — so the shared store
    // never holds a cropped picture.
    const QImage scaled = source.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QRect clip((scaled.width() - size) / 2, (scaled.height() - size) / 2, size, size);
    return std::make_unique<QImage>(scaled.copy(clip));
}

std::shared_ptr<Thumbnail> ThumbnailerRunnable::generate(ThumbnailCache *cache, const QString &path, int size,
                                                         bool crop, bool force, const LoadPolicy &policy,
                                                         const CancelToken *cancel) {
    // Fast path: ONE file open (header probe) for format/animation/orientation.
    // DocumentInfo (mime sniff + per-format probes, 3-5 opens) is only used for
    // videos and formats the header parser doesn't know.
    QFileInfo fileInfo(path);
    std::unique_ptr<DocumentInfo> imgInfo;
    DocumentType type;
    QString format;
    int orientation = 1;
    QSize probeSize;
    qimgv_probe_result pr{};
    if(qimgv_engine_probe(fileInfo.absoluteFilePath().toUtf8().constData(), &pr) == QIMGV_OK &&
       pr.format != QIMGV_FMT_UNKNOWN) {
        switch(pr.format) {
        case QIMGV_FMT_JPEG: format = "jpg"; break;
        case QIMGV_FMT_PNG: format = "png"; break;
        case QIMGV_FMT_GIF: format = "gif"; break;
        case QIMGV_FMT_WEBP: format = "webp"; break;
        case QIMGV_FMT_AVIF: format = "avif"; break;
        case QIMGV_FMT_JXL: format = "jxl"; break;
        case QIMGV_FMT_BMP: format = "bmp"; break;
        case QIMGV_FMT_TIFF: format = "tiff"; break;
        default: break;
        }
        type = pr.animated ? DocumentType::ANIMATED : DocumentType::STATIC;
        orientation = (pr.orientation >= 1 && pr.orientation <= 8) ? pr.orientation : 1;
        if(pr.width > 0 && pr.height > 0)
            probeSize = QSize(pr.width, pr.height);
    } else {
        imgInfo = std::make_unique<DocumentInfo>(path, policy);
        type = imgInfo->type();
        format = imgInfo->format();
        orientation = imgInfo->exifOrientation();
    }

    // Identity is the spec's: the bucket is chosen from the request, and the
    // stored file is keyed by the source URI alone. A zoom step therefore
    // reuses a stored bucket instead of writing a new file per pixel size.
    const int bucket = ThumbnailCache::bucketForRequest(size, crop, probeSize);

    QImage stored;      // the aspect-preserved picture (bucket-sized)
    QSize originalSize; // the source's own dimensions, for the label and the spec keys
    bool animated = (type == ANIMATED);
    bool video = (type == VIDEO);

    if(!force && cache) {
        // The spec's rule reads one way (smaller requests read the small bucket
        // and downscale) and the useful reading of it is symmetric: any bucket
        // at least as large as the request can answer it. So the search starts
        // at the requested bucket and stops at the first hit — a thumbnail
        // written at 256 for the grid serves a 128 request from the panel
        // without a decode, and a 128 one still cannot serve a 256 request.
        for(int i = 0; i < ThumbnailCache::bucketCount; ++i) {
            if(ThumbnailCache::bucketSizes[i] < bucket)
                continue;
            if(auto hit = cache->read(path, ThumbnailCache::bucketSizes[i], fileInfo)) {
                stored = std::move(hit->image);
                originalSize = hit->originalSize;
                // The spec keys make the cached animation/video classification
                // authoritative: it is what this app recorded when it decoded.
                animated = hit->animated;
                video = hit->video;
                break;
            }
        }
    }

    if(stored.isNull()) {
        if(type == DocumentType::NONE) {
            return std::make_shared<Thumbnail>(fileInfo.fileName(), "", size, nullptr);
        }
        // Decode straight into the bucket box, aspect preserved. Sizes are the
        // spec's maxima, so an image already inside the box is not enlarged.
        std::pair<std::unique_ptr<QImage>, QSize> pair;
        if(type == VIDEO)
            pair = createVideoThumbnail(path, bucket, policy.tmpDir, policy.mpvBinary, cancel);
        else
            pair = createThumbnail(fileInfo.absoluteFilePath(), format.toStdString().c_str(), bucket, cancel);
        if(cancel && cancel->requested()) {
            // the contract the accounting pins: a canceled decode still
            // arrives as a well-formed, empty, error-labelled attempt
            auto empty = std::make_shared<Thumbnail>(fileInfo.fileName(), "error", size, nullptr);
            return empty;
        }
        originalSize = pair.second;
        const std::unique_ptr<QImage> rotated = ImageLib::exifRotated(std::move(pair.first), orientation);
        if(!rotated || rotated->isNull())
            return std::make_shared<Thumbnail>(fileInfo.fileName(), "error", size, nullptr);
        stored = *rotated;

        if(cache) {
            // Save only when the source is larger than the box — a thumbnail
            // of a thumbnail buys nothing (the spec says the same).
            if(originalSize.width() > bucket || originalSize.height() > bucket)
                cache->save(stored, path, bucket, fileInfo, originalSize, animated, video);
        }
    }

    // Display presentation is applied after the read, never stored.
    std::unique_ptr<QImage> image = fitForDisplay(stored, size, crop);

    auto &&tmpPixmap = new QPixmap(image->size());
    *tmpPixmap = QPixmap::fromImage(*image);
    tmpPixmap->setDevicePixelRatio(qGuiApp->devicePixelRatio());

    // The label reports the SOURCE's dimensions, and on a cache hit nothing in
    // the store reliably knows them: Thumb::Image::Width/Height are optional in
    // the spec and KIO does not write them (measured: 0 of 679 entries in a live
    // store). The probe ran before the cache lookup and already answered — it
    // is a header read this path needs anyway for format and orientation — so a
    // hit uses it rather than reporting 0x0. The stored keys still win when
    // present, because they describe the decode that produced the pixels.
    if(!originalSize.isValid() && probeSize.isValid())
        originalSize = probeSize;

    QString label;
    if(tmpPixmap->width() == 0) {
        label = "error";
    } else {
        // A size nobody can establish is left out rather than invented: "0x0"
        // is a number, and a viewer has no way to tell it from a real one.
        if(originalSize.isValid())
            label = QString::number(originalSize.width()) + "x" + QString::number(originalSize.height());
        if(animated)
            label.append(" [a]");
        else if(video)
            label.append(" [v]");
    }
    std::shared_ptr<QPixmap> pixmapPtr(tmpPixmap);
    return std::make_shared<Thumbnail>(fileInfo.fileName(), label, size, pixmapPtr);
}

ThumbnailerRunnable::~ThumbnailerRunnable() {
}

std::pair<std::unique_ptr<QImage>, QSize> ThumbnailerRunnable::createThumbnail(const QString &path, const char *format,
                                                                               int box, const CancelToken *cancel) {
    // The decode itself is one call inside QImageReader, so the checkpoint can
    // only sit in front of it. Empty means "no picture", not "no image": the
    // caller writes info into it and builds the error thumbnail from it, and a
    // null here would be dereferenced two statements later.
    if(cancel && cancel->requested())
        return std::make_pair(std::make_unique<QImage>(), QSize());
    QImageReader *reader = new QImageReader(path, format);
    std::unique_ptr<QImage> result;
    QSize originalSize;
    bool indexed = (reader->imageFormat() == QImage::Format_Indexed8);
    bool manualResize = indexed || !reader->supportsOption(QImageIOHandler::Size);
    if(!manualResize) { // resize during read via QImageReader (faster)
        const QSize natural = reader->size();
        originalSize = natural;
        reader->setScaledSize(boundedBox(natural, box));
        result = std::make_unique<QImage>();
        if(!reader->read(result.get())) {
            // If read() returns false there's no guarantee that size conversion worked properly.
            // So we fallback to manual.
            // Se far I've seen this happen only on some weird (corrupted?) jpeg saved from camera
            manualResize = true;
            result.reset();
            // Force reset reader because it is really finicky
            // and can fail on the second read attempt (yeah wtf)
            reader->setFileName("");
            delete reader;
            reader = new QImageReader(path, format);
        }
    }
    if(manualResize) { // manual resize. slower but should just work
        std::unique_ptr<QImage> fullSize = std::make_unique<QImage>();
        reader->read(fullSize.get());
        if(indexed) {
            auto newFmt = QImage::Format_RGB32;
            if(fullSize->hasAlphaChannel())
                newFmt = QImage::Format_ARGB32;
            fullSize = std::make_unique<QImage>(fullSize->convertToFormat(newFmt));
        }
        originalSize = fullSize->size();
        result = std::make_unique<QImage>(
            fullSize->scaled(boundedBox(fullSize->size(), box), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    // force reader to close file so it can be deleted later
    reader->setFileName("");
    delete reader;
    return std::make_pair(std::move(result), originalSize);
}

void ThumbnailerRunnable::waitForProcess(QProcess &process, const CancelToken *cancel, int timeoutMs) {
    if(!cancel) {
        process.waitForFinished(timeoutMs);
        return;
    }
    // 200 ms is the latency a canceled job pays before its worker is free again,
    // against the 8000 ms the wait is allowed to last: polling the flag is what
    // turns a blocked worker into a free one, and the slice only decides how fast.
    QDeadlineTimer deadline(timeoutMs);
    constexpr int slice = 200;
    while(!process.waitForFinished(slice)) {
        if(cancel->requested()) {
            // Killing is the point, and the reap after it is what makes the kill
            // final: after this returns the process is gone either way.
            process.kill();
            process.waitForFinished(slice);
            return;
        }
        // NotRunning covers both a failure to start and a finish that raced the
        // wait; either way there is nothing left to wait for.
        if(process.state() == QProcess::NotRunning || deadline.hasExpired())
            return;
    }
}

std::pair<std::unique_ptr<QImage>, QSize> ThumbnailerRunnable::createVideoThumbnail(const QString &path, int box,
                                                                                    const QString &tmpDir,
                                                                                    const QString &mpvBinary,
                                                                                    const CancelToken *cancel) {
    QImageReader reader;
    const QString tmpFilePath = videoFrameTempPath(tmpDir, path, box, false);
    QString tmpFilePathEsc = tmpFilePath;
    tmpFilePathEsc.replace("%", "%%");
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(mpvBinary, QStringList() << "--start=30%"
                                           << "--frames=1"
                                           << "--aid=no"
                                           << "--sid=no"
                                           << "--no-config"
                                           << "--load-scripts=no"
                                           << "--no-terminal"
                                           << "--o=" + tmpFilePathEsc << path);
    waitForProcess(process, cancel, 8000);
    process.close();

    reader.setFileName(tmpFilePath);
    reader.setFormat("png");
    std::unique_ptr<QImage> result;

    // mpv wrote the frame at its native size; bound it to the bucket box.
    // Aspect is preserved, so nothing here needs a crop.
    const QSize natural = reader.size();
    reader.setScaledSize(boundedBox(natural, box));
    QSize originalSize = natural;
    result = std::make_unique<QImage>(reader.read());

    // force reader to close file so it can be deleted later
    reader.setFileName("");

    // remove temporary file
    QFile tmpFile(tmpFilePath);
    tmpFile.remove();

    return std::make_pair(std::move(result), originalSize);
}
