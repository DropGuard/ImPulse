#include "directorymanager.h"

#include <QCollator>

#include "enginebridge.h"
#include "qimgv_engine.h"

#include <QLoggingCategory>
Q_LOGGING_CATEGORY(lcWatchPerf, "qimgv.perf.watch", QtInfoMsg)

#include <QTimer>
#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

DirectoryManager::DirectoryManager(EngineBridge &bridge) : m_bridge(bridge), m_sortingMode(SORT_NAME) {
    m_regex.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    readSettings();
    setSortingMode(settings->sortingMode());
    connect(settings, &Settings::settingsChanged, this, &DirectoryManager::readSettings);

    connect(&m_bridge, &EngineBridge::dirListed, this, &DirectoryManager::onEngineDirListed);
    connect(&m_bridge, &EngineBridge::openFailed, this, &DirectoryManager::onEngineOpenFailed);
    connect(&m_bridge, &EngineBridge::dirMutated, this, &DirectoryManager::onEngineMutated);
    m_appliedSort = m_sortingMode;
}

// ##############################################################
// ####################### PUBLIC METHODS #######################
// ##############################################################

void DirectoryManager::readSettings() {
    m_regex.setPattern(settings->supportedFormatsRegex());
    m_showHiddenFiles = settings->showHiddenFiles();
}

bool DirectoryManager::setDirectory(const QString &dirPath) {
    if(dirPath.isEmpty()) {
        m_engineDir.clear();
        if(m_rescanTimer)
            m_rescanTimer->stop();
        m_bridge.stopWatch();
        return false;
    }
    if(!directoryUsable(dirPath))
        return false;

    // The listing is asynchronous — it arrives via onEngineDirListed(), and the
    // vectors keep serving the previous directory until then. A *refused* request
    // is not asynchronous though: nothing will ever arrive, so it has to fail
    // here rather than leave the caller staring at a silently empty folder.
    if(!m_bridge.openDir(dirPath, engineSortMode(m_sortingMode), m_showHiddenFiles)) {
        qWarning() << "[DirectoryManager] engine refused" << dirPath << (qimgv_last_error() ? qimgv_last_error() : "");
        return false;
    }
    m_directoryPath = dirPath;
    m_engineDir = dirPath;
    m_appliedSort = m_sortingMode;
    markListingInSync();
    // a newer open supersedes everything armed for the previous session
    m_pendingOnListed.clear();
    return true;
}

// A folder's sibling order is a scan, not an object with state: this used to
// construct a throwaway DirectoryManager wired to the live bridge per
// folder-boundary keypress. The scan stays on the UI thread (walking the
// 100k-entry bench fixture costs 35 ms, regex-matching and collator-sorting
// every file in it costs 956 ms — measured), the collator decides the order,
// and the engine is never touched.
QString DirectoryManager::adjacentDirectory(const QString &dirPath, bool forward, SortingMode mode) {
    if(dirPath.isEmpty())
        return QString();
    const QString parent = QFileInfo(dirPath).absolutePath();
    if(!directoryUsable(parent))
        return QString();

    QStringList paths;
    for(const auto &entry : fs::directory_iterator(toStdString(parent))) {
        if(!entry.is_directory())
            continue;
        QString name = QString::fromStdString(entry.path().filename().generic_string());
        if(!m_showHiddenFiles && name.startsWith(QLatin1Char('.')))
            continue;
        paths.append(QString::fromStdString(entry.path().generic_string()));
    }

    // dirs order by name in every mode: the engine's comparator lands on its
    // name tiebreak because scan.rs gives every directory the same
    // size/mtime defaults — only NAME_DESC reverses
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(paths.begin(), paths.end(), [&collator](const QString &a, const QString &b) {
        return collator.compare(a, b) < 0;
    });
    if(mode == SortingMode::SORT_NAME_DESC)
        std::reverse(paths.begin(), paths.end());

    const int index = paths.indexOf(dirPath);
    if(index < 0)
        return QString();
    if(forward)
        return index + 1 < paths.size() ? paths.at(index + 1) : QString();
    return index > 0 ? paths.at(index - 1) : QString();
}

