#include "scalerrunnable.h"

#include "utils/imagelib.h"

#include <utility>

ScalerRunnable::ScalerRunnable(ScalerRequest r) : m_req(std::move(r)) {
}

void ScalerRunnable::run() {
    //QElapsedTimer t;
    //t.start();
    std::shared_ptr<const QImage> scaled;
    if(m_req.filter == 0 || (m_req.size.width() > m_req.image->width() && !m_req.smoothUpscaling)) {
        scaled =
            std::shared_ptr<const QImage>(ImageLib::scaled(m_req.image->getImage(), m_req.size, QI_FILTER_NEAREST));
    } else {
        scaled = std::shared_ptr<const QImage>(ImageLib::scaled(m_req.image->getImage(), m_req.size, m_req.filter));
    }
    //qDebug() << ">> " << req.size << ": " << t.elapsed();
    emit finished(scaled, m_req);
}
