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
        : m_cache(_cache), m_idleDays(_idleDays), m_byteCap(_byteCap), m_ranOn(_ranOn) {
    }
    void run() override {
        if(m_ranOn)
            m_ranOn->store(QThread::currentThreadId(), std::memory_order_release);
        m_cache->purgeOlderThan(m_idleDays, m_byteCap);
    }

  private:
    ThumbnailCache *m_cache;
    int m_idleDays;
    qint64 m_byteCap;
    std::atomic<Qt::HANDLE> *m_ranOn;
};

} // namespace

Thumbnailer::Thumbnailer(WorkerPool &_pool) : m_pool(_pool) {
    // the shared freedesktop store, not an app-private directory: this is the
    // whole point of the format — a thumbnail Dolphin already made is a hit
    m_cache = new ThumbnailCache(ThumbnailCache::rootDir());
    // startup sweep, after the first frames have settled. On a pool worker, not
    // the UI thread: telling our entries from another program's means reading
    // each PNG's text chunks, measured at ~40x the cost of the stat walk.
    QTimer::singleShot(3000, this, [this]() {
        scheduleSweep(ThumbnailCache::cacheMaxIdleDays, ThumbnailCache::cacheMaxBytes);
    });
}

void Thumbnailer::scheduleSweep(int idleDays, qint64 byteCap, std::atomic<Qt::HANDLE> *ranOn) {
    m_pool.submit(PoolOwner::Maintenance, 0, std::make_unique<CacheSweepJob>(m_cache, idleDays, byteCap, ranOn));
}

Thumbnailer::~Thumbnailer() {
    m_pool.drain(PoolOwner::Thumbnailer); // the queued jobs die with the returned vector
    // A frame extraction in progress would otherwise be waited out for its whole
    // 8 s timeout while the app sits in a destructor. Marking it frees the worker
    // within a poll slice, and no picture is delivered to a view that is going
    // away anyway — there is no report to settle, because the pool's own
    // queued/running record is the only accounting.
    m_pool.cancel(PoolOwner::Thumbnailer);
    m_pool.waitForOwner(PoolOwner::Thumbnailer);
    // A sweep still running holds `cache`, which is deleted below. It is the one
    // job here that must be WAITED for rather than cancelled: cancel() only
    // marks, and a marked sweep would keep walking the store and then touch a
    // destroyed object. Draining first is not enough either — drain() returns
    // queued jobs, but a running one is already past that point.
    m_pool.drain(PoolOwner::Maintenance);
    m_pool.waitForOwner(PoolOwner::Maintenance);
    // a parentless QObject: nothing else in the graph owns it, and the jobs
    // that were handed its pointer are gone with the drain and the wait above
    delete m_cache;
}

void Thumbnailer::clearTasks() {
    // Invalidate the batch: queued jobs are drained (destroyed without ever
    // running) and running ones are marked — those past their pop decode to
    // completion and deliver, and any not yet popped is discarded at the pop
    // checkpoint instead of decoding. Nothing else is bookkeeping —
    // the pool is the only record of what is pending, so there is no index
    // here that a later report could corrupt.
    m_pool.drain(PoolOwner::Thumbnailer); // the queued jobs die with the returned vector
    m_pool.cancel(PoolOwner::Thumbnailer);
}

void Thumbnailer::getThumbnailAsync(const QString &path, int size, bool crop, bool force) {
    // Dedup asked of the pool, not of an index beside it: the pool already
    // tracks both states a request can be in (queued, inside run()), and a
    // caller-side copy has to be corrected on every exit — completion, drain,
    // cancel, and a report arriving after the next wave already re-requested.
    // Each missed exit was a duplicate decode or a file that could never be
    // requested again (2026-11-19).
    if(m_pool.hasQueuedOrRunning<ThumbnailerRunnable>(PoolOwner::Thumbnailer, [&](const ThumbnailerRunnable &job) {
           return job.requestedPath() == path && job.requestedSize() == size && job.requestedCrop() == crop;
       }))
        return;
    startThumbnailerThread(path, size, crop, force);
}

void Thumbnailer::startThumbnailerThread(QString filePath, int size, bool crop, bool force) {
    auto job =
        std::make_unique<ThumbnailerRunnable>(settings->useThumbnailCache() ? m_cache : nullptr, std::move(filePath),
                                              size, crop, force, LoadPolicy::fromSettings());
    connect(job.get(), &ThumbnailerRunnable::taskEnd, this, &Thumbnailer::onTaskEnd);
    m_pool.submit(PoolOwner::Thumbnailer, 0, std::move(job));
}

void Thumbnailer::onTaskEnd(const std::shared_ptr<Thumbnail> &thumbnail, const QString &filePath) {
    emit thumbnailReady(thumbnail, filePath);
}
