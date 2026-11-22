#pragma once

#include <QObject>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <functional>
#include <vector>

#include "enginebridge.h"
#include "settings.h"
#include "sourcecontainers/fsentry.h"

struct ListingFileChanges {
    struct Displacement {
        quint64 id = 0;
        QString oldPath;
        QString newPath;
        int oldIndex = 0;
    };
    QList<Displacement> displaced;
    QStringList contentChanged;
    bool hasAdditions = false;
};

class DirectoryManager : public QObject {
    Q_OBJECT
  public:
    // Which engine this instance mirrors. Required, not defaulted: a manager
    // that quietly picks the process-wide bridge cannot be pointed at another
    // one, and the ambiguity is not hypothetical — a test that spies on one
    // bridge while its manager talks to a different one sees no events at all and
    // reports that as the design's fault. Production passes
    // EngineBridge::instance() at the construction site, where it is visible.
    explicit DirectoryManager(EngineBridge &bridge);
    // Re-opens unconditionally — even the directory already open: the previous
    // session's listing answer is superseded, and armed whenListed actions for
    // OTHER directories are dropped. Actions armed for the directory being
    // reopened survive: the open IS the listing they are waiting for.
    bool setDirectory(const QString &);
    // The folder a folder-boundary transition should enter: this folder's place in
    // its parent's order, resolved synchronously because a folder-boundary keypress
    // needs the answer right there. Empty result means there is nowhere to go and
    // the caller shows its boundary message. Never touches the shared engine.
    // mode is the caller listing's own sort mode: the answer has to follow the
    // order the user is looking at, not the persisted setting (a manager can be
    // resorted at runtime without the setting moving)
    QString adjacentDirectory(const QString &dirPath, bool forward, SortingMode mode);
    // Arms `action` to run once the listing for dirPath lands (mirror
    // rebuilt, counts fresh). One engine session only: a newer openDir
    // supersedes the queue, an open failure drops its own entry, and
    // cancelListed revokes one. For decisions only knowable once the listing
    // exists (Core's folder-boundary load). Returns the cancel token.
    qint64 whenListed(const QString &dirPath, std::function<void()> action);
    bool cancelListed(qint64 token);
    const QString &directoryPath() const;
    int indexOfFile(const QString &filePath) const;
    int indexOfDir(const QString &dirPath) const;
    QString filePathAt(int index) const;
    unsigned long fileCount() const;
    unsigned long dirCount() const;
    inline bool isSupportedFile(const QString &filePath) const;
    bool isEmpty() const;
    bool isSettled() const;
    quint64 appliedRevision() const {
        return m_appliedRevision;
    }
    SortingMode appliedSortingMode() const {
        return m_appliedSort;
    }

    bool containsFile(const QString &filePath) const;
    QString fileNameAt(int index) const;
    QString prevOfFile(const QString &filePath) const;
    QString nextOfFile(const QString &filePath) const;
    QDateTime lastModified(const QString &filePath) const;

    QString firstFile() const;
    QString lastFile() const;
    void setSortingMode(SortingMode mode);
    SortingMode sortingMode() const;
    bool isFile(const QString &path) const;
    bool isDir(const QString &path) const;

    unsigned long totalCount() const;
    bool containsDir(const QString &dirPath) const;
    const FSEntry &fileEntryAt(int index) const;
    const FSEntry &dirEntryAt(int index) const;
    QString dirPathAt(int index) const;
    QString dirNameAt(int index) const;

    // The one place qimgv's SortingMode becomes a QIMGV_SORT_* code. Public so a
    // test can pin it against the header constants: the two enums are ordered
    // differently, so a positional mistake sorts by the wrong column silently.
    static int engineSortMode(SortingMode mode);

  private:
    EngineBridge &m_bridge;
    QRegularExpression m_regex;
    std::vector<FSEntry> m_fileEntryVec, m_dirEntryVec;
    const FSEntry m_defaultEntry;
    QString m_directoryPath;

    QString m_mirroredDir;
    quint64 m_requestedSession = 0;
    quint64 m_appliedSession = 0;
    SortingMode m_appliedSort = SORT_NAME;
    quint64 m_appliedRevision = 0;
    QTimer m_snapshotRetry;
    bool m_snapshotDirty = false;
    bool m_publishing = false;

    struct PendingListed {
        qint64 token = 0;
        QString dir;
        std::function<void()> action;
    };
    qint64 m_nextWhenListedToken = 1;
    QVector<PendingListed> m_pendingOnListed;

    void pullSnapshot();
    void applySnapshot(EngineBridge::Snapshot snapshot);
    static bool directoryUsable(const QString &dirPath);
    static std::filesystem::file_time_type fileTimeFromMs(qint64 ms);

    void readSettings();
    // synced from the settings in readSettings(); the three scan/filter sites
    // read this member instead of reaching for the global
    bool m_showHiddenFiles = false;
    SortingMode m_sortingMode;

    // Directories only ever order by name: the engine's comparator lands on
    // its name tiebreak because scan.rs gives every directory the same
    // size/mtime defaults.

    bool checkFileRange(int index) const;
    bool checkDirRange(int index) const;

  private slots:
    void onEngineListingChanged();
    void onEngineOpenFailed(const QString &dirPath, int code);

  signals:
    void listingChanged();
    void loaded(const QString &path);
    void sortingChanged();
    void loadingFailed(const QString &dirPath);
    // A single comparison of two same-session snapshots, not a sequence of
    // operations: each old path maps directly to its new path (empty if gone).
    void filesChanged(const ListingFileChanges &changes);
};