bool DirectoryManager::directoryUsable(const QString &dirPath) {
    if(!std::filesystem::exists(toStdString(dirPath))) {
        qDebug() << "[DirectoryManager] Error - path does not exist.";
        return false;
    }
    if(!std::filesystem::is_directory(toStdString(dirPath))) {
        qDebug() << "[DirectoryManager] Error - path is not a directory.";
        return false;
    }
    QDir dir(dirPath);
    if(!dir.isReadable()) {
        qDebug() << "[DirectoryManager] Error - cannot read directory.";
        return false;
    }
    return true;
}

const QString &DirectoryManager::directoryPath() const {
    return m_directoryPath;
}

int DirectoryManager::indexOfFile(const QString &filePath) const {
    auto item = find_if(m_fileEntryVec.begin(), m_fileEntryVec.end(), [filePath](const FSEntry &e) {
        return e.path == filePath;
    });
    if(item != m_fileEntryVec.end())
        return distance(m_fileEntryVec.begin(), item);
    return -1;
}

int DirectoryManager::indexOfDir(const QString &dirPath) const {
    auto item = find_if(m_dirEntryVec.begin(), m_dirEntryVec.end(), [dirPath](const FSEntry &e) {
        return e.path == dirPath;
    });
    if(item != m_dirEntryVec.end())
        return distance(m_dirEntryVec.begin(), item);
    return -1;
}

QString DirectoryManager::filePathAt(int index) const {
    return checkFileRange(index) ? m_fileEntryVec.at(index).path : "";
}

QString DirectoryManager::fileNameAt(int index) const {
    return checkFileRange(index) ? m_fileEntryVec.at(index).name : "";
}

QString DirectoryManager::dirPathAt(int index) const {
    return checkDirRange(index) ? m_dirEntryVec.at(index).path : "";
}

QString DirectoryManager::dirNameAt(int index) const {
    return checkDirRange(index) ? m_dirEntryVec.at(index).name : "";
}

QString DirectoryManager::firstFile() const {
    QString filePath = "";
    if(m_fileEntryVec.size())
        filePath = m_fileEntryVec.front().path;
    return filePath;
}

QString DirectoryManager::lastFile() const {
    QString filePath = "";
    if(m_fileEntryVec.size())
        filePath = m_fileEntryVec.back().path;
    return filePath;
}

QString DirectoryManager::prevOfFile(const QString &filePath) const {
    QString prevFilePath = "";
    int currentIndex = indexOfFile(filePath);
    if(currentIndex > 0)
        prevFilePath = m_fileEntryVec.at(currentIndex - 1).path;
    return prevFilePath;
}

QString DirectoryManager::nextOfFile(const QString &filePath) const {
    QString nextFilePath = "";
    int currentIndex = indexOfFile(filePath);
    if(currentIndex >= 0 && currentIndex < static_cast<int>(m_fileEntryVec.size()) - 1)
        nextFilePath = m_fileEntryVec.at(currentIndex + 1).path;
    return nextFilePath;
}

bool DirectoryManager::checkFileRange(int index) const {
    return index >= 0 && index < static_cast<int>(m_fileEntryVec.size());
}

bool DirectoryManager::checkDirRange(int index) const {
    return index >= 0 && index < static_cast<int>(m_dirEntryVec.size());
}

unsigned long DirectoryManager::totalCount() const {
    return fileCount() + dirCount();
}

unsigned long DirectoryManager::fileCount() const {
    return m_fileEntryVec.size();
}

unsigned long DirectoryManager::dirCount() const {
    return m_dirEntryVec.size();
}

const FSEntry &DirectoryManager::fileEntryAt(int index) const {
    if(checkFileRange(index))
        return m_fileEntryVec.at(index);
    else
        return m_defaultEntry;
}

QDateTime DirectoryManager::lastModified(const QString &filePath) const {
    QFileInfo info;
    if(containsFile(filePath))
        info.setFile(filePath);
    return info.lastModified();
}

// TODO: what about symlinks?
inline bool DirectoryManager::isSupportedFile(const QString &path) const {
    return (isFile(path) && m_regex.match(path).hasMatch());
}

bool DirectoryManager::isFile(const QString &path) const {
    if(!std::filesystem::exists(toStdString(path)))
        return false;
    if(!std::filesystem::is_regular_file(toStdString(path)))
        return false;
    return true;
}

bool DirectoryManager::isDir(const QString &path) const {
    if(!std::filesystem::exists(toStdString(path)))
        return false;
    if(!std::filesystem::is_directory(toStdString(path)))
        return false;
    return true;
}

bool DirectoryManager::isEmpty() const {
    return m_fileEntryVec.empty();
}

