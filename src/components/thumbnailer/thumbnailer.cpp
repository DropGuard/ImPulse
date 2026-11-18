#include "thumbnailer.h"

#include <utility>

#include <QThread>
#include <QTimer>

namespace {

// The sweep as a pool job. It owns nothing but the arguments: the store object
// lives on Thumbnailer, which waits for this job before destroying it (see the
// destructor), so the pointer is valid for the whole of run().
class CacheSweepJob : public PoolJob {
  public:
    CacheSweepJob(ThumbnailCache *_cache, int _idleDays, qint64 _byteCap, std::atomic<Qt::HANDLE> *_ranOn)
        : cache(_cache), idleDays(_idleDays), byteCap(_byteCap), ranOn(_ranOn) {
    }
    void run() override {
        if(ranOn)
            ranOn->store(QThread::currentThreadId(), std::memory_order_release);
        cache->purgeOlderThan(idleDays, byteCap);
    }

  private:
    ThumbnailCache *cache;
    int idleDays;
    qint64 byteCap;
    std::atomic<Qt::HANDLE> *ranOn;
};

} // namespace

Thumbnailer::Thumbnailer(WorkerPool &_pool) : pool(_pool) {
    // the shared freedesktop store, not an app-private directory: this is the
    // whole point of the format — a thumbnail Dolphin already made is a hit
    cache = new ThumbnailCache(ThumbnailCache::rootDir());
    // startup sweep, after the first frames have settled. On a pool worker, not
    // the UI thread: telling our entries from another program's means reading
    // each PNG's text chunks, measured at ~40x the cost of the stat walk.
    QTimer::singleShot(3000, this, [this]() {
        scheduleSweep(ThumbnailCache::cacheMaxIdleDays, ThumbnailCache::cacheMaxBytes);
    });
}

void Thumbnailer::scheduleSweep(int idleDays, qint64 byteCap, std::atomic<Qt::HANDLE> *ranOn) {
    pool.submit(PoolOwner::Maintenance, 0, std::make_unique<CacheSweepJob>(cache, idleDays, byteCap, ranOn));
}

Thumbnailer::~Thumbnailer() {
    pool.drain(PoolOwner::Thumbnailer); // the queued jobs die with the returned vector
    // A frame extraction in progress would otherwise be waited out for its whole
    // 8 s timeout while the app sits in a destructor. Marking it frees the worker
    // within a poll slice; the job's abandoned report settles its submission
    // entry, and no picture is delivered to a view that is going away anyway.
    pool.cancel(PoolOwner::Thumbnailer);
    pool.waitForOwner(PoolOwner::Thumbnailer);
    // A sweep still running holds `cache`, which is deleted below. It is the one
    // job here that must be WAITED for rather than cancelled: cancel() only
    // marks, and a marked sweep would keep walking the store and then touch a
    // destroyed object. Draining first is not enough either — drain() returns
    // queued jobs, but a running one is already past that point.
    pool.drain(PoolOwner::Maintenance);
    pool.waitForOwner(PoolOwner::Maintenance);
    // a parentless QObject: nothing else in the graph owns it, and the jobs
    // that were handed its pointer are gone with the drain and the wait above
    delete cache;
}

void Thumbnailer::waitForDone() {
    pool.waitForOwner(PoolOwner::Thumbnailer);
}

void Thumbnailer::clearTasks() {
    // The batch is whatever the pool still holds. Queued jobs are drained —
    // they die without ever reporting — and running jobs are marked, so the
    // ones still before their entry checkpoint abandon (no picture) while the
    // ones past it deliver normally. Every report decrements its own count,
    // and a count whose job was drained or cleared away settles at zero
    // through the clear below.
    pool.drain(PoolOwner::Thumbnailer); // the queued jobs die with the returned vector
    pool.cancel(PoolOwner::Thumbnailer);
    // Forget the whole batch NOW, not report-by-report: the reports travel as
    // queued signals, and the caller's next line is typically a re-request of
    // the same visible files. An entry kept for a marked job would swallow
    // that re-request — the mark is only observed at the job's entry
    // checkpoint, its abandonment travels asynchronously, and under load the
    // wave's request lands inside that window (2026-11-19, named from a core:
    // a marked-before-start job's stranded entry ate the recovery wave's
    // re-request). The accepted cost: a job past its checkpoint when the clear
    // marks it may decode alongside its re-request, and the picture is set
    // twice — bounded, idempotent, and cheaper than a thumbnail that never
    // arrives.
    submitted.clear();
}

std::shared_ptr<Thumbnail> Thumbnailer::getThumbnail(QString filePath, int size) {
    return ThumbnailerRunnable::generate(nullptr, std::move(filePath), size, false, false, LoadPolicy::fromSettings());
}

QString Thumbnailer::requestKey(const QString &path, int size, bool crop) {
    // one spelling, used by the insert, the removal and the dedup check.
    // Identity is the full triple: a path may repeat at different sizes/crops.
    return path + QLatin1Char('|') + QString::number(size) + QLatin1Char('|') +
           (crop ? QLatin1Char('1') : QLatin1Char('0'));
}

void Thumbnailer::getThumbnailAsync(const QString &path, int size, bool crop, bool force) {
    // Straight into the pool. Dedup lives here (one request per triple across
    // queued AND running); batch invalidation lives in clearTasks().
    const QString key = requestKey(path, size, crop);
    if(submitted.contains(key))
        return;
    ++submitted[key];
    startThumbnailerThread(path, size, crop, force);
}

void Thumbnailer::startThumbnailerThread(QString filePath, int size, bool crop, bool force) {
    auto job =
        std::make_unique<ThumbnailerRunnable>(settings->useThumbnailCache() ? cache : nullptr, std::move(filePath),
                                              size, crop, force, LoadPolicy::fromSettings());
    connect(job.get(), &ThumbnailerRunnable::taskEnd, this, &Thumbnailer::onTaskEnd);
    // a job marked before it started reports nothing to the view, but its
    // submission entry still has to go or the file is never requested again
    connect(job.get(), &ThumbnailerRunnable::abandoned, this, &Thumbnailer::onTaskAbandoned);
    pool.submit(PoolOwner::Thumbnailer, 0, std::move(job));
}

void Thumbnailer::onTaskAbandoned(const QString &filePath, int size, bool crop) {
    const auto it = submitted.find(requestKey(filePath, size, crop));
    if(it != submitted.end() && --it.value() <= 0)
        submitted.erase(it);
}

void Thumbnailer::onTaskEnd(const std::shared_ptr<Thumbnail> &thumbnail, const QString &filePath, bool crop) {
    const auto it = submitted.find(requestKey(filePath, thumbnail->size(), crop));
    if(it != submitted.end() && --it.value() <= 0)
        submitted.erase(it);
    emit thumbnailReady(thumbnail, filePath);
}
