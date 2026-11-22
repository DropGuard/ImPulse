#include "enginebridge.h"

#include <QDebug>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <cstring>

#include "qimgv_engine.h"
#include "settings.h"

QByteArray EngineBridge::extensionListFor(const QList<QByteArray> &formats) {
    // the '\n'-joined extension whitelist for the engine, sourced from the
    // deployed QImageReader plugins — the same list
    // Settings::supportedFormatsRegex() builds its pattern from. Empty means
    // "accept everything" (FFI contract: NULL).
    QStringList out;
    for(const auto &f : formats)
        out << QString::fromUtf8(f);
    return out.join('\n').toUtf8();
}

void EngineBridge::notifyCb(void *user) {
    // Runs on an engine thread. The queued call is the whole wake: drainEvents
    // drains until the engine is empty, so N callbacks cost N idempotent drains
    // and no event is ever lost. (The Linux tree used an eventfd and a
    // QSocketNotifier for this hop; the queued invoke is the same hop in Qt's
    // own vocabulary, which is why this file carries no platform branch.)
    //
    // LIFETIME, in one place: `user` is the bridge, and this callback may run
    // on any engine thread until qimgv_engine_free returns. That free lives in
    // ~EngineHandle, a by-value member, so it runs as part of the bridge's own
    // destruction — and it JOINS the loop thread (which joins the watcher)
    // before returning. After that join no engine thread can be inside this
    // function, and the queued metacalls already posted to the bridge were
    // cancelled with it. Holding the handle by value is what makes that
    // ordering true by construction: there is no path that destroys the
    // bridge without first stopping the threads that call back into it.
    auto *bridge = static_cast<EngineBridge *>(user);
    Q_ASSERT(bridge);
    QMetaObject::invokeMethod(
        bridge,
        [bridge] {
            bridge->drainEvents();
        },
        Qt::QueuedConnection);
}

EngineBridge &EngineBridge::instance() {
    static EngineBridge bridge;
    return bridge;
}

EngineBridge::EngineBridge(QObject *parent) : QObject(parent) {
    const QByteArray exts = settings ? extensionListFor(settings->supportedFormats()) : QByteArray();
    m_engine.adopt(qimgv_engine_new(&EngineBridge::notifyCb, this, exts.isEmpty() ? nullptr : exts.constData()));
    if(!m_engine) {
        const char *err = qimgv_last_error();
        qWarning() << "[EngineBridge] engine init failed:" << (err ? err : "unknown");
        return;
    }
    qDebug() << "[EngineBridge] ctor done";
}

EngineBridge::~EngineBridge() = default;

// The handle's destructor runs as the LAST member teardown, and qimgv_engine_free
// joins the engine's loop thread (which in turn joins the watcher). The order is
// therefore: this object stops being reachable -> engine threads joined -> queued
// metacalls already posted to `this` were cancelled by QObject's own destruction.
// Nothing can call back into a half-destroyed bridge.
// The engine's C type lives here and nowhere else in the tree — not in the
// header, not in a comment. This struct is what lets the handle be a pimpl
// without an erased pointer.
struct EngineHandle::Impl {
    qimgv_engine *engine = nullptr;
};

EngineHandle::EngineHandle() : m_d(std::make_unique<Impl>()) {
}

EngineHandle::~EngineHandle() {
    // The one free. qimgv_engine_free joins the loop thread, which joins the
    // watcher, so when this returns no engine thread can be inside notifyCb.
    if(m_d->engine)
        qimgv_engine_free(m_d->engine);
}

void EngineHandle::adopt(void *created) {
    // called once, from the constructor, while m_d->engine is still null: the
    // engine is born here and never re-adopted over a live one (the destructor
    // is the only thing that frees it, and it joins the engine's threads first)
    Q_ASSERT(!m_d->engine);
    m_d->engine = static_cast<qimgv_engine *>(created);
}

EngineHandle::operator bool() const {
    return m_d->engine != nullptr;
}

bool EngineBridge::openDir(const QString &path, int sortMode, bool showHidden) {
    if(!m_engine)
        return false;
    setShowHidden(showHidden);
    // sort rides with OpenDir (atomic in the engine) — a separate setSort
    // would emit a stale resort event for the previous directory's state
    const QByteArray utf8 = path.toUtf8();
    qDebug() << "[EngineBridge] openDir" << path;
    uint64_t epoch = 0;
    const int rc = qimgv_engine_open_dir(m_engine.m_d->engine, utf8.constData(), sortMode, &epoch);
    if(rc == QIMGV_OK) {
        m_sessionEpoch = epoch;
        m_requestedDir = path;
    }
    return rc == QIMGV_OK;
}

