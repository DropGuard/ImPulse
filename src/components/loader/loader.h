#pragma once

#include <optional>
#include <QSet>
#include "components/cache/thumbnailcache.h"
#include "components/workerpool/workerpool.h"
#include "loaderrunnable.h"

class Loader : public QObject {
    Q_OBJECT
  public:
    // the pool is injected, not owned: Core's single WorkerPool outlives every
    // component that runs on it
    explicit Loader(WorkerPool &_pool);
    ~Loader() override;
    // nullopt when the file cannot be opened; the async path reports the same
    // case as loadFailed(), so loadFinished() never carries an absent image
    std::optional<std::shared_ptr<Image>> load(const QString &path, const LoadPolicy &policy);
    // tier-1: decode at screen resolution for instant display
    void loadAsyncPriority(const QString &path, const LoadPolicy &policy);
    void loadAsync(const QString &path, const LoadPolicy &policy);
    // tier-2: full-resolution re-decode (upgrade)
    void loadAsyncFull(const QString &path, const LoadPolicy &policy);
    // The flight window over the Loader's queue. `target` is the file on
    // screen: its queued job (if any) is re-submitted at priority 2 so it
    // never rides behind stale work, jobs whose path is in `window` are
    // re-submitted as they were (tier included), and everything else queued
    // dies — a queued job has not started, so dropping it costs nothing that
    // was decoded. Jobs already inside run() are not drained and keep their
    // place in the pool's records: a decode has no seam past its entry
    // checkpoint, and the one running for `target` is the wanted tier anyway.
    void loadWindow(const QString &target, const QSet<QString> &window, const LoadPolicy &policy);

    void clearTasks();
    bool isBusy() const;
    bool isLoading(const QString &path) const;

  private:
    WorkerPool &m_pool;
    void clearPool();
    // the one submission seam: dedup against the pool's own records (queued
    // and running — the pool tracks what a side index could only chase),
    // connect, submit. Both the fresh path (a new job) and the window path (a
    // drained job kept alive) go through it, so the two cannot grow different
    // bookkeeping.
    void resubmit(std::unique_ptr<LoaderRunnable> job, int priority);
    void doLoadAsync(const QString &path, int priority, bool scaled, const LoadPolicy &policy);

  signals:
    void loadFinished(std::shared_ptr<Image>, const QString &path, bool scaled);
    void loadFailed(const QString &path);

  private slots:
    void onLoadFinished(std::optional<std::shared_ptr<Image>>, const QString &, bool scaled);
};
