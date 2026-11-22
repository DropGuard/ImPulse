#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QUuid>
#include <memory>
#include <mutex>

class WorkerPool;

struct RecoverableEdit {
    QUuid id;
    QString sourcePath;
};

class EditDraftStore : public QObject {
    Q_OBJECT
  public:
    explicit EditDraftStore(WorkerPool &pool);
    ~EditDraftStore() override;

    void write(const QUuid &id, const QString &sourcePath, std::shared_ptr<const QImage> pixels);
    void remove(const QUuid &id);
    bool flush();
    QList<RecoverableEdit> recoverable() const;
    QImage read(const QUuid &id) const;
    static bool exportNew(const QImage &pixels, const QString &destPath);

  signals:
    void writeFailed();

  private:
    class Writer;
    struct Change {
        QString sourcePath;
        std::shared_ptr<const QImage> pixels;
    };

    void enqueue(const QUuid &id, Change change);
    void drain();
    bool apply(const QUuid &id, const Change &change) const;
    QString pathOf(const QUuid &id) const;

    WorkerPool &m_pool;
    const QString m_dir;
    std::mutex m_mutex;
    QHash<QUuid, Change> m_pending;
    QSet<QUuid> m_failed;
    bool m_writing = false;
};