void EngineBridge::setSort(int sortMode) {
    if(m_engine)
        qimgv_engine_set_sort(m_engine.m_d->engine, sortMode);
}

void EngineBridge::setShowHidden(bool show) {
    if(m_engine)
        qimgv_engine_set_show_hidden(m_engine.m_d->engine, show ? 1 : 0);
}

const QString &EngineBridge::listedDirectory() const {
    return m_listedDir;
}

int EngineBridge::fileCount() const {
    if(!m_engine)
        return 0;
    const int n = qimgv_engine_file_count(m_engine.m_d->engine);
    return n < 0 ? 0 : n;
}

int EngineBridge::dirCount() const {
    if(!m_engine)
        return 0;
    const int n = qimgv_engine_dir_count(m_engine.m_d->engine);
    return n < 0 ? 0 : n;
}

int EngineBridge::compareNames(const QString &a, const QString &b, SortingMode mode) {
    const QByteArray ua = a.toUtf8();
    const QByteArray ub = b.toUtf8();
    // only the two codes this answers for; the engine treats anything else as
    // ascending name order, which is the same fallback the caller applies
    const int code = (mode == SortingMode::SORT_NAME_DESC) ? QIMGV_SORT_NAME_DESC : QIMGV_SORT_NAME;
    return qimgv_engine_compare_names(ua.constData(), ub.constData(), code);
}

bool EngineBridge::traceEnabled() {
    // Read once: the value is a debugging switch, and a getenv per event would
    // make the trace change the timing of the thing it is tracing.
    static const bool on = [] {
        const QString value = qEnvironmentVariable("QIMGV_ENGINE_TRACE");
        return !value.isEmpty() && value != QLatin1String("0");
    }();
    return on;
}

void EngineBridge::stopWatch() {
    if(m_engine)
        qimgv_engine_stop_watch(m_engine.m_d->engine);
}

bool EngineBridge::exportListing(QByteArray &out, quint32 &fileCount, quint32 &dirCount, quint64 &revision) {
    if(!m_engine)
        return false;
    uint32_t needed = 0, fc = 0, dc = 0;
    uint64_t rev = 0;
    if(qimgv_engine_export_listing(m_engine.m_d->engine, nullptr, 0, &needed, &fc, &dc, &rev) == QIMGV_OK) {
        out.clear(); // empty listing
        fileCount = fc;
        dirCount = dc;
        revision = rev;
        return true;
    }
    // Probe-then-write is two lock passes, and the listing is not frozen between
    // them: a folder being written while it is read grows the arena, the write
    // pass finds its buffer short and reports TRUNCATED with a larger `needed`,
    // and treating that as a failure would throw away a good listing. Go round
    // again on the new size, bounded so a permanently racing caller cannot spin.
    for(int attempt = 0;; ++attempt) {
        out.resize(int(needed));
        const int rc = qimgv_engine_export_listing(m_engine.m_d->engine, reinterpret_cast<unsigned char *>(out.data()),
                                                   size_t(out.size()), &needed, &fc, &dc, &rev);
        if(rc == QIMGV_OK) {
            // A successful write fills `needed` bytes and no more, and `needed`
            // may be SMALLER than the buffer this loop grew to on a previous
            // round if the listing shrank in between. Parsing the longer buffer
            // would leave trailing zeros past the last record and the exactness
            // check would report a corrupt arena, so hand back what was written.
            out.resize(int(needed));
            break;
        }
        if(rc != QIMGV_ERR_TRUNCATED || needed <= size_t(out.size()) || attempt >= 16) {
            qWarning() << "[EngineBridge] export listing failed:" << qimgv_last_error();
            return false;
        }
    }
    fileCount = fc;
    dirCount = dc;
    // `rev` came back with the pass that actually wrote these bytes: an earlier
    // round's number would name a listing the caller does not hold
    revision = rev;
    return true;
}

