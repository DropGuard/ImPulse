// The flip-flight instrument: not a gate, an instrument. Drives real
// navigation (ActionManager triggers, real decodes, the shared pool) over a
// large-image directory at keyboard-repeat rate and prints what the preload
// design decisions wait on: per-flip display latency, the stop-point latency
// of the last flip of a flight, and where each delivery landed relative to
// the view — behind the current file (a zombie decode, waste), ahead of it
// (the hedge, intended), or on it (on time). It asserts nothing and is not
// registered with ctest.
//
// Run: ./build/qimgv/tests/flipbench
// With loader decode times: QT_LOGGING_RULES="qimgv.perf.loader=true" ./build/...
// The candidates this feeds live in notes/hardening.md (velocity window,
// decode seams); numbers first, mechanisms second (discipline 6).

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QApplication>
#include <QHash>
#include <QImage>
#include <QThread>
#include <algorithm>
#include <QRandomGenerator>

#include "core.h"
#include "settings.h"
#include "components/actionmanager/actionmanager.h"
#include "components/directorymanager/enginebridge.h"
#include "components/directorymodel.h"
#include "core_test_helpers.h"
#include "testhelpers.h"

namespace {

// processEvents until the condition holds or the deadline passes; the
// instrument's only wait primitive. Bounded, and never passes early.
template <typename Cond> bool spinUntil(const Cond &cond, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while(!cond()) {
        if(t.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QThread::msleep(2);
    }
    return true;
}

bool writeBig(const QString &path, const QSize &size, const char *format) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(QColor(30 + QRandomGenerator::global()->bounded(200), 30 + QRandomGenerator::global()->bounded(200),
                    30 + QRandomGenerator::global()->bounded(200)));
    return img.save(path, format);
}

struct Arrival {
    QString path;
    qint64 at; // round-clock ms at delivery
};

struct FlightReport {
    QList<int> flipLatency;  // ms, in flip order; -1 = never delivered
    qint64 stopLatency = -1; // last flip's first delivery
    bool stopLanded = false;
    int behind = 0, ahead = 0, onTime = 0;
};

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    IsolatedHome home;
    coretest::AppSingletons singletons;

    // One directory, JPEG-major with PNG stragglers spread through it: the
    // JPEGs ride the scaled tier, the PNGs decode full and are the ones whose
    // zombies would matter.
    const QString dir = QDir(home.path()).filePath("flight");
    QDir().mkpath(dir);
    // argv, all optional: <files> <jpeg-width x jpeg-height> <png-width x png-height> <intervalMs> <flips>
    // Defaults model a photo library; the heavy scenario that stresses the
    // window is e.g. ./flipbench 26 6000 4000 5000 4000 25 15
    const int fileCount = argc > 1 ? atoi(argv[1]) : 26;
    const QSize jpgSize = argc > 3 ? QSize(atoi(argv[2]), atoi(argv[3])) : QSize(3000, 2000);
    const QSize pngSize = argc > 5 ? QSize(atoi(argv[4]), atoi(argv[5])) : QSize(3000, 2000);
    const int interval = argc > 6 ? atoi(argv[6]) : 60;
    const int flipsPerRound = argc > 7 ? atoi(argv[7]) : 15;
    QStringList files;
    QElapsedTimer gen;
    gen.start();
    for(int i = 0; i < fileCount; ++i) {
        const bool png = (i % 4 == 3);
        const QString path =
            QDir(dir).filePath(QStringLiteral("f%1.%2").arg(i, 2, 10, QLatin1Char('0')).arg(png ? "png" : "jpg"));
        if(!writeBig(path, png ? pngSize : jpgSize, png ? "PNG" : "JPG"))
            qFatal("fixture write failed: %s", qPrintable(path));
        files << path;
    }
    qDebug() << "[flipbench] fixture:" << fileCount << "files jpeg" << jpgSize << "png" << pngSize << ", generated in"
             << gen.elapsed() << "ms";