bool DirectoryManager::containsFile(const QString &filePath) const {
    return (std::find(m_fileEntryVec.begin(), m_fileEntryVec.end(), filePath) != m_fileEntryVec.end());
}

bool DirectoryManager::containsDir(const QString &dirPath) const {
    return (std::find(m_dirEntryVec.begin(), m_dirEntryVec.end(), dirPath) != m_dirEntryVec.end());
}

// ##############################################################
// ###################### PRIVATE METHODS #######################
// ##############################################################
void DirectoryManager::setSortingMode(SortingMode mode) {
    if(mode != m_sortingMode) {
        m_sortingMode = mode;
        if(!m_engineDir.isEmpty()) {
            // engine resorts and reports back via onEngineDirListed()
            m_bridge.setSort(engineSortMode(mode));
            return;
        }
        m_appliedSort = mode;
        // without an open directory the vectors are empty — they only fill
        // through the engine now that the subdirs bypass is gone
    }
}

SortingMode DirectoryManager::sortingMode() const {
    return m_sortingMode;
}

bool DirectoryManager::isSettled() const {
    return m_mirroredDir == m_engineDir && m_appliedSort == m_sortingMode && !m_resyncing && !m_wantRescan &&
           !(m_rescanTimer && m_rescanTimer->isActive());
}

// Entry management

void DirectoryManager::removeFileEntry(const QString &filePath) {
    if(!containsFile(filePath))
        return;
    int index = indexOfFile(filePath);
    m_fileEntryVec.erase(m_fileEntryVec.begin() + index);
    qCDebug(lcWatchPerf) << "fileRem" << filePath;
    emit fileRemoved(filePath, index);
}

// ---- dir entries

void DirectoryManager::removeDirEntry(const QString &dirPath) {
    if(!containsDir(dirPath))
        return;
    int index = indexOfDir(dirPath);
    m_dirEntryVec.erase(m_dirEntryVec.begin() + index);
    qCDebug(lcWatchPerf) << "dirRem" << dirPath;
    emit dirRemoved(dirPath, index);
}

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
// rust engine bridge

// settings.h's SortingMode order differs from the QIMGV_SORT_* codes — map
// explicitly, never positionally.
int DirectoryManager::engineSortMode(SortingMode mode) {
    switch(mode) {
    case SORT_NAME: return QIMGV_SORT_NAME;
    case SORT_NAME_DESC: return QIMGV_SORT_NAME_DESC;
    case SORT_SIZE: return QIMGV_SORT_SIZE;
    case SORT_SIZE_DESC: return QIMGV_SORT_SIZE_DESC;
    case SORT_TIME: return QIMGV_SORT_TIME;
    case SORT_TIME_DESC: return QIMGV_SORT_TIME_DESC;
    }
    return QIMGV_SORT_NAME;
}

// C++17 has no clock_cast; anchor the file clock against the system clock.
// Used only for mirror entries — change detection compares engine metadata,
// not these values.
std::filesystem::file_time_type DirectoryManager::fileTimeFromMs(qint64 ms) {
    using namespace std::chrono;
    using FileClock = std::filesystem::file_time_type::clock;
    const auto sysNow = system_clock::now();
    const auto target = system_clock::time_point(milliseconds(ms));
    return std::filesystem::file_time_type{FileClock::now() + (target - sysNow)};
}

// in-app file operations and an engine snapshot that looks stale both land
// here; the debounced re-open lets the engine catch up with local truth.
// The mirror keeps entries the engine's listing does not have (local truth the
// engine has not caught up with). Rescanning is the right response, but only a
// bounded number of times: a permanent disagreement is a bug to report, not a
// timer to run forever.
// The repair ladder this used to carry (bounded rescan rounds, a Behind/Diverged
// give-up state) died with the stat-based keep: nothing drives the mirror out of
// agreement anymore, so there is nothing to bound. What remains is the agreement
// mark a landed listing makes.
void DirectoryManager::markListingInSync() {
    m_wantRescan = false;
}

void DirectoryManager::scheduleRescan() {
    if(m_resyncing)
        return;
    if(m_directoryPath.isEmpty())
        return;
    if(!m_rescanTimer) {
        m_rescanTimer = new QTimer(this);
        m_rescanTimer->setSingleShot(true);
        m_rescanTimer->setInterval(150);
        connect(m_rescanTimer, &QTimer::timeout, this, [this]() {
            if(!m_resyncing && !m_directoryPath.isEmpty())
                m_bridge.openDir(m_directoryPath, engineSortMode(m_sortingMode), m_showHiddenFiles);
        });
    }
    m_rescanTimer->start();
}

