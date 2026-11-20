#include "directorymodel.h"

#include "components/directorymanager/enginebridge.h"

#include <QSet>

DirectoryModel::DirectoryModel(WorkerPool &pool, EngineBridge &bridge, QObject *parent)
    : QObject(parent), m_dirManager(bridge), m_loader(pool) {
    scaler = new Scaler(&m_cache, pool);

    connect(&m_dirManager, &DirectoryManager::fileRemoved, this, &DirectoryModel::onFileRemoved);
    connect(&m_dirManager, &DirectoryManager::fileAdded, this, &DirectoryModel::onFileAdded);
    connect(&m_dirManager, &DirectoryManager::fileModified, this, &DirectoryModel::onFileModified);
    connect(&m_dirManager, &DirectoryManager::fileRenamed, this, &DirectoryModel::onFileRenamed);
    connect(&m_dirManager, &DirectoryManager::dirRemoved, this, &DirectoryModel::dirRemoved);
    connect(&m_dirManager, &DirectoryManager::dirAdded, this, &DirectoryModel::dirAdded);
    connect(&m_dirManager, &DirectoryManager::dirRenamed, this, &DirectoryModel::dirRenamed);

    connect(&m_dirManager, &DirectoryManager::loaded, this, &DirectoryModel::loaded);
    connect(&m_dirManager, &DirectoryManager::sortingChanged, this, &DirectoryModel::onSortingChanged);
    connect(&m_loader, &Loader::loadFinished, this, &DirectoryModel::onImageReady);
    connect(&m_loader, &Loader::loadFailed, this, &DirectoryModel::loadFailed);
}

DirectoryModel::~DirectoryModel() {
    m_loader.clearTasks();
    delete scaler;
}

int DirectoryModel::totalCount() const {
    return m_dirManager.totalCount();
}

int DirectoryModel::fileCount() const {
    return m_dirManager.fileCount();
}

int DirectoryModel::dirCount() const {
    return m_dirManager.dirCount();
}

int DirectoryModel::indexOfFile(const QString &filePath) const {
    return m_dirManager.indexOfFile(filePath);
}

int DirectoryModel::indexOfDir(const QString &filePath) const {
    return m_dirManager.indexOfDir(filePath);
}

SortingMode DirectoryModel::sortingMode() const {
    return m_dirManager.sortingMode();
}

const FSEntry &DirectoryModel::fileEntryAt(int index) const {
    return m_dirManager.fileEntryAt(index);
}

QString DirectoryModel::fileNameAt(int index) const {
    return m_dirManager.fileNameAt(index);
}

QString DirectoryModel::filePathAt(int index) const {
    return m_dirManager.filePathAt(index);
}

QString DirectoryModel::dirNameAt(int index) const {
    return m_dirManager.dirNameAt(index);
}

QString DirectoryModel::dirPathAt(int index) const {
    return m_dirManager.dirPathAt(index);
}

QString DirectoryModel::directoryPath() const {
    return m_dirManager.directoryPath();
}

bool DirectoryModel::containsFile(const QString &filePath) const {
    return m_dirManager.containsFile(filePath);
}

bool DirectoryModel::containsDir(const QString &dirPath) const {
    return m_dirManager.containsDir(dirPath);
}

bool DirectoryModel::isEmpty() const {
    return m_dirManager.isEmpty();
}

QString DirectoryModel::firstFile() const {
    return m_dirManager.firstFile();
}

QString DirectoryModel::lastFile() const {
    return m_dirManager.lastFile();
}

QString DirectoryModel::nextOf(const QString &filePath) const {
    return m_dirManager.nextOfFile(filePath);
}

QString DirectoryModel::prevOf(const QString &filePath) const {
    return m_dirManager.prevOfFile(filePath);
}

QDateTime DirectoryModel::lastModified(const QString &filePath) const {
    return m_dirManager.lastModified(filePath);
}

// -----------------------------------------------------------------------------

QString DirectoryModel::adjacentDirectory(const QString &dirPath, bool forward) {
    return m_dirManager.adjacentDirectory(dirPath, forward, sortingMode());
}

void DirectoryModel::setSortingMode(SortingMode mode) {
    m_dirManager.setSortingMode(mode);
}

void DirectoryModel::removeFile(const QString &filePath, bool trash, FileOpResult &result) {
    // no mirror write here: the engine's watch sees the change and the
    // listing follows via fileRemoved — one writer, no reconciliation
    if(trash)
        FileOperations::moveToTrash(filePath, result);
    else
        FileOperations::removeFile(filePath, result);
}

