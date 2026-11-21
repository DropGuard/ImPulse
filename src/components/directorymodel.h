#pragma once

#include <QHash>
#include <QObject>
#include "cache/cache.h"
#include "directorymanager/directorymanager.h"
#include "scaler/scaler.h"
#include "loader/loader.h"
#include "utils/fileoperations.h"

class DirectoryModel : public QObject {
    Q_OBJECT
  public:
    // The bridge is required, not defaulted: the model drives whichever engine
    // it is given, and reaching for the process-wide instance here made every
    // Core built with an injected bridge silently drive the global one instead.
    // The rule is the one Thumbnailer already follows (pool + bridge named at
    // the construction site).
    DirectoryModel(WorkerPool &pool, EngineBridge &bridge, QObject *parent = nullptr);
    ~DirectoryModel() override;

    // public: Core connects its scalingFinished and drives requests through it
    std::unique_ptr<Scaler> scaler;

    // The manager that owns the listing. The model's own surface is the image
    // side — cache, loader, divergence — and everything the manager decides is
    // reached through here rather than re-declared query by query: a
    // pass-through layer must be edited in triplicate and hides which layer
    // decides. The manager is also the object to connect for directory events
    // (dirAdded/dirRemoved/dirRenamed/loaded).
    DirectoryManager &listing() {
        return m_dirManager;
    }
    const DirectoryManager &listing() const {
        return m_dirManager;
    }

    void load(const QString &filePath, bool asyncHint);
    void load(const QString &filePath, bool asyncHint, bool scaled);
    // The flip: one drain pass maintains the flight window — the target
    // promotes to priority 2, the direction neighbor survives at 0, everything
    // else queued dies (queued jobs have not started, so dropping them costs
    // nothing decoded) — and the display follows load()'s rules: a cache hit
    // shows instantly, a queued or running decode delivers when it lands.
    void loadFocused(const QString &filePath, bool forward, bool withPreload);
    void preload(const QString &filePath);
    void upgradeToFull(const QString &filePath);

    void copyFileTo(const QString &srcFile, const QString &destDirPath, bool force, FileOpResult &result);
    void moveFileTo(const QString &srcFile, const QString &destDirPath, bool force, FileOpResult &result);
    void renameEntry(const QString &oldFilePath, const QString &newName, bool force, FileOpResult &result);
    void removeFile(const QString &filePath, bool trash, FileOpResult &result);
    void removeDir(const QString &dirPath, bool trash, bool recursive, FileOpResult &result);

    bool setDirectory(const QString &);

    void unload(int index);

    bool loaderBusy() const;

    std::shared_ptr<Image> getImageAt(int index);
    std::shared_ptr<Image> getImage(const QString &filePath);

    void updateImage(const QString &filePath, std::shared_ptr<Image> img);

    void unload(const QString &filePath);
    bool isLoaded(int index) const;
    bool isLoaded(const QString &filePath) const;
    void reload(const QString &filePath);
    void unloadExcept(const QString &filePath, bool keepNearby);

    bool saveFile(const QString &filePath);
    bool saveFile(const QString &filePath, const QString &destPath);
  signals:
    void fileRemoved(const QString &filePath, int index);
    void fileAdded(const QString &filePath);
    void fileModified(const QString &filePath);
    void divergenceDetected(const QString &filePath);
    // EDGE events. fileModified fires when this model observed a real change
    // (reload, or a mirror report for an entry it holds nothing about);
    // divergenceDetected fires once per (path, disk mtime) when the disk
    // moved under an entry holding uncommitted edits. The dedup lives here
    // because the mtime comparison lives here.
    void fileRenamed(const QString &filePath, const QString &newPath, int oldIndex);
    void loadFailed(const QString &path);
    void sortingChanged(SortingMode);
    void imageReady(std::shared_ptr<Image> img, const QString &, bool scaled);
    void imageUpdated(const QString &filePath);

  private:
    DirectoryManager m_dirManager;
    Loader m_loader;
    QSet<QString> m_scaledCache; // paths whose cached image is tier-1 (scaled)
    Cache m_cache;

    // the disk mtime the last divergence announcement was made for, per path:
    // one external save surfaces as several wire/mirror cycles, and each must
    // not re-announce. Keyed by path and overwritten per divergence — bounded
    // by the listing, stale entries are harmless (mtimes only move forward).
    QHash<QString, QDateTime> m_announcedDivergences;

  private slots:
    void onImageReady(std::shared_ptr<Image> img, const QString &path, bool scaled);
    void onSortingChanged();
    void onFileAdded(const QString &filePath);
    void onFileRemoved(const QString &filePath, int index);
    void onFileModified(const QString &filePath);
    void onFileRenamed(const QString &filePath, const QString &newPath, int oldIndex);
};