void DirectoryManager::onEngineDirListed(const QString &dirPath) {
    if(m_resyncing || dirPath != m_engineDir)
        return;
    if(m_directoryPath != dirPath)
        return;
    m_resyncing = true;
    const bool applied = mirrorEngineListing(dirPath);
    m_resyncing = false;
    if(m_wantRescan) {
        m_wantRescan = false;
        scheduleRescan();
    }
    if(!applied) {
        // A rejected arena is not a listing: the actions armed for it must keep
        // waiting for one that arrives, rather than fire against a mirror that was
        // never updated. Bounded so a permanently broken engine cannot make the
        // 150ms rescan timer spin forever.
        if(++m_rejectedListings <= 3)
            qWarning() << "[DirectoryManager] listing rejected" << m_rejectedListings << "times for" << dirPath;
        else
            qCritical() << "[DirectoryManager] giving up on mirroring" << dirPath
                        << ": the engine keeps rejecting its own listing";
        return;
    }
    m_rejectedListings = 0;
    // deferred actions armed for this listing run with fresh counts; the
    // leftovers can never fire (the engine lists one directory at a time).
    // The queue is moved out first: an action may re-enter and arm its own
    // follow-up.
    auto pendings = std::move(m_pendingOnListed);
    m_pendingOnListed.clear();
    for(const auto &pending : pendings)
        if(pending.dir == dirPath)
            pending.action();
}

// The engine's own order is the contract: it decides where an entry sits, this
// side only splices. An out-of-range index is a wire bug — degrade to the
// nearest legal position (and say so) rather than corrupting memory; the
// convergence contract test fails long before this branch can matter.
static void insertAtEngineIndex(std::vector<FSEntry> &vec, FSEntry &&entry, int index) {
    const int limit = static_cast<int>(vec.size());
    if(index < 0 || index > limit) {
        qCWarning(lcWatchPerf) << "engine index out of range" << index << "of" << limit;
        index = std::clamp(index, 0, limit);
    }
    vec.insert(vec.begin() + index, std::move(entry));
}

// MODIFIED is applied from the wire: the watcher stat'ed the file before
// emitting, so a re-stat here would be a second decision about the same fact —
// and it is exactly where the old FSEntry self-assignment bug lived.
void DirectoryManager::applyEngineModification(const QString &path, bool isDir, quint64 size, qint64 mtimeMs,
                                               int index) {
    std::vector<FSEntry> &vec = isDir ? m_dirEntryVec : m_fileEntryVec;
    FSEntry entry;
    entry.path = path;
    entry.name = QFileInfo(path).fileName();
    entry.isDirectory = isDir;
    entry.size = size;
    entry.modifyTime = fileTimeFromMs(isDir ? 0 : mtimeMs);

    int current = isDir ? indexOfDir(path) : indexOfFile(path);
    if(current < 0) {
        // the mirror is behind the engine (it never saw the add): converge by
        // trusting the wire position instead of re-statting
        insertAtEngineIndex(vec, std::move(entry), index);
        if(isDir) {
            emit dirAdded(path);
        } else {
            m_prevFileMeta.insert(path, qMakePair(size, mtimeMs));
            emit fileAdded(path);
        }
        return;
    }
    // the wire index is the entry's FINAL position in the engine's post-update
    // order; erase first, then splice straight to it
    vec.erase(vec.begin() + current);
    insertAtEngineIndex(vec, std::move(entry), index);
    if(!isDir)
        m_prevFileMeta.insert(path, qMakePair(size, mtimeMs));
    emit fileModified(path);
}

