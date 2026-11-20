#include "cache.h"

Cache::Cache() {
}

// the map owns its items raw: without this, every entry alive at destruction
// leaks (caught by LSan on the first sanitizer run)
Cache::~Cache() {
    qDeleteAll(m_items);
}

bool Cache::contains(const QString &path) const {
    return m_items.contains(path);
}

bool Cache::insert(std::shared_ptr<Image> img) {
    if(!img)
        return false; // reporting success for a failed decode hides the failure
    if(m_items.contains(img->filePath()))
        return false;
    m_items.insert(img->filePath(), new CacheItem(img));
    return true;
}

void Cache::remove(const QString &path) {
    if(m_items.contains(path) && m_items[path]->lockStatus() != 0) {
        auto *item = m_items.take(path);
        delete item;
    }
}

void Cache::clear() {
    // A pinned item is skipped, not waited for. lock() would block this thread
    // on a permit that the scaler only releases from a slot queued on the SAME
    // thread — and since the scaler's work now arrives through a queued
    // connection, that is a deadlock rather than a wait. Skipping defers the
    // eviction by one pass: the item stays alive, unlocks later, and the next
    // clear or trim takes it.
    for(auto it = m_items.begin(); it != m_items.end();) {
        if(it.value()->lockStatus() == 0) {
            ++it;
            continue;
        }
        delete it.value();
        it = m_items.erase(it);
    }
}

std::shared_ptr<Image> Cache::get(const QString &path) {
    if(m_items.contains(path)) {
        CacheItem *item = m_items.value(path);
        return item->getContents();
    }
    return nullptr;
}

bool Cache::reserve(const QString &path) {
    if(m_items.contains(path)) {
        m_items[path]->lock();
        return true;
    }
    return false;
}

bool Cache::release(const QString &path) {
    // only unlock what is actually locked: a caller holding a stale key (an
    // item evicted and re-inserted under the same path, for instance) would
    // otherwise hand out an extra permit, and the item would then need two
    // acquires to be evicted — permanently pinned by arithmetic
    if(m_items.contains(path) && m_items[path]->lockStatus() == 0) {
        m_items[path]->unlock();
        return true;
    }
    return false;
}

// removes all items except the ones in list
void Cache::trimTo(const QStringList &pathList) {
    for(auto it = m_items.begin(); it != m_items.end();) {
        // keep what the caller listed, keep what is in use, evict the rest now
        if(!pathList.contains(it.key()) && it.value()->lockStatus() != 0) {
            delete it.value();
            it = m_items.erase(it);
        } else {
            ++it;
        }
    }
}

const QList<QString> Cache::keys() const {
    return m_items.keys();
}
