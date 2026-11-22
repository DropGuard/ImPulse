#include "directorymanager.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QElapsedTimer>
#include <QScopedValueRollback>

#include "enginebridge.h"
#include "qimgv_engine.h"
#include "utils/stuff.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace fs = std::filesystem;

DirectoryManager::DirectoryManager(EngineBridge &bridge) : m_bridge(bridge), m_sortingMode(SORT_NAME) {
    m_regex.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    readSettings();
    setSortingMode(settings->sortingMode());
    connect(settings, &Settings::settingsChanged, this, &DirectoryManager::readSettings);

    connect(&m_bridge, &EngineBridge::listingChanged, this, &DirectoryManager::onEngineListingChanged);
    connect(&m_bridge, &EngineBridge::openFailed, this, &DirectoryManager::onEngineOpenFailed);
    m_snapshotRetry.setSingleShot(true);
    connect(&m_snapshotRetry, &QTimer::timeout, this, &DirectoryManager::pullSnapshot);
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
        m_requestedSession = 0;
        m_snapshotDirty = false;
        m_snapshotRetry.stop();
        m_pendingOnListed.clear();
        m_bridge.stopWatch();
        return false;
    }
    if(!directoryUsable(dirPath))
        return false;

    // The listing is asynchronous — it arrives as a snapshot, and the
    // vectors keep serving the previous directory until then. A *refused* request
    // is not asynchronous though: nothing will ever arrive, so it has to fail
    // here rather than leave the caller staring at a silently empty folder.
    if(!m_bridge.openDir(dirPath, engineSortMode(m_sortingMode), m_showHiddenFiles)) {
        qWarning() << "[DirectoryManager] engine refused" << dirPath << (qimgv_last_error() ? qimgv_last_error() : "");
        return false;
    }
    m_directoryPath = dirPath;
    m_requestedSession = m_bridge.session();
    m_snapshotDirty = false;
    m_snapshotRetry.stop();
    // a newer open supersedes everything armed for the PREVIOUS session — but
    // an action armed for the directory being reopened survives it: the open
    // IS the listing that action is waiting for, and wiping it here killed the
    // natural arm-then-open ordering on nothing but convention
    m_pendingOnListed.erase(std::remove_if(m_pendingOnListed.begin(), m_pendingOnListed.end(),
                                           [&dirPath](const PendingListed &p) {
                                               return p.dir != dirPath;
                                           }),
                            m_pendingOnListed.end());
    return true;
}

