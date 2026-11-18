#pragma once

#include "components/thumbnailer/thumbnailerrunnable.h"
#include "components/cache/thumbnailcache.h"
#include "components/workerpool/workerpool.h"
#include "settings.h"

#include <QSet>
#include <atomic>

class Thumbnailer : public QObject {
    Q_OBJECT
  public:
    // The pool is required, not defaulted: production names the shared pool at
    // the construction site, tests name their isolated ones. A default lets
    // every call site keep the hidden choice (00cf517e's rule).
    explicit Thumbnailer(WorkerPool &_pool);
    ~Thumbnailer() override;
    static std::shared_ptr<Thumbnail> getThumbnail(QString filePath, int size);
    void clearTasks();
    void waitForDone();

  public slots:
    void getThumbnailAsync(const QString &path, int size, bool crop, bool force);
    // The store's sweep, on a pool worker. It reads every entry's PNG text
    // chunks (to tell ours from another program's), which measured 40x the cost
    // of the bare stat walk it replaced — so it cannot run on the UI thread.
    // Public so a test can run one without waiting out the startup delay.
    // `ranOn`, when given, receives the QThread identity the sweep ran on: the
    // only externally visible difference between "on a worker" and "inline",
    // and therefore the only thing a test can assert about placement.
    void scheduleSweep(int idleDays, qint64 byteCap, std::atomic<Qt::HANDLE> *ranOn = nullptr);

  private:
    WorkerPool &pool;
    ThumbnailCache *cache;
    void startThumbnailerThread(QString filePath, int size, bool crop, bool force);
    // Every request handed to the pool and not yet reported back. This is the
    // dedup the engine used to provide across both of its own states (queued
    // and in flight) — with the engine's queue gone, the pool is the only queue
    // and this is the only record of what is in it.
    //
    // Keyed through requestKey() and NOT by a hand-written concatenation: the
    // three places that used to spell "path|size|crop" themselves (insert,
    // remove, and the dedup test) could drift, and a removal whose key missed
    // would leak an entry permanently — the file would never be requested
    // again. It also subsumes the runningTasks map this replaced: a job is
    // submitted before it starts, so "in flight" is a subset of "in the set".
    QSet<QString> submitted;
    static QString requestKey(const QString &path, int size, bool crop);

  private slots:
    void onTaskEnd(const std::shared_ptr<Thumbnail> &thumbnail, const QString &filePath, bool crop);
    void onTaskAbandoned(const QString &filePath, int size, bool crop);

  signals:
    void thumbnailReady(std::shared_ptr<Thumbnail> thumbnail, QString filePath);
};
