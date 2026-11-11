#pragma once

#include "components/directorymanager/enginebridge.h"
#include "components/thumbnailer/thumbnailerrunnable.h"
#include "components/cache/thumbnailcache.h"
#include "components/workerpool/workerpool.h"
#include "settings.h"

#include <atomic>

class Thumbnailer : public QObject {
    Q_OBJECT
  public:
    // The pool and the bridge are required, not defaulted: production names
    // the shared pool and the process-wide bridge at the construction site,
    // tests name their isolated ones. A default lets every call site keep the
    // hidden choice (00cf517e's rule).
    explicit Thumbnailer(WorkerPool &_pool, EngineBridge &bridge);
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
    EngineBridge &mBridge;
    WorkerPool &pool;
    ThumbnailCache *cache;
    void startThumbnailerThread(QString filePath, int size, bool crop, bool force);
    QMultiMap<QString, int> runningTasks;
    // engine-dispatched requests (path -> {size, crop}); cleared entries are
    // reported as done so the scheduler's in-flight accounting doesn't leak
    QMultiHash<QString, QPair<int, bool>> dispatched;
    QHash<QString, bool> mPendingForce;
    bool mBatchStart = true;

  private slots:
    void onTaskStart(const QString &filePath, int size);
    void onTaskEnd(const std::shared_ptr<Thumbnail> &thumbnail, const QString &filePath, bool crop);
    void onThumbNext(const QString &filePath, int size, bool crop);

  signals:
    void thumbnailReady(std::shared_ptr<Thumbnail> thumbnail, QString filePath);
};