// RENAMED is one row moving, and the pixels ride along with it: a rename
// changes a name and nothing else, so the thumbnail a row already holds is
// still the right picture for the path it now describes. Two shapes are not a
// move: an overwrite-by-move, where the engine removed the destination entry
// before relocating the source, and a source the mirror never saw — there the
// only way back into agreement is to insert at the wire position.
void DirectoryManager::applyEngineRename(const QString &from, const QString &to, bool isDir, quint64 size,
                                         qint64 mtimeMs, int index) {
    std::vector<FSEntry> &vec = isDir ? m_dirEntryVec : m_fileEntryVec;
    int source = isDir ? indexOfDir(from) : indexOfFile(from);
    int dest = isDir ? indexOfDir(to) : indexOfFile(to);

    // The engine's index counts the vector *without* the entry it removed, so
    // the overwritten row has to be gone before the source is spliced — and its
    // removal has to be announced first, because every index after it shifts.
    if(dest >= 0 && dest != source) {
        vec.erase(vec.begin() + dest);
        if(!isDir)
            m_prevFileMeta.remove(to);
        if(dest < source)
            --source;
        if(isDir)
            emit dirRemoved(to, dest);
        else
            emit fileRemoved(to, dest);
    }

    if(source < 0) {
        FSEntry entry;
        entry.path = to;
        entry.name = QFileInfo(to).fileName();
        entry.isDirectory = isDir;
        entry.size = size;
        entry.modifyTime = fileTimeFromMs(isDir ? 0 : mtimeMs);
        insertAtEngineIndex(vec, std::move(entry), index);
        if(isDir) {
            emit dirAdded(to);
        } else {
            m_prevFileMeta.insert(to, qMakePair(size, mtimeMs));
            emit fileAdded(to);
        }
        return;
    }

    // rewrite in place, then splice: the row itself never leaves the view
    vec[source].path = to;
    vec[source].name = QFileInfo(to).fileName();
    vec[source].size = size;
    vec[source].modifyTime = fileTimeFromMs(isDir ? 0 : mtimeMs);
    FSEntry moved = std::move(vec[source]);
    vec.erase(vec.begin() + source);
    insertAtEngineIndex(vec, std::move(moved), index);
    if(isDir) {
        emit dirRenamed(from, to, source);
    } else {
        m_prevFileMeta.remove(from);
        m_prevFileMeta.insert(to, qMakePair(size, mtimeMs));
        emit fileRenamed(from, to, source);
    }
}

void DirectoryManager::onEngineMutated(int kind, const QString &path, bool isDir, quint64 size, qint64 mtimeMs,
                                       int index, const QString &oldPath) {
    const bool tracing = EngineBridge::traceEnabled();
    if(m_resyncing) {
        if(tracing)
            qDebug() << "[mirror trace] drop (resync in progress)" << path;
        return;
    }
    // A mutation edits the listing the vectors currently hold, so its path has
    // to live in the directory those vectors describe. The wire carries no
    // directory for a mutation, and the engine's order index is computed
    // against its own listing — splicing a foreign path in would put a row from
    // another folder at a position that means nothing here. Nothing has been
    // mirrored yet, so there is nothing to contradict.
    if(!m_mirroredDir.isEmpty() && !path.startsWith(m_mirroredDir + QLatin1Char('/'))) {
        qWarning() << "[DirectoryManager] mutation outside the mirrored directory:" << path
                   << "mirrored:" << m_mirroredDir;
        return;
    }
    switch(kind) {
    case QIMGV_MUT_ADDED: {
        if(containsFile(path) || containsDir(path)) {
            if(tracing)
                qDebug() << "[mirror trace] drop (already mirrored)" << path;
            return; // already mirrored (racing duplicate)
        }
        FSEntry entry;
        entry.path = path;
        entry.name = QFileInfo(path).fileName();
        entry.isDirectory = isDir;
        entry.size = size;
        entry.modifyTime = fileTimeFromMs(isDir ? 0 : mtimeMs);
        insertAtEngineIndex(isDir ? m_dirEntryVec : m_fileEntryVec, std::move(entry), index);
        if(isDir) {
            emit dirAdded(path);
        } else {
            m_prevFileMeta.insert(path, qMakePair(size, mtimeMs));
            emit fileAdded(path);
        }
        break;
    }
    case QIMGV_MUT_MODIFIED: applyEngineModification(path, isDir, size, mtimeMs, index); break;
    case QIMGV_MUT_RENAMED: {
        // The guard above can only see the destination. A rename also edits
        // the row its *source* names, so that path has to live here too —
        // otherwise the mirror would erase an unrelated entry and grow a row
        // from another folder at an index computed against that folder.
        if(!m_mirroredDir.isEmpty() && !oldPath.startsWith(m_mirroredDir + QLatin1Char('/'))) {
            qWarning() << "[DirectoryManager] rename source outside the mirrored directory:" << oldPath
                       << "mirrored:" << m_mirroredDir;
            return;
        }
        applyEngineRename(oldPath, path, isDir, size, mtimeMs, index);
        break;
    }
    case QIMGV_MUT_REMOVED:
        removeDirEntry(path);
        removeFileEntry(path);
        break;
    default: break;
    }
    if(tracing) {
        // the counts, not just the kind: the failure this trace exists to name is
        // a mutation that lands and still leaves the mirror one entry short
        qDebug() << "[mirror trace] applied kind" << kind << path << "index" << index << "files"
                 << m_fileEntryVec.size() << "dirs" << m_dirEntryVec.size();
    }
}

