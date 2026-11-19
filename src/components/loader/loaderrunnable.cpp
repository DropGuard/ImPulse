#include "loaderrunnable.h"

#include <QLoggingCategory>
Q_LOGGING_CATEGORY(lcLoaderPerf, "qimgv.perf.loader", QtInfoMsg)

#include <QElapsedTimer>

LoaderRunnable::LoaderRunnable(QString _path, bool _scaled, const LoadPolicy &_policy)
    : path(_path), scaled(_scaled), policy(_policy) {
}

void LoaderRunnable::run() {
    // The mark is evaluated by the pool at the pop: a job marked while queued
    // never reaches run() at all, and past the pop a decode runs to completion —
    // there is no entry checkpoint here to re-check a mark that landed mid-pop.
    // Reporting a *failure* for a file the user already left would turn "no
    // longer wanted" into "this file is broken" on screen; failures are only
    // ever real decode failures.
    QElapsedTimer t;
    t.start();
    auto image = ImageFactory::createImage(path, scaled, policy);
    // the RESULT decides the tier: only formats with native scaled decode
    // (JPEG) produce a tier-1 image; everything else decodes full here
    bool resultScaled = scaled && image.has_value() && (*image)->isScaledResult();
    qCDebug(lcLoaderPerf) << "L:" << t.elapsed() << "ms" << (resultScaled ? "[scaled]" : "[full]");
    emit finished(image, path, resultScaled);
}