void DirectoryModel::renameEntry(const QString &oldPath, const QString &newName, bool force, FileOpResult &result) {
    // the disk is the only thing touched here: the watcher pairs the move into
    // one Renamed event, so an in-app rename and an external one land on the
    // mirror through the same single-event path
    FileOperations::rename(oldPath, newName, force, result);
}

void DirectoryModel::removeDir(const QString &dirPath, bool trash, bool recursive, FileOpResult &result) {
    if(trash) {
        FileOperations::moveToTrash(dirPath, result);
    } else {
        FileOperations::removeDir(dirPath, recursive, result);
    }
    if(result != FileOpResult::SUCCESS)
        return;
}

void DirectoryModel::copyFileTo(const QString &srcFile, const QString &destDirPath, bool force, FileOpResult &result) {
    FileOperations::copyFileTo(srcFile, destDirPath, force, result);
}

void DirectoryModel::moveFileTo(const QString &srcFile, const QString &destDirPath, bool force, FileOpResult &result) {
    // moving out of the watched directory surfaces as a removal event; the
    // old processEvents() ordering hack died with the local write path
    FileOperations::moveFileTo(srcFile, destDirPath, force, result);
}
// -----------------------------------------------------------------------------
bool DirectoryModel::setDirectory(const QString &path) {
    m_cache.clear();
    return m_dirManager.setDirectory(path);
}

qint64 DirectoryModel::whenListed(const QString &dirPath, std::function<void()> action) {
    return m_dirManager.whenListed(dirPath, std::move(action));
}

bool DirectoryModel::cancelListed(qint64 token) {
    return m_dirManager.cancelListed(token);
}

void DirectoryModel::unload(int index) {
    QString filePath = this->filePathAt(index);
    m_cache.remove(filePath);
}

void DirectoryModel::unload(const QString &filePath) {
    m_cache.remove(filePath);
}

void DirectoryModel::unloadExcept(const QString &filePath, bool keepNearby) {
    QList<QString> list;
    list << filePath;
    if(keepNearby) {
        list << prevOf(filePath);
        list << nextOf(filePath);
    }
    m_cache.trimTo(list);
}

bool DirectoryModel::loaderBusy() const {
    return m_loader.isBusy();
}

bool DirectoryModel::listingSettled() const {
    return m_dirManager.isSettled();
}

void DirectoryModel::onImageReady(std::shared_ptr<Image> img, const QString &path, bool scaled) {
    if(!img) {
        emit loadFailed(path);
        return;
    }
    m_cache.remove(path);
    m_cache.insert(img);
    if(scaled)
        m_scaledCache.insert(path);
    else
        m_scaledCache.remove(path);
    emit imageReady(img, path, scaled);
}

bool DirectoryModel::saveFile(const QString &filePath) {
    return saveFile(filePath, filePath);
}

bool DirectoryModel::saveFile(const QString &filePath, const QString &destPath) {
    if(!containsFile(filePath) || !m_cache.contains(filePath))
        return false;
    auto img = m_cache.get(filePath);
    if(img->save(destPath, settings->JPEGSaveQuality())) {
        // listing changes (modified/new) surface through the engine's watch
        // and arrive via fileModified/fileAdded — same as any external edit
        return true;
    }
    return false;
}

// dirManager events

void DirectoryModel::onSortingChanged() {
    emit sortingChanged(sortingMode());
}

void DirectoryModel::onFileAdded(const QString &filePath) {
    emit fileAdded(filePath);
}

void DirectoryModel::onFileModified(const QString &filePath) {
    QDateTime modTime = lastModified(filePath);
    if(!modTime.isValid())
        return;
    auto img = m_cache.get(filePath);
    if(!img) {
        // nothing cached to compare against: the mirror saw it, pass it on
        emit fileModified(filePath);
        return;
    }
    if(modTime == img->lastModified())
        return; // a re-report of a state the entry already reflects — no edge
    // an edited image IS uncommitted work: the disk does not write over
    // memory, exactly as bare keys do not write over disk. reload() would
    // cache.remove() the user's edits away; the divergence is announced as
    // its own edge, once per (path, disk mtime).
    if(img->isEdited()) {
        if(m_announcedDivergences.value(filePath) != modTime) {
            m_announcedDivergences.insert(filePath, modTime);
            emit divergenceDetected(filePath);
        }
        return;
    }
    reload(filePath);
    m_announcedDivergences.remove(filePath); // any divergence is resolved
    emit fileModified(filePath);
}

void DirectoryModel::onFileRemoved(const QString &filePath, int index) {
    unload(filePath);
    emit fileRemoved(filePath, index);
}

