#include "loader.h"

#include <vector>

Loader::Loader(WorkerPool &_pool) : m_pool(_pool) {
}

Loader::~Loader() {
    // submitted jobs are the pool's to destroy: the unstarted ones come back
    // from drain() and die here, running ones the pool destroys on the worker
    // thread when run() returns
    m_pool.drain(PoolOwner::Loader); // the queued jobs die with the returned vector
    m_pool.cancel(PoolOwner::Loader);
    m_pool.waitForOwner(PoolOwner::Loader);
}

void Loader::clearTasks() {
    clearPool();
    // A decode that already started cannot be taken back, but it can still be
    // told to stop at its entry checkpoint — and a job that stops there reports
    // nothing. The wait is what makes this safe: after it, no Loader job is
    // left that could still be in flight.
    m_pool.cancel(PoolOwner::Loader);
    m_pool.waitForOwner(PoolOwner::Loader);
}

bool Loader::isBusy() const {
    return m_pool.hasQueuedOrRunning<LoaderRunnable>(PoolOwner::Loader, [](const LoaderRunnable &) {
        return true;
    });
}

bool Loader::isLoading(const QString &path) const {
    return m_pool.hasQueuedOrRunning<LoaderRunnable>(PoolOwner::Loader, [&path](const LoaderRunnable &job) {
        return job.loadPath() == path;
    });
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
    auto dropped = m_pool.drain(PoolOwner::Loader);
    std::unique_ptr<LoaderRunnable> promote;
    std::vector<std::unique_ptr<LoaderRunnable>> kept;
    for(auto &job : dropped) {
        auto *runnable = static_cast<LoaderRunnable *>(job.get());
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
    if(isLoading(job->loadPath()))
        return;
    connect(job.get(), &LoaderRunnable::finished, this, &Loader::onLoadFinished, Qt::UniqueConnection);
    m_pool.submit(PoolOwner::Loader, priority, std::move(job));
}

void Loader::doLoadAsync(const QString &path, int priority, bool scaled, const LoadPolicy &policy) {
    if(isLoading(path)) {
        return;
    }

    auto job = std::make_unique<LoaderRunnable>(path, scaled, policy);
    resubmit(std::move(job), priority);
}

void Loader::onLoadFinished(std::optional<std::shared_ptr<Image>> image, const QString &path, bool scaled) {
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
    m_pool.drain(PoolOwner::Loader);
}
