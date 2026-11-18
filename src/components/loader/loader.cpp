#include "loader.h"

#include <vector>

Loader::Loader(WorkerPool &_pool) : pool(_pool) {
}

Loader::~Loader() {
    // submitted jobs are the pool's to destroy: the unstarted ones come back
    // from drain() and die here, running ones the pool destroys on the worker
    // thread when run() returns. The task table is bookkeeping — an entry for
    // a finished-but-undelivered job points at memory the pool already freed.
    pool.drain(PoolOwner::Loader); // the queued jobs die with the returned vector
    pool.cancel(PoolOwner::Loader);
    pool.waitForOwner(PoolOwner::Loader);
    tasks.clear();
}

void Loader::clearTasks() {
    clearPool();
    // A decode that already started cannot be taken back, but it can still be
    // told to stop at its entry checkpoint — and a job that stops there reports
    // nothing, so the entries it was tracked by are dropped here instead of by
    // the onLoadFinished that will not arrive. The wait is what makes this safe:
    // after it, no Loader job is left that could still be holding an entry.
    pool.cancel(PoolOwner::Loader);
    pool.waitForOwner(PoolOwner::Loader);
    tasks.clear();
}

bool Loader::isBusy() const {
    return (tasks.count() != 0);
}

bool Loader::isLoading(const QString &path) {
    return tasks.contains(path);
}

std::optional<std::shared_ptr<Image>> Loader::load(const QString &path, const LoadPolicy &policy) {
    // sync path (editing fallback): always full quality
    return ImageFactory::createImage(path, false, policy);
}

// clears all buffered tasks before loading
void Loader::loadAsyncPriority(const QString &path, const LoadPolicy &policy) {
    clearPool();
    doLoadAsync(path, 2, true, policy); // tier-1 scaled for instant display
}

void Loader::loadAsync(const QString &path, const LoadPolicy &policy) {
    doLoadAsync(path, 0, true, policy);
}

void Loader::loadAsyncFull(const QString &path, const LoadPolicy &policy) {
    doLoadAsync(path, 0, false, policy); // tier-2 full-res upgrade
}

void Loader::loadWindow(const QString &target, const QSet<QString> &window, const LoadPolicy &policy) {
    auto dropped = pool.drain(PoolOwner::Loader);
    std::unique_ptr<LoaderRunnable> promote;
    std::vector<std::unique_ptr<LoaderRunnable>> kept;
    for(auto &job : dropped) {
        auto *runnable = static_cast<LoaderRunnable *>(job.get());
        tasks.remove(runnable->loadPath());
        if(runnable->loadPath() == target)
            promote.reset(static_cast<LoaderRunnable *>(job.release()));
        else if(window.contains(runnable->loadPath()))
            kept.emplace_back(static_cast<LoaderRunnable *>(job.release()));
        // else: outside the window, dies with the vector
    }
    // the target goes last: its 2 outranks whatever the flight left queued,
    // and submission order cannot weaken that
    for(auto &job : kept)
        resubmit(std::move(job), 0);
    if(promote)
        resubmit(std::move(promote), 2);
    else
        doLoadAsync(target, 2, true, policy);
}

void Loader::resubmit(std::unique_ptr<LoaderRunnable> job, int priority) {
    const QString path = job->loadPath();
    if(tasks.contains(path))
        return;
    tasks.insert(path, job.get());
    connect(job.get(), &LoaderRunnable::finished, this, &Loader::onLoadFinished, Qt::UniqueConnection);
    pool.submit(PoolOwner::Loader, priority, std::move(job));
}

void Loader::doLoadAsync(const QString &path, int priority, bool scaled, const LoadPolicy &policy) {
    if(tasks.contains(path)) {
        return;
    }

    auto job = std::make_unique<LoaderRunnable>(path, scaled, policy);
    resubmit(std::move(job), priority);
}

void Loader::onLoadFinished(std::optional<std::shared_ptr<Image>> image, const QString &path, bool scaled) {
    // the pool destroyed the job on its worker thread once run() returned;
    // dropping the entry is bookkeeping, not ownership
    tasks.take(path);
    // this is where absence stops travelling: loadFinished() only ever fires
    // with an image, and the caller that cannot show one gets loadFailed()
    if(!image)
        emit loadFailed(path);
    else
        emit loadFinished(*image, path, scaled);
}

void Loader::clearPool() {
    // drain() is tryTake() for the whole owner: everything still queued comes
    // back and dies with the vector; a job already inside run() stays out
    // there and reports through the queued onLoadFinished like any other
    auto dropped = pool.drain(PoolOwner::Loader);
    // every job this pool ever received from Loader is a LoaderRunnable
    for(auto &job : dropped)
        tasks.remove(static_cast<LoaderRunnable *>(job.get())->loadPath());
}
