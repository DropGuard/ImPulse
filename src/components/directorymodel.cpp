#include "directorymodel.h"

#include "components/directorymanager/enginebridge.h"
#include "sourcecontainers/imagestatic.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSet>

namespace {
QByteArray sourceDigest(const QString &path) {
    if(!QFileInfo(path).isFile())
        return {};
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray();
}
} // namespace

DirectoryModel::DirectoryModel(WorkerPool &pool, EngineBridge &bridge, QObject *parent)
    : QObject(parent), scaler(std::make_unique<Scaler>(&m_cache, pool)), m_dirManager(bridge), m_loader(pool),
      m_draftStore(pool) {
    connect(&m_dirManager, &DirectoryManager::loaded, this, &DirectoryModel::onListingOpened);
    connect(&m_dirManager, &DirectoryManager::filesChanged, this, &DirectoryModel::onFilesChanged);
    connect(&m_dirManager, &DirectoryManager::sortingChanged, this, &DirectoryModel::onSortingChanged);
    connect(&m_draftStore, &EditDraftStore::writeFailed, this, &DirectoryModel::editDraftWriteFailed);
    connect(&m_loader, &Loader::loadFinished, this, &DirectoryModel::onImageReady);
    connect(&m_loader, &Loader::loadFailed, this, &DirectoryModel::loadFailed);
}

DirectoryModel::~DirectoryModel() {
    m_loader.clearTasks();
}

// -----------------------------------------------------------------------------

