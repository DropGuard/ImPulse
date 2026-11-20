#include "cacheitem.h"

CacheItem::CacheItem() : m_sem(1) {
}

CacheItem::CacheItem(std::shared_ptr<Image> _contents) : m_contents(std::move(_contents)), m_sem(1) {
}

std::shared_ptr<Image> CacheItem::getContents() {
    return m_contents;
}

void CacheItem::lock() {
    m_sem.acquire(1);
}

void CacheItem::unlock() {
    m_sem.release(1);
}

int CacheItem::lockStatus() {
    return m_sem.available();
}