// A folder's sibling order is a scan, not an object with state: this used to
// construct a throwaway DirectoryManager wired to the live bridge per
// folder-boundary keypress. The scan stays on the UI thread (walking the
// 100k-entry bench fixture costs 35 ms, regex-matching and collator-sorting
// every file in it costs 956 ms — measured) and the engine is never touched.
QString DirectoryManager::adjacentDirectory(const QString &dirPath, bool forward, SortingMode mode) {
    if(dirPath.isEmpty())
        return QString();
    const QString parent = QFileInfo(dirPath).absolutePath();
    if(!directoryUsable(parent))
        return QString();

    QStringList paths;
    for(const auto &entry : fs::directory_iterator(toStdString(parent))) {
        // Follows a symlink to a directory, like scan.rs does (it re-stats a
        // symlink and keeps it when the target is a directory); a broken link
        // answers is_directory()==false on both sides and is skipped.
        if(!entry.is_directory())
            continue;
        QString name = QString::fromStdString(entry.path().filename().generic_string());
        // The dotfile half of the engine's hidden rule. NOT the whole rule:
        // scan.rs also drops a Windows file-system-hidden attribute when
        // showHidden is false, and this scan does not — a hidden-attribute,
        // non-dot directory would then sit in the sibling order but not in the
        // listing. Unix has no such attribute (scan.rs's half is a no-op there),
        // so the two agree everywhere but that Windows corner; folding it in
        // needs the engine's own membership decision, which this synchronous
        // on-the-UI-thread scan exists to avoid.
        if(!m_showHiddenFiles && name.startsWith(QLatin1Char('.')))
            continue;
        paths.append(QString::fromStdString(entry.path().generic_string()));
    }

    // Dirs order by name in every mode: scan.rs gives every directory the same
    // size/mtime defaults, so the engine's comparator lands on its name
    // tiebreak, and only NAME_DESC reverses.
    //
    // The order is asked of the engine rather than re-derived with a collator.
    // QCollator is an approximation and the two disagree on names that differ
    // only in case: measured on {IMG2.jpg, img2.jpg}, QCollator puts the
    // lowercase first and the engine's comparator the uppercase — so sibling
    // navigation could land on a different folder than the one next to the
    // current row on screen.
    std::sort(paths.begin(), paths.end(), [&mode](const QString &a, const QString &b) {
        return EngineBridge::compareNames(a, b, mode) < 0;
    });

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

const FSEntry &DirectoryManager::dirEntryAt(int index) const {
    return checkDirRange(index) ? m_dirEntryVec.at(index) : m_defaultEntry;
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
// A sort is the engine's decision, like every other order question. This side
// asks for it and waits: the engine resorts in place, commits a revision, and
// notifies; the pull that follows is the same one every other commit takes.
// The rows it installs carry the same IDs, so the views keep their widgets —
// what a sort changes is position, and position is the one thing a widget
// does not own.
void DirectoryManager::setSortingMode(SortingMode mode) {
    if(mode == m_sortingMode)
        return;
    m_sortingMode = mode;
    if(m_requestedSession != 0)
        m_bridge.setSort(engineSortMode(mode));
}

SortingMode DirectoryManager::sortingMode() const {
    return m_sortingMode;
}

bool DirectoryManager::isSettled() const {
    return !m_snapshotDirty &&
           (m_requestedSession == 0 || (m_requestedSession == m_appliedSession && m_appliedSort == m_sortingMode));
}

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

void DirectoryManager::onEngineListingChanged() {
    if(m_requestedSession == 0)
        return;
    m_snapshotDirty = true;
    pullSnapshot();
}

void DirectoryManager::pullSnapshot() {
    if(!m_snapshotDirty || m_publishing)
        return;
    EngineBridge::Snapshot snapshot;
    if(!m_bridge.readSnapshot(snapshot)) {
        m_snapshotRetry.start(20);
        return;
    }
    m_snapshotDirty = false;
    m_snapshotRetry.stop();
    if(snapshot.session != m_requestedSession || snapshot.revision <= m_appliedRevision)
        return;
    {
        const QScopedValueRollback publishing(m_publishing, true);
        applySnapshot(std::move(snapshot));
    }
    if(m_snapshotDirty)
        m_snapshotRetry.start(0);
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
    if(m_requestedSession == 0 || dirPath != m_directoryPath)
        return;
    qWarning() << "[DirectoryManager] engine open failed:" << dirPath << "code" << code;
    m_requestedSession = 0;
    m_snapshotDirty = false;
    m_snapshotRetry.stop();
    m_directoryPath = m_mirroredDir;
    m_pendingOnListed.clear();
    emit loadingFailed(dirPath);
}

void DirectoryManager::applySnapshot(EngineBridge::Snapshot snapshot) {
    const auto sortFromEngine = [](int sort) {
        switch(sort) {
        case QIMGV_SORT_NAME_DESC: return SORT_NAME_DESC;
        case QIMGV_SORT_SIZE: return SORT_SIZE;
        case QIMGV_SORT_SIZE_DESC: return SORT_SIZE_DESC;
        case QIMGV_SORT_TIME: return SORT_TIME;
        case QIMGV_SORT_TIME_DESC: return SORT_TIME_DESC;
        default: return SORT_NAME;
        }
    };
    const auto makeEntries = [](std::vector<EngineBridge::EngineEntry> rows) {
        std::vector<FSEntry> entries;
        entries.reserve(rows.size());
        for(auto &row : rows) {
            FSEntry entry(std::move(row.path), std::move(row.name), row.size, fileTimeFromMs(row.mtimeMs), row.isDir);
            entry.id = row.id;
            entry.contentRevision = row.contentRevision;
            entries.push_back(std::move(entry));
        }
        return entries;
    };

    const bool opened = snapshot.session != m_appliedSession;
    const auto sort = sortFromEngine(snapshot.sort);
    const bool resorted = sort != m_appliedSort;
    const auto previousFiles = std::exchange(m_fileEntryVec, makeEntries(std::move(snapshot.files)));
    m_dirEntryVec = makeEntries(std::move(snapshot.dirs));
    m_mirroredDir = std::move(snapshot.directory);
    m_appliedSession = snapshot.session;
    m_appliedRevision = snapshot.revision;
    m_appliedSort = sort;

    ListingFileChanges changes;
    if(!opened) {
        QHash<quint64, int> oldIndices;
        oldIndices.reserve(qsizetype(previousFiles.size()));
        for(int index = 0; index < int(previousFiles.size()); ++index)
            oldIndices.insert(previousFiles[index].id, index);
        for(const auto &entry : m_fileEntryVec) {
            const auto previous = oldIndices.find(entry.id);
            if(previous == oldIndices.end()) {
                changes.hasAdditions = true;
                continue;
            }
            const int oldIndex = previous.value();
            const auto &old = previousFiles[oldIndex];
            if(old.path != entry.path)
                changes.displaced.append(ListingFileChanges::Displacement{entry.id, old.path, entry.path, oldIndex});
            if(old.contentRevision != entry.contentRevision)
                changes.contentChanged.append(entry.path);
            oldIndices.erase(previous);
        }
        for(const int oldIndex : std::as_const(oldIndices)) {
            const auto &old = previousFiles[oldIndex];
            changes.displaced.append(ListingFileChanges::Displacement{old.id, old.path, {}, oldIndex});
        }
    }

    emit listingChanged();
    // Startup and per-open timing, ms since the process started
    // (QElapsedTimer's static epoch): the engine logs its own scan+sort split,
    // and this line is the mirror-side counterpart — the gap between the two
    // numbers is the mirror's cost, not the engine's.
    qInfo() << "[snapshot] applied session" << snapshot.session << "rev" << snapshot.revision << "("
            << m_fileEntryVec.size() << "files," << m_dirEntryVec.size() << "dirs) at" << startupMs() << "ms";
    if(opened)
        emit loaded(m_mirroredDir);
    else if(changes.hasAdditions || !changes.displaced.isEmpty() || !changes.contentChanged.isEmpty())
        emit filesChanged(changes);
    if(resorted)
        emit sortingChanged();
    if(!opened || snapshot.session != m_requestedSession)
        return;
    const auto pendings = std::exchange(m_pendingOnListed, {});
    for(const auto &pending : pendings) {
        if(snapshot.session != m_requestedSession)
            break;
        if(pending.dir == m_directoryPath)
            pending.action();
    }
}
