#include "filelog.h"

#include <QDateTime>
#include <QDir>
#include <QLoggingCategory>
#include <cstdio>

namespace FileLog {

static QtMessageHandler gPrevHandler = nullptr;
// Meyers singleton: construction on first use instead of static-init
// order, which is what clazy non-pod-global-static flags
static SessionLog &session() {
    static SessionLog s;
    return s;
}

QString formatLine(QtMsgType type, const QString &msg) {
    const char lvl[] = "DWCFI"; // enum order: Debug Warning Critical Fatal Info
    return QString("%1 %2 %3\n")
        .arg(QDateTime::currentDateTime().toString("MM-dd hh:mm:ss.zzz"), QString(QChar(lvl[type])), msg);
}

void pruneOldSessions(const QString &dir, int keep) {
    if(keep < 1)
        return;
    // the legacy glob ages out the logs the identity migration did not carry
    const QStringList logs =
        QDir(dir).entryList(QStringList() << "impulse-*.log*" << "qimgv-*.log*", QDir::Files, QDir::Name);
    for(int i = 0; i + keep < logs.size(); ++i)
        QFile::remove(dir + "/" + logs.at(i));
}

SessionLog::SessionLog(qint64 partLimitBytes, int maxParts) : m_partLimit(partLimitBytes), m_maxParts(maxParts) {
}

bool SessionLog::open(const QString &basePath) {
    m_base = basePath;
    m_bytes = 0;
    m_part = 1;
    m_stopped = false;
    m_file.setFileName(m_base);
    if(!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
    m_file.write("=== session start ===\n");
    m_file.flush();
    return true;
}

bool SessionLog::isWriting() const {
    return m_file.isOpen() && !m_stopped;
}

int SessionLog::part() const {
    return m_part;
}

qint64 SessionLog::bytesInPart() const {
    return m_bytes;
}

QString SessionLog::lastError() const {
    return m_file.errorString();
}

void SessionLog::write(const QString &line) {
    if(m_stopped || !m_file.isOpen())
        return;
    m_bytes += m_file.write(line.toUtf8());
    m_file.flush();
    if(m_bytes >= m_partLimit) {
        if(m_part >= m_maxParts) {
            m_stopped = true; // console keeps working
            return;
        }
        ++m_part;
        m_file.close();
        m_file.setFileName(m_base + QString(".part%1").arg(m_part));
        if(!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            m_stopped = true; // rotation target unopenable: stop writing, console keeps working
        m_bytes = 0;
    }
}

static void fileLogHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
    if(gPrevHandler)
        gPrevHandler(type, ctx, msg); // console keeps working
    session().write(formatLine(type, msg));
}

bool install(const QString &dir) {
    QDir().mkpath(dir);
    pruneOldSessions(dir, 10);
    const QString base = dir + "/impulse-" + QDateTime::currentDateTime().toString("MMdd-hhmmss") + ".log";
    if(!session().open(base)) {
        fprintf(stderr, "[log] open failed: %s\n", session().lastError().toLocal8Bit().constData());
        return false;
    }
    fprintf(stderr, "[log] handler installed: %s\n", base.toLocal8Bit().constData());
    if(!qEnvironmentVariableIsEmpty("QIMGV_DEBUG"))
        // only our categories — a bare "*.debug = true" would also flood the
        // file with Qt-internal plugin scanning noise
        QLoggingCategory::setFilterRules("qimgv.perf.*.debug = true\n");
    gPrevHandler = qInstallMessageHandler(fileLogHandler);
    return true;
}

} // namespace FileLog