// The bytes did not move, only the name they are reached by — but the image
// cache is keyed by path and has no rekey, so the old key is dropped rather
// than followed. That is the same memory behavior the remove+add pair had: the
// renamed file decodes again on the next request, and nothing stale waits
// under a path the listing no longer has.
void DirectoryModel::onFileRenamed(const QString &filePath, const QString &newPath, int oldIndex) {
    Q_UNUSED(oldIndex)
    unload(filePath);
    emit fileRenamed(filePath, newPath, oldIndex);
}

bool DirectoryModel::isLoaded(int index) const {
    return m_cache.contains(filePathAt(index));
}

bool DirectoryModel::isLoaded(const QString &filePath) const {
    return m_cache.contains(filePath);
}

std::shared_ptr<Image> DirectoryModel::getImageAt(int index) {
    return getImage(filePathAt(index));
}

// returns cached image
// if image is not cached, loads it in the main thread
// for async access use loadAsync(), then catch onImageReady()
std::shared_ptr<Image> DirectoryModel::getImage(const QString &filePath) {
    std::shared_ptr<Image> img = m_cache.get(filePath);
    if(img)
        return img;
    // the one place absence is flattened back into a null pointer, because the
    // callers of getImage() hand it straight to sinks that test for it
    // (guiSetImage, getMimeDataForImage). Everything upstream of this line
    // carries std::optional and cannot forward the question unknowingly.
    return m_loader.load(filePath, LoadPolicy::fromSettings()).value_or(nullptr);
}

void DirectoryModel::updateImage(const QString &filePath, std::shared_ptr<Image> img) {
    if(containsFile(filePath) /*& cache.contains(filePath)*/) {
        if(!m_cache.contains(filePath)) {
            m_cache.insert(img);
        } else {
            m_cache.insert(img);
            emit imageUpdated(filePath);
        }
    }
}

void DirectoryModel::load(const QString &filePath, bool asyncHint) {
    load(filePath, asyncHint, true); // tier-1 by default
}

void DirectoryModel::load(const QString &filePath, bool asyncHint, bool scaled) {
    if(!containsFile(filePath) || m_loader.isLoading(filePath))
        return;
    if(!m_cache.contains(filePath)) {
        if(asyncHint) {
            m_loader.loadAsyncPriority(filePath, LoadPolicy::fromSettings());
        } else {
            // sync path (editing): full quality
            auto img = m_loader.load(filePath, LoadPolicy::fromSettings());
            if(img) {
                m_cache.insert(*img);
                emit imageReady(*img, filePath, false);
            } else {
                emit loadFailed(filePath);
            }
        }
    } else {
        bool cachedScaled = m_scaledCache.contains(filePath);
        // instant display from cache; a tier-1 cached image upgrades to full
        if(cachedScaled && scaled)
            m_loader.loadAsyncFull(filePath, LoadPolicy::fromSettings());
        emit imageReady(m_cache.get(filePath), filePath, cachedScaled);
    }
}

void DirectoryModel::reload(const QString &filePath) {
    if(m_cache.contains(filePath)) {
        m_cache.remove(filePath);
        load(filePath, false);
    }
}

void DirectoryModel::loadFocused(const QString &filePath, bool forward, bool withPreload) {
    if(!containsFile(filePath))
        return;
    QSet<QString> window;
    window.insert(filePath);
    QString neighbor;
    if(withPreload) {
        neighbor = forward ? nextOf(filePath) : prevOf(filePath);
        if(!neighbor.isEmpty())
            window.insert(neighbor);
    }
    m_loader.loadWindow(filePath, window, LoadPolicy::fromSettings());
    // the direction neighbor no earlier flip submitted
    if(!neighbor.isEmpty())
        preload(neighbor);
    if(m_cache.contains(filePath)) {
        bool cachedScaled = m_scaledCache.contains(filePath);
        if(cachedScaled)
            m_loader.loadAsyncFull(filePath, LoadPolicy::fromSettings());
        emit imageReady(m_cache.get(filePath), filePath, cachedScaled);
    }
}

void DirectoryModel::preload(const QString &filePath) {
    if(containsFile(filePath) && !m_cache.contains(filePath))
        m_loader.loadAsync(filePath, LoadPolicy::fromSettings()); // tier-1 scaled
}

// tier-2: full-res re-decode of a tier-1 cached image
void DirectoryModel::upgradeToFull(const QString &filePath) {
    if(containsFile(filePath) && !m_loader.isLoading(filePath) && m_scaledCache.contains(filePath))
        m_loader.loadAsyncFull(filePath, LoadPolicy::fromSettings());
}