void DirectoryModel::removeFile(const QString &filePath, bool trash, FileOpResult &result) {
    // no mirror write here: the engine's watch sees the change and the
    // The listing follows the watcher; the file operation does not edit it.
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

void DirectoryModel::unload(int index) {
    QString filePath = m_dirManager.filePathAt(index);
    m_cache.remove(filePath);
}

void DirectoryModel::unload(const QString &filePath) {
    m_cache.remove(filePath);
}

void DirectoryModel::unloadExcept(const QString &filePath, bool keepNearby) {
    QList<QString> list;
    list << filePath;
    if(keepNearby) {
        list << m_dirManager.prevOfFile(filePath);
        list << m_dirManager.nextOfFile(filePath);
    }
    m_cache.trimTo(list);
}

bool DirectoryModel::loaderBusy() const {
    return m_loader.isBusy();
}

void DirectoryModel::onImageReady(std::shared_ptr<Image> img, const QString &path, bool scaled) {
    if(editedImageAt(path))
        return;
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

DirectoryModel::SaveResult DirectoryModel::saveFile(const QString &filePath, const QString &destPath) {
    const QUuid id = editAt(filePath);
    if(id.isNull()) {
        if(!m_dirManager.containsFile(filePath))
            return SaveResult::Failed;
        auto image = m_cache.get(filePath);
        return image && image->save(destPath, settings->JPEGSaveQuality()) ? SaveResult::Saved : SaveResult::Failed;
    }

    const auto draft = m_edits.value(id);
    const auto pixels = draft.image->getImage();
    const auto sourceUnchanged = [&draft] {
        return sourceDigest(draft.sourcePath) == draft.sourceDigest;
    };
    const bool replacingSource = QFileInfo(filePath) == QFileInfo(destPath);
    bool saved = false;
    if(replacingSource) {
        bool conflict = false;
        saved = FileOperations::saveWithBackup(*pixels, destPath, settings->JPEGSaveQuality(), filePath, [&] {
            conflict = !sourceUnchanged();
            return !conflict;
        });
        if(conflict)
            return SaveResult::SourceChanged;
    } else if(sourceUnchanged()) {
        saved = FileOperations::saveWithBackup(*pixels, destPath, settings->JPEGSaveQuality(), filePath);
    } else {
        saved = EditDraftStore::exportNew(*pixels, destPath);
        if(!saved)
            return SaveResult::SourceChanged;
    }
    if(!saved)
        return SaveResult::Failed;
    finishEdit(id);
    if(replacingSource) {
        unload(filePath);
        load(filePath, false);
    } else {
        emit imageUpdated(filePath);
    }
    return SaveResult::Saved;
}

// dirManager events

void DirectoryModel::onSortingChanged() {
    emit sortingChanged(m_dirManager.appliedSortingMode());
}

void DirectoryModel::onListingOpened() {
    bool detached = false;
    for(auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        if(it->entryId != 0 && editAt(it->sourcePath) != it.key()) {
            it->entryId = 0;
            detached = true;
        }
    }
    if(detached)
        emit editDraftDetached();
}

void DirectoryModel::onFilesChanged(const ListingFileChanges &changes) {
    bool detached = false;
    for(const auto &file : changes.displaced) {
        unload(file.oldPath);
        m_announcedDivergences.remove(file.oldPath);
    }
    for(const auto &file : changes.displaced) {
        for(auto it = m_edits.begin(); it != m_edits.end(); ++it) {
            if(it->entryId != file.id)
                continue;
            if(file.newPath.isEmpty()) {
                it->entryId = 0;
                detached = true;
            } else {
                const auto pixels = it->image->getImage();
                it->sourcePath = file.newPath;
                it->image = it->image->editedCopy(file.newPath, pixels);
                m_draftStore.write(it.key(), file.newPath, pixels);
            }
        }
    }
    if(detached)
        emit editDraftDetached();
    for(const auto &path : changes.contentChanged)
        onFileModified(path);
    emit filesReconciled(changes);
}

void DirectoryModel::onFileModified(const QString &filePath) {
    QDateTime modTime = m_dirManager.lastModified(filePath);
    if(!modTime.isValid())
        return;
    std::shared_ptr<Image> img = editedImageAt(filePath);
    if(!img)
        img = m_cache.get(filePath);
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

bool DirectoryModel::isLoaded(int index) const {
    return isLoaded(m_dirManager.filePathAt(index));
}

bool DirectoryModel::isLoaded(const QString &filePath) const {
    return m_cache.contains(filePath) || !editAt(filePath).isNull();
}

std::shared_ptr<Image> DirectoryModel::getImageAt(int index) {
    return getImage(m_dirManager.filePathAt(index));
}

// returns cached image
// if image is not cached, loads it in the main thread
// for async access use loadAsync(), then catch onImageReady()
std::shared_ptr<Image> DirectoryModel::getImage(const QString &filePath) {
    if(auto edited = editedImageAt(filePath))
        return edited;
    std::shared_ptr<Image> img = m_cache.get(filePath);
    if(img)
        return img;
    // the one place absence is flattened back into a null pointer, because the
    // callers of getImage() hand it straight to sinks that test for it
    // (guiSetImage, getMimeDataForImage). Everything upstream of this line
    // carries std::optional and cannot forward the question unknowingly.
    return m_loader.load(filePath, LoadPolicy::fromSettings()).value_or(nullptr);
}

QUuid DirectoryModel::editAt(const QString &path) const {
    for(auto it = m_edits.cbegin(); it != m_edits.cend(); ++it) {
        if(it->entryId != 0 && it->sourcePath == path &&
           m_dirManager.fileEntryAt(m_dirManager.indexOfFile(path)).id == it->entryId)
            return it.key();
    }
    return {};
}

std::shared_ptr<ImageStatic> DirectoryModel::editedImageAt(const QString &path) const {
    const auto it = m_edits.constFind(editAt(path));
    return it != m_edits.cend() ? it->image : nullptr;
}

bool DirectoryModel::editImage(const QString &filePath, const EditTransform &transform) {
    const quint64 entryId = m_dirManager.fileEntryAt(m_dirManager.indexOfFile(filePath)).id;
    if(entryId == 0)
        return false;
    QUuid id = editAt(filePath);
    EditDraft draft = m_edits.value(id);
    if(id.isNull()) {
        draft.entryId = entryId;
        draft.sourcePath = filePath;
        draft.sourceDigest = sourceDigest(filePath);
        if(draft.sourceDigest.isEmpty())
            return false;
        auto original = m_loader.load(filePath, LoadPolicy::fromSettings()).value_or(nullptr);
        draft.image = std::dynamic_pointer_cast<ImageStatic>(original);
        if(!draft.image || sourceDigest(filePath) != draft.sourceDigest)
            return false;
        id = QUuid::createUuid();
    }
    const auto input = draft.image->getImage();
    if(!input || input->isNull())
        return false;
    std::shared_ptr<const QImage> pixels = transform(input);
    if(!pixels || pixels->isNull())
        return false;
    draft.image = draft.image->editedCopy(filePath, pixels);
    m_edits.insert(id, std::move(draft));
    m_draftStore.write(id, filePath, pixels);
    emit imageUpdated(filePath);
    return true;
}

void DirectoryModel::finishEdit(const QUuid &id) {
    m_edits.remove(id);
    m_draftStore.remove(id);
}

void DirectoryModel::discardEdit(const QString &filePath) {
    const auto id = editAt(filePath);
    if(!id.isNull()) {
        finishEdit(id);
        unload(filePath);
        m_scaledCache.remove(filePath);
        emit imageUpdated(filePath);
    }
}

QList<RecoverableEdit> DirectoryModel::recoverableEdits() const {
    auto edits = m_draftStore.recoverable();
    edits.removeIf([this](const auto &edit) {
        return m_edits.contains(edit.id);
    });
    for(auto it = m_edits.cbegin(); it != m_edits.cend(); ++it) {
        if(it->entryId == 0)
            edits.append({it.key(), it->sourcePath});
    }
    return edits;
}

bool DirectoryModel::exportRecoveredEdit(const QUuid &id, const QString &destPath) {
    const auto it = m_edits.constFind(id);
    if(it != m_edits.cend() && it->entryId != 0)
        return false;
    const QImage pixels = it == m_edits.cend() ? m_draftStore.read(id) : *it->image->getImage();
    if(pixels.isNull() || !EditDraftStore::exportNew(pixels, destPath))
        return false;
    finishEdit(id);
    return true;
}

void DirectoryModel::load(const QString &filePath, bool asyncHint) {
    load(filePath, asyncHint, true); // tier-1 by default
}

void DirectoryModel::load(const QString &filePath, bool asyncHint, bool scaled) {
    if(auto edited = editedImageAt(filePath)) {
        emit imageReady(edited, filePath, false);
        return;
    }
    if(!m_dirManager.containsFile(filePath) || m_loader.isLoading(filePath))
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
    if(auto edited = editedImageAt(filePath)) {
        emit imageReady(edited, filePath, false);
        return;
    }
    if(!m_dirManager.containsFile(filePath))
        return;
    QSet<QString> window;
    window.insert(filePath);
    QString neighbor;
    if(withPreload) {
        neighbor = forward ? m_dirManager.nextOfFile(filePath) : m_dirManager.prevOfFile(filePath);
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
    if(m_dirManager.containsFile(filePath) && !m_cache.contains(filePath))
        m_loader.loadAsync(filePath, LoadPolicy::fromSettings()); // tier-1 scaled
}

// tier-2: full-res re-decode of a tier-1 cached image
void DirectoryModel::upgradeToFull(const QString &filePath) {
    if(m_dirManager.containsFile(filePath) && !m_loader.isLoading(filePath) && m_scaledCache.contains(filePath))
        m_loader.loadAsyncFull(filePath, LoadPolicy::fromSettings());
}