qint64 DirectoryManager::whenListed(const QString &dirPath, std::function<void()> action) {
    const qint64 token = m_nextWhenListedToken++;
    m_pendingOnListed.append({token, dirPath, std::move(action)});
    return token;
}

bool DirectoryManager::cancelListed(qint64 token) {
    for(int i = 0; i < m_pendingOnListed.size(); ++i) {
        if(m_pendingOnListed[i].token == token) {
            m_pendingOnListed.remove(i);
            return true;
        }
    }
    return false;
}

void DirectoryManager::onEngineOpenFailed(const QString &dirPath, int code) {
    if(dirPath != m_engineDir)
        return;
    qWarning() << "[DirectoryManager] engine open failed:" << dirPath << "code" << code;
    // an open that failed will never produce the listing its actions wait for
    m_pendingOnListed.erase(std::remove_if(m_pendingOnListed.begin(), m_pendingOnListed.end(),
                                           [&](const PendingListed &p) {
                                               return p.dir == dirPath;
                                           }),
                            m_pendingOnListed.end());
    emit loadingFailed(dirPath);
}

bool DirectoryManager::mirrorEngineListing(const QString &dirPath) {
    // bulk pull: the whole listing crosses in one engine lock pass
    QByteArray listing;
    quint32 newFileCount = 0, newDirCount = 0;
    if(!m_bridge.exportListing(listing, newFileCount, newDirCount))
        return false;
    return applyArena(dirPath, listing, newFileCount, newDirCount);
}