    EngineBridge bridge;
    Core core({.bridge = &bridge});

    // the delivery ledger, reset per flight
    QElapsedTimer roundClock;
    QHash<QString, qint64> flipAt; // path -> round-clock ms of its trigger
    QList<Arrival> arrivals;       // every imageReady, in arrival order
    int behind = 0, ahead = 0, onTime = 0;
    QObject::connect(core.directoryModel().get(), &DirectoryModel::imageReady, &app,
                     [&](std::shared_ptr<Image>, const QString &path, bool) {
                         arrivals.append(Arrival{path, roundClock.elapsed()});
                         // classified at DELIVERY time against the then-current
                         // file: report-time classification would call the whole
                         // flight "behind" by construction
                         const int cur = core.directoryModel()->indexOfFile(core.currentFilePath());
                         const int idx = core.directoryModel()->indexOfFile(path);
                         if(idx < 0 || idx < cur)
                             behind++;
                         else if(idx > cur)
                             ahead++;
                         else
                             onTime++;
                     });

    const struct {
        const char *name;
    } rounds[] = {{"flight"}};

    for(const auto &roundSpec : rounds) {
        core.loadPath(files.first());
        if(!spinUntil(
               [&] {
                   return core.directoryModel()->fileCount() == fileCount &&
                          core.directoryModel()->isLoaded(files.first());
               },
               15000))
            qFatal("cold open never settled");

        flipAt.clear();
        arrivals.clear();

        // the flight: steady keyboard rate regardless of how deliveries lag —
        // the user keeps pressing, the flight carries what it carries
        for(int k = 0; k < flipsPerRound; ++k) {
            const qint64 tick = roundClock.elapsed();
            actionManager->action("nextImage")->trigger();
            flipAt.insert(core.currentFilePath(), roundClock.elapsed());
            while(roundClock.elapsed() - tick < interval) {
                QCoreApplication::processEvents(QEventLoop::AllEvents);
                QThread::msleep(2);
            }
        }

        // the stop: the last flip's file must land — the number the whole
        // design question is about
        const QString stopPath = core.currentFilePath();
        const qint64 stopAt = roundClock.elapsed();
        const bool landed = spinUntil(
            [&] {
                return core.directoryModel()->isLoaded(stopPath);
            },
            15000);
        const qint64 stopLatency = landed ? roundClock.elapsed() - stopAt : -1;

        // grace: let stragglers land, then classify the ledger
        spinUntil(
            [] {
                return false;
            },
            2000);

        FlightReport report;
        report.stopLanded = landed;
        report.stopLatency = stopLatency;
        report.behind = behind;
        report.ahead = ahead;
        report.onTime = onTime;
        QList<QString> flipped = flipAt.keys();
        std::sort(flipped.begin(), flipped.end(), [&](const QString &a, const QString &b) {
            return flipAt.value(a) < flipAt.value(b);
        });
        for(const QString &path : std::as_const(flipped)) {
            qint64 first = -1;
            for(const auto &a : arrivals)
                if(a.path == path && a.at >= flipAt.value(path)) {
                    first = a.at - flipAt.value(path);
                    break;
                }
            report.flipLatency.append(static_cast<int>(first));
        }

        QString line;
        for(const int ms : std::as_const(report.flipLatency))
            line += (ms < 0 ? QStringLiteral("n/d") : QStringLiteral("%1").arg(ms)) + QLatin1Char(' ');
        qDebug() << "[flipbench]" << roundSpec.name << "(interval" << interval << "ms) —" << flipsPerRound
                 << "flips | stop latency:"
                 << (landed ? QString::number(stopLatency) + "ms" : QStringLiteral("NOT LANDED"))
                 << "| deliveries: behind=" << behind << "ahead=" << ahead << "onTime=" << onTime;
        qDebug() << "[flipbench]   flip latencies (ms):" << line;
    }

    qDebug() << "[flipbench] done";
    return 0;
}
