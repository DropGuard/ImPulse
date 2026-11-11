#pragma once

#include <QProcess>
#include <QThread>
#include <QCryptographicHash>
#include <ctime>
#include "sourcecontainers/thumbnail.h"
#include "components/cache/thumbnailcache.h"
#include "components/workerpool/workerpool.h"
#include "utils/imagefactory.h"
#include "utils/imagelib.h"
#include "settings.h"
#include <memory>
#include <QImageWriter>

class ThumbnailerRunnable : public QObject, public PoolJob {
    Q_OBJECT
  public:
    ThumbnailerRunnable(ThumbnailCache *_cache, QString _path, int _size, bool _crop, bool _force,
                        const LoadPolicy &_policy);
    ~ThumbnailerRunnable() override;
    void run() override;
    static std::shared_ptr<Thumbnail> generate(ThumbnailCache *cache, const QString &path, int size, bool crop,
                                               bool force, const LoadPolicy &policy,
                                               const CancelToken *cancel = nullptr);
    // The frame a video decode writes out lands in a shared directory, so its
    // name must be unique per SOURCE PATH, SIZE and CROP — two presenters (or
    // two processes) extracting the same clip concurrently used to hand two
    // mpv writers one file, and the reader that lost the race cached a torn
    // frame under its own correct id. The pid keeps the cross-process face of
    // that race out too; the file is deleted once read, so nothing accumulates.
    // Public static so a test can pin the seam, the way
    // EngineBridge::extensionListFor does.
    static QString videoFrameTempPath(const QString &tmpDir, const QString &path, int size, bool crop);
    // Waits for the frame extractor the way a cancellable job has to: in slices,
    // so a job that is no longer wanted can stop a wait that would otherwise hold
    // a worker for the whole timeout. A null cancel waits in one call, which is
    // what the synchronous Thumbnailer::getThumbnail does. Public static for the
    // same reason as above — a test pins it against a real process.
    static void waitForProcess(QProcess &process, const CancelToken *cancel, int timeoutMs);

  private:
    static QString tempIdFor(const QString &path, int size, bool crop);
    // Decodes at `box` (a square the result must fit inside, never upscaled
    // past the source) WITHOUT cropping: a cropped image is not shareable, so
    // the bucket store only ever holds aspect-preserved pictures.
    static std::pair<std::unique_ptr<QImage>, QSize> createThumbnail(const QString &path, const char *format, int box,
                                                                     const CancelToken *cancel);
    static std::pair<std::unique_ptr<QImage>, QSize> createVideoThumbnail(const QString &path, int box,
                                                                          const QString &tmpDir,
                                                                          const QString &mpvBinary,
                                                                          const CancelToken *cancel);
    // The display half: takes the stored (or freshly decoded) aspect-preserved
    // image and produces what the widget wants — scaled to `size`, or cropped
    // to a centered `size` square when the thumbnail strip asks for square
    // previews (`squareThumbnails`); the folder grid never asks.
    // This is the only place a crop happens.
    static std::unique_ptr<QImage> fitForDisplay(const QImage &source, int size, bool crop);
    QString path;
    int size;
    bool crop, force;
    // the environment, carried from the dispatch site: the pool thread never
    // reads the settings global
    LoadPolicy policy;
    ThumbnailCache *cache = nullptr;

  signals:
    void taskStart(QString, int);
    void taskEnd(std::shared_ptr<Thumbnail>, QString, bool crop);
};