bool DirectoryManager::applyArena(const QString &dirPath, const QByteArray &listing, quint32 newFileCount,
                                  quint32 newDirCount) {
    QElapsedTimer rebuildTimer;
    rebuildTimer.start();

    // parse the arena into mirror entries
    QElapsedTimer parseTimer;
    parseTimer.start();
    std::vector<EngineBridge::EngineEntry> files, dirs;
    if(!EngineBridge::parseListing(listing, files, dirs)) {
        // Nothing was parsed (a rejected arena yields no records), and installing
        // the prefix that happened to decode would create a directory that does
        // not exist anywhere: entries silently missing from the view, from the
        // counts and from end-of-folder navigation. Keep the old mirror and ask
        // for another listing.
        qWarning() << "[DirectoryManager] listing arena rejected:" << listing.size() << "bytes, header claimed"
                   << newFileCount << "files +" << newDirCount << "dirs";
        m_wantRescan = true;
        return false;
    }
    qDebug() << "[DirectoryManager] arena pull+parse" << parseTimer.elapsed() << "ms; parsed" << files.size() << "files"
             << dirs.size() << "dirs; arena" << listing.size() << "bytes";

    QElapsedTimer hashTimer;
    hashTimer.start();
    QHash<QString, QPair<quint64, qint64>> newMeta;
    newMeta.reserve(newFileCount * 2);
    for(const auto &f : files)
        newMeta.insert(f.path, {f.size, f.mtimeMs});

    QSet<QString> newPaths;
    newPaths.reserve(newFileCount * 2);
    for(const auto &f : files)
        newPaths.insert(f.path);
    QSet<QString> newDirPaths;
    newDirPaths.reserve(newDirCount * 2);
    for(const auto &d : dirs)
        newDirPaths.insert(d.path);
    qDebug() << "[dbg] parse" << parseTimer.elapsed() << "ms; hash containers" << hashTimer.elapsed() << "ms";

    // The mirror is a pure projection of the export: an entry the export lacks
    // is gone, with no stat of the filesystem to second-guess the engine. The
    // stat-based keep that used to live here repaired missed events silently
    // and made divergence unmeasurable; missed events now surface through the
    // convergence contract instead. Local mutations no longer run ahead of the
    // engine.

    const bool isNewDir = (m_mirroredDir != dirPath);
    QElapsedTimer rebuildTimer2;
    rebuildTimer2.start();

    if(isNewDir) {
        m_fileEntryVec.clear();
        m_dirEntryVec.clear();
        for(const auto &f : files) {
            FSEntry entry;
            entry.path = f.path;
            entry.name = f.name;
            entry.isDirectory = false;
            entry.size = f.size;
            entry.modifyTime = fileTimeFromMs(f.mtimeMs);
            m_fileEntryVec.push_back(std::move(entry));
        }
        for(const auto &d : dirs) {
            FSEntry entry;
            entry.path = d.path;
            entry.name = d.name;
            entry.isDirectory = true;
            m_dirEntryVec.push_back(std::move(entry));
        }
        m_prevFileMeta = std::move(newMeta);
        m_mirroredDir = dirPath;
        qDebug() << "[DirectoryManager] listing vectors rebuilt" << rebuildTimer2.elapsed() << "ms";
        // NOTE: emit loaded triggers the view-layer repopulation (100k items
        // measured ~400ms in the gui/ layer) — on the same UI thread but
        // belonging to the view, not to this mirror
        emit loaded(m_directoryPath);
        qDebug() << "[DirectoryManager] listing rebuild (incl. view repopulation)" << newFileCount << "files took"
                 << rebuildTimer.elapsed() << "ms";
        // a fresh listing is the definition of agreement
        markListingInSync();
        return true;
    }

    // same directory: granular resync — keep views/selection stable
    QSet<QString> oldPaths;
    for(const auto &f : m_fileEntryVec)
        oldPaths.insert(f.path);
    QSet<QString> oldDirPaths;
    for(const auto &d : m_dirEntryVec)
        oldDirPaths.insert(d.path);

    // 1. removals (descending index keeps earlier indexes valid); an entry the
    //    export lacks is removed unconditionally — the mirror never consults
    //    the filesystem against the engine
    for(int i = static_cast<int>(m_fileEntryVec.size()) - 1; i >= 0; --i) {
        const QString &p = m_fileEntryVec[static_cast<size_t>(i)].path;
        if(!newPaths.contains(p)) {
            emit fileRemoved(p, i);
            m_fileEntryVec.erase(m_fileEntryVec.begin() + i);
        }
    }
    for(int i = static_cast<int>(m_dirEntryVec.size()) - 1; i >= 0; --i) {
        const QString &p = m_dirEntryVec[static_cast<size_t>(i)].path;
        if(!newDirPaths.contains(p)) {
            emit dirRemoved(p, i);
            m_dirEntryVec.erase(m_dirEntryVec.begin() + i);
        }
    }

    // 2. changed files: compare engine metadata against the previous mirror —
    //    the signal is all the mirror owes the view; the entry content comes
    //    from the export in step 3, never from a native re-stat
    for(const auto &f : files) {
        if(oldPaths.contains(f.path)) {
            const auto prev = m_prevFileMeta.constFind(f.path);
            if(prev == m_prevFileMeta.constEnd() || prev->first != f.size || prev->second != f.mtimeMs)
                emit fileModified(f.path);
        }
    }

    // 3. rebuild both vectors in engine order
    m_fileEntryVec.clear();
    for(const auto &f : files) {
        FSEntry entry;
        entry.path = f.path;
        entry.name = f.name;
        entry.isDirectory = false;
        entry.size = f.size;
        entry.modifyTime = fileTimeFromMs(f.mtimeMs);
        m_fileEntryVec.push_back(std::move(entry));
    }
    m_dirEntryVec.clear();
    for(const auto &d : dirs) {
        FSEntry entry;
        entry.path = d.path;
        entry.name = d.name;
        entry.isDirectory = true;
        m_dirEntryVec.push_back(std::move(entry));
    }

    // 4. additions (emitted after the vectors hold the final state)
    for(const auto &f : files)
        if(!oldPaths.contains(f.path))
            emit fileAdded(f.path);
    for(const auto &d : dirs)
        if(!oldDirPaths.contains(d.path))
            emit dirAdded(d.path);

    // 5. resort signal if the sort mode changed since the last mirror
    if(m_appliedSort != m_sortingMode) {
        m_appliedSort = m_sortingMode;
        emit sortingChanged();
    }

    m_prevFileMeta = std::move(newMeta);
    qDebug() << "[DirectoryManager] listing resync" << newFileCount << "files took" << rebuildTimer.elapsed() << "ms";
    markListingInSync();
    return true;
}
