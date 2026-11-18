#pragma once

#include "components/thumbnailer/thumbnailerrunnable.h"
#include "components/cache/thumbnailcache.h"
#include "components/workerpool/workerpool.h"
#include "settings.h"

#include <QHash>
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
    // Live requests, counted: each getThumbnailAsync increments the key, each
    // report (taskEnd or abandoned) decrements it, clearTasks zeroes it. The
    // count matters because a report travels from a worker thread as a queued
    // signal: between "clearTasks marked the running job" and "the UI thread
    // processed its abandoned report" there is a window in which the entry
    // must not swallow the re-request the same wave is about to make — the
    // presenters' waves re-request the same visible files immediately after
    // clearing, and under load that window is hit for real (2026-11-19, named
    // from a core: one thumbnail per wave went missing in the strip). A plain
    // set strands the entry for exactly that window; a count lets the wave's
    // own request coexist with the doomed job, and the two reports each
    // settle their own half. The accepted cost: only a job already past its
    // entry checkpoint when the clear marks it can decode alongside its
    // re-request, and the picture is then set twice — bounded, idempotent,
    // and cheaper than a thumbnail that never arrives.
    //
    // Keyed through requestKey() and NOT by a hand-written concatenation: the
    // three places that touch the key (increment, decrement, dedup test)
    // could drift, and a decrement whose key missed would leak the count
    // permanently — the file would never be requested again.
    QHash<QString, int> submitted;
    static QString requestKey(const QString &path, int size, bool crop);

  private slots:
    void onTaskEnd(const std::shared_ptr<Thumbnail> &thumbnail, const QString &filePath, bool crop);
    void onTaskAbandoned(const QString &filePath, int size, bool crop);

  signals:
    void thumbnailReady(std::shared_ptr<Thumbnail> thumbnail, QString filePath);
};
