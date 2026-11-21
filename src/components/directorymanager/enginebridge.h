#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

// The engine handle, owned by value. Its destructor is the ONLY place
// the engine is freed, and dropping it is what makes the ordering
// guarantee explicit: free joins the engine's loop thread, the loop joins the
// watcher thread, and only then can EngineBridge's own destruction finish —
// so a notify callback posted from an engine thread cannot outlive the
// receiving QObject. Holding the handle by value means that ordering cannot
// be skipped by forgetting a line in a destructor, which is the shape this
// tree avoids everywhere else (commit() for table writes, WorkerPool for
// jobs).
//
// TRUE PIMPL here, for a specific reason: the engine's C type must not appear
// in a public header at all. It used to, through reset()/get() parameters, and
// R2 — which scans for the FFI vocabulary — then could not see the leak. An
// erased void* fixed the symptom and cost compile-time type safety for no
// gain; a private implementation struct keeps both. The cost is a constructor
// and a destructor out of line, and it is the whole cost: the handle stays
// non-copyable and non-movable (both are deleted below, and the unique_ptr
// would not have made it movable on its own).
//
// Non-copyable, non-movable: exactly one owner, exactly one free.
class EngineHandle {
  public:
    EngineHandle();
    ~EngineHandle();
    EngineHandle(const EngineHandle &) = delete;
    EngineHandle &operator=(const EngineHandle &) = delete;
    EngineHandle(EngineHandle &&) = delete;
    EngineHandle &operator=(EngineHandle &&) = delete;

    // Takes ownership of an engine created by the FFI constructor; nullptr is a
    // live-but-dead bridge, which the callers test through operator bool.
    // The parameter is void* only because the FFI type is not nameable here —
    // the definition casts once, in the translation unit that owns the type.
    void adopt(void *created);
    explicit operator bool() const;
    // the .cpp's own FFI calls need the typed handle; nothing outside this
    // translation unit may ask for it, so it is not declared here — the .cpp
    // reads d->engine directly.

  private:
    struct Impl;
    // Defined in the .cpp: Impl is incomplete here, so this is also what makes
    // the destructor's ordering (see above) stay out of the header.
    std::unique_ptr<Impl> m_d;
    // EngineBridge issues the FFI calls; it reads d->engine and nothing else.
    // A narrower door than a public accessor: no other type can reach it.
    friend class EngineBridge;
};

// Owns the process-wide Rust engine instance and pumps its events into the
// Qt event loop. The engine's notify callback queues a drain on the UI
// thread's event loop, and the drain pulls the engine's events until the
// engine is empty. Engine calls never block the UI thread, and the hop is
// Qt's own queued-invoke vocabulary, so the file carries no platform branch.
class EngineBridge : public QObject {
    Q_OBJECT
  public:
    struct EngineEntry {
        QString name, path;
        quint64 size = 0;
        qint64 mtimeMs = 0;
        bool isDir = false;
    };

    static EngineBridge &instance();
    // Construction is public: the class is instance-safe (each object owns its
    // engine), and tests need isolated bridges — the
    // global instance's watch state would otherwise force every test into
    // unique-directory workarounds. Production keeps using instance().
    explicit EngineBridge(QObject *parent = nullptr);
    ~EngineBridge() override;

    EngineBridge(EngineBridge const &) = delete;
    EngineBridge &operator=(EngineBridge const &) = delete;

    // Accepted asynchronously (returns false only if the engine is dead);
    // results arrive later via dirListed()/openFailed(). sortMode uses the
    // QIMGV_SORT_* codes; the caller maps qimgv's SortingMode explicitly.
    bool openDir(const QString &path, int sortMode, bool showHidden);
    void setSort(int sortMode);
    void setShowHidden(bool show);

    // The directory the reads below currently describe. openDir is accepted
    // asynchronously and the engine swaps its whole listing in one lock when the
    // scan finishes, so between the call and the dirListed event these reads
    // return the PREVIOUS directory — legitimately, and the reason a caller that
    // waits on a count alone can read someone else's listing. Comes from the
    // engine's own event, not from what was requested.
    const QString &listedDirectory() const;

    // synchronous reads of the engine's current listing. Test-only today:
    // production mirrors through exportListing below, and everything here
    // answers for the PREVIOUS directory until the scan lands (see above).
    int fileCount() const;
    int dirCount() const;
    bool entryAt(int index, bool dirs, EngineEntry &out) const;
    // Bulk pull: the whole listing serialized in one engine lock pass, and the
    // version it was taken at. The two come back together because they mean
    // one thing — without the revision a reader can only compare two pulls and
    // hope nothing moved between them.
    bool exportListing(QByteArray &out, quint32 &fileCount, quint32 &dirCount, quint64 &revision);
    // Decodes the arena produced by exportListing (wire format documented in
    // model.rs::write_listing). Static and side-effect free so the producer and
    // this consumer can be round-tripped against each other in a test.
    // Returns false unless the record counts and the arena agree exactly —
    // a truncated tail or producer/consumer drift.
    static bool parseListing(const QByteArray &arena, std::vector<EngineEntry> &files, std::vector<EngineEntry> &dirs);

    // Joins the engine's extension whitelist wire format ('\n'-separated).
    // Pure: the formats come in, so tests can pin the seam where an empty
    // list once meant "accept nothing" against the documented NULL contract.
    // An empty result means "accept everything" (the FFI constructor's NULL).
    static QByteArray extensionListFor(const QList<QByteArray> &formats);

    // The event-path trace, on when QIMGV_ENGINE_TRACE is set to anything but
    // "0". The engine reads the same variable name on its side, so a line
    // printed here and a line printed there are the same run; the seq in
    // qimgv_event is what lets the two halves be matched up. Shared rather than
    // duplicated because a switch that has to be turned on twice is a switch
    // that gets turned on once.
    static bool traceEnabled();
    void stopWatch();

  signals:
    void dirListed(const QString &dirPath);
    // the same rows, reordered: the mirror permutes what it holds
    void sortChanged(const QString &dirPath, quint64 revision);
    void openFailed(const QString &dirPath, int statusCode);
    void dirMutated(int kind, const QString &path, bool isDir, quint64 size, qint64 mtimeMs, int index,
                    const QString &oldPath, quint64 revision);

  private:
    static void notifyCb(void *user);

  private slots:
    void drainEvents();

  private:
    EngineHandle m_engine;
    quint32 m_sessionEpoch = 0;
    QString m_listedDir;
};
