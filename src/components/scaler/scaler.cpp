#include "scaler.h"

/* What this should do in theory:
 * 1 request comes
 * 2 we run it
 * 3a if during scaling no new requests came, we return the result and forget about it. end.
 * 3b if some requests did come, by the end of current task we dispose of its result,
 *    start the last task that came and ignore the middle ones.
 */

Scaler::Scaler(Cache *_cache, WorkerPool &_pool, QObject *parent)
    : QObject(parent), m_pool(_pool), m_buffered(false), m_running(false), m_cache(_cache) {
    // no explicit connection type: `finished` is emitted on a pool thread and
    // received here, so it is queued by construction and the state machine runs
    // on the UI thread. DirectConnection used to run these slots on the pool
    // thread, where they wrote `running`/`buffered`/`startedRequest` and reached
    // Cache's QMap through a semaphore that nothing else in the app shared.
    connect(this, &Scaler::acceptScalingResult, this, &Scaler::slotForwardScaledResult, Qt::QueuedConnection);
}

Scaler::~Scaler() {
    // at most one job is out (queued or running): the machine never hands off a
    // second while one is in flight. Whatever is queued comes back from drain()
    // and dies with the vector; a running one the pool destroys on its worker
    // thread when run() returns. Reservations still held at this point are not
    // released — moot, since the Cache that owns them dies right after.
    m_pool.drain(PoolOwner::Scaler); // the queued jobs die with the returned vector
    m_pool.waitForOwner(PoolOwner::Scaler);
}

void Scaler::requestScaled(const ScalerRequest &req) {
    Q_ASSERT(QThread::currentThread() == thread());
    if(!m_running) {
        //////////////////////////////////
        if(!m_buffered) {
            m_bufferedRequest = req;
            m_buffered = true;
            //qDebug() << "1 requestScaled() - locking..  " <<  req.image->name();
            m_cache->reserve(req.image->filePath());
            //qDebug() << "1 requestScaled() - LOCKED!  " <<  req.image->name();
            startRequest(req);
        } else if(m_bufferedRequest.image != req.image) {
            //qDebug() << "2 requestScaled() - locking...  " <<  req.image->name();
            m_cache->reserve(req.image->filePath());
            //qDebug() << "2 requestScaled() - LOCKED!  " <<  req.image->name();
            auto tmp = m_bufferedRequest;
            m_bufferedRequest = req;
            m_buffered = true;
            if(m_startedRequest.image != tmp.image) {
                m_cache->release(tmp.image->filePath());
                //qDebug() << "2 requestScaled() - RELEASED!  " <<  tmp.image->name();
            }
        } else {
            m_bufferedRequest = req;
            m_buffered = true;
        }
        //////////////////////////////
    } else {
        if(!m_buffered) {
            if(req.image != m_startedRequest.image)
                m_cache->reserve(req.image->filePath());
            m_bufferedRequest = req;
            m_buffered = true;
        } else {
            if(req.image == m_bufferedRequest.image) {
                m_bufferedRequest = req;
                m_buffered = true;
            } else {
                if(m_bufferedRequest.image != m_startedRequest.image) {
                    //qDebug() << "4 RELEASING " << bufferedRequest.image->name();
                    m_cache->release(m_bufferedRequest.image->filePath());
                }
                if(req.image != m_startedRequest.image)
                    m_cache->reserve(req.image->filePath());
                m_bufferedRequest = req;
                m_buffered = true;
            }
        }
    }
}

void Scaler::onTaskFinish(std::shared_ptr<const QImage> scaled, const ScalerRequest &req) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_running = false;
    if(m_buffered && m_bufferedRequest.image == req.image) {
    } else {
        //qDebug() << "onTaskFinish() - 2 releasing..  " <<  req.image->name();
        m_cache->release(req.image->filePath());
        //qDebug() << "onTaskFinish() - 2 RELEASED!  " <<  name;
    }
    if(m_buffered) {
        //qDebug() << "onTaskFinish - startingBuffered: " << bufferedRequest.string;
        // the discarded result frees itself
        startRequest(m_bufferedRequest);
    } else {
        emit acceptScalingResult(scaled, req);
    }
}

void Scaler::slotForwardScaledResult(std::shared_ptr<const QImage> image, const ScalerRequest &req) {
    if(!image)
        return;
    // QImage crosses the signal boundary — implicitly shared, and the paint-
    // device conversion is the view's job, on the GUI thread
    emit scalingFinished(image, req);
}

void Scaler::startRequest(const ScalerRequest &req) {
    m_running = true;
    m_startedRequest = req;
    // clear the buffered flag if there were no requests after this one
    if(m_buffered && m_bufferedRequest == req)
        m_buffered = false;
    auto job = std::make_unique<ScalerRunnable>(req);
    connect(job.get(), &ScalerRunnable::finished, this, &Scaler::onTaskFinish);
    m_pool.submit(PoolOwner::Scaler, 1, std::move(job));
}
