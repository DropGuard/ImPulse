#pragma once

#include <QDebug>
#include <QMap>
#include <QSemaphore>
#include <QMutexLocker>
#include "sourcecontainers/image.h"
#include "components/cache/cacheitem.h"
#include "utils/imagefactory.h"

class Cache {
  public:
    explicit Cache();
    ~Cache();
    bool contains(const QString &path) const;
    void remove(const QString &path);
    void clear();

    bool insert(std::shared_ptr<Image> img);
    void trimTo(const QStringList &list);

    std::shared_ptr<Image> get(const QString &path);
    bool release(const QString &path);
    bool reserve(const QString &path);
    // test-only: the cache's contents as keys, so a suite can assert what a
    // duplicate insert or a trim actually stored. Production reads through get.
    const QList<QString> keys() const;

  private:
    QMap<QString, CacheItem *> m_items;
};