// Decodes the arena written by DirectoryState::write_listing (model.rs).
// Little-endian: u32 file_count, u32 dir_count, u64 session, u64 revision,
// i32 applied_sort, u32 directory_len, directory bytes, then file_count +
// dir_count records of u64 id, u64 content_revision, u64 size, i64 mtime_ms,
// u32 name_len, u32 path_len, name bytes, path bytes (no NUL) — files first.
// A record that does not fit is refused outright rather than yielding a
// short listing: a prefix that decoded cleanly is indistinguishable, at the
// call site, from a directory that genuinely has fewer entries.
//
// Returns false unless the arena held exactly file_count file records plus
// dir_count dir records and was consumed to the last byte. Both checks matter:
// the header counts are the only place where producer and consumer agree on how
// much data SHOULD be here, so a disagreeing count is drift, not an empty list.
bool EngineBridge::parseListing(const QByteArray &arena, Snapshot &out) {
    if(arena.size() < 32)
        return false;
    const auto *cursor = reinterpret_cast<const unsigned char *>(arena.constData());
    const auto *end = cursor + arena.size();
    auto read32 = [&cursor]() {
        const quint32 value = qFromLittleEndian<quint32>(cursor);
        cursor += 4;
        return value;
    };
    auto read64 = [&cursor]() {
        const quint64 value = qFromLittleEndian<quint64>(cursor);
        cursor += 8;
        return value;
    };
    auto readString = [&cursor](quint32 length) {
        const auto value = QString::fromUtf8(reinterpret_cast<const char *>(cursor), qsizetype(length));
        cursor += length;
        return value;
    };

    Snapshot snapshot;
    const quint32 files = read32();
    const quint32 dirs = read32();
    snapshot.session = read64();
    snapshot.revision = read64();
    const quint32 sort = read32();
    const quint32 dirLength = read32();
    if(sort > QIMGV_SORT_SIZE_DESC || dirLength > quint64(end - cursor))
        return false;
    snapshot.sort = int(sort);
    snapshot.directory = readString(dirLength);
    if(quint64(files) + dirs > quint64(end - cursor) / 40)
        return false;

    auto readRows = [&](quint32 count, bool isDir, std::vector<EngineEntry> &rows) {
        rows.reserve(count);
        for(quint32 row = 0; row < count; ++row) {
            if(end - cursor < 40)
                return false;
            EngineEntry entry;
            entry.id = read64();
            entry.contentRevision = read64();
            entry.size = read64();
            entry.mtimeMs = static_cast<qint64>(read64());
            const quint32 nameLength = read32();
            const quint32 pathLength = read32();
            if(entry.id == 0 || quint64(nameLength) + pathLength > quint64(end - cursor))
                return false;
            entry.name = readString(nameLength);
            entry.path = readString(pathLength);
            entry.isDir = isDir;
            rows.push_back(std::move(entry));
        }
        return true;
    };
    if(!readRows(files, false, snapshot.files) || !readRows(dirs, true, snapshot.dirs) || cursor != end)
        return false;
    out = std::move(snapshot);
    return true;
}

bool EngineBridge::readSnapshot(Snapshot &out) {
    QByteArray arena;
    quint32 files = 0, dirs = 0;
    quint64 revision = 0;
    return exportListing(arena, files, dirs, revision) && parseListing(arena, out);
}

bool EngineBridge::entryAt(int index, bool dirs, EngineEntry &out) const {
    if(!m_engine || index < 0)
        return false;
    qimgv_entry_meta meta{};
    char name[512], path[4096];
    const int r =
        dirs ? qimgv_engine_dir_entry_at(m_engine.m_d->engine, index, &meta, name, sizeof(name), path, sizeof(path))
             : qimgv_engine_file_entry_at(m_engine.m_d->engine, index, &meta, name, sizeof(name), path, sizeof(path));
    if(r != QIMGV_OK)
        return false;
    out.name = QString::fromUtf8(name);
    out.path = QString::fromUtf8(path);
    out.size = meta.size;
    out.mtimeMs = meta.mtime_ms;
    out.isDir = meta.is_dir != 0;
    return true;
}

void EngineBridge::drainEvents() {
    if(!m_engine)
        return;
    const auto session = m_sessionEpoch;
    const auto dir = m_requestedDir;
    bool changed = false;
    bool listed = false;
    int failure = QIMGV_OK;
    qimgv_event event{};
    for(;;) {
        const int result = qimgv_engine_take_event(m_engine.m_d->engine, &event, nullptr, 0, nullptr, 0, nullptr);
        if(result != 1) {
            if(result != 0)
                qWarning() << "[EngineBridge] take_event refused:" << result;
            break;
        }
        if(event.epoch != session)
            continue;
        if(traceEnabled())
            qDebug() << "[bridge trace] notice" << event.kind << "session" << event.epoch << "revision"
                     << event.revision;
        if(event.kind == QIMGV_EVENT_OPEN_FAILED) {
            failure = event.code;
        } else {
            changed = true;
            listed |= event.kind == QIMGV_EVENT_DIR_LISTED;
        }
    }
    if(listed)
        m_listedDir = dir;
    if(changed)
        emit listingChanged();
    if(m_sessionEpoch != session)
        return;
    if(listed)
        emit dirListed(dir);
    if(failure != QIMGV_OK && m_sessionEpoch == session)
        emit openFailed(dir, failure);
}
