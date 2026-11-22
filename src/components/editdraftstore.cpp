#include "editdraftstore.h"

#include "workerpool/workerpool.h"

#include <QDir>
#include <QImageReader>
#include <QImageWriter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <utility>

namespace {
// PNG text chunks carry only the keys Qt maps onto the standard PNG ones —
// Description, Title, Author and friends. A custom key is accepted by the
// writer and then silently absent from the file: measured, setText("ImPulse
// draft source", path) round-trips as an empty string while the same call on
// "Description" returns the value. So the draft rides Description, and the
// store's own key is a prefix inside the value rather than the key itself.
constexpr char kDraftTextKey[] = "Description";
constexpr char kDraftPrefix[] = "impulse-draft:";

QString draftDirectory() {
    const QString override = qEnvironmentVariable("QIMGV_DRAFT_DIR");
    if(!override.isEmpty())
        return QFileInfo(override).absoluteFilePath();
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("edit-drafts"));
}
} // namespace

class EditDraftStore::Writer : public PoolJob {
  public:
    explicit Writer(EditDraftStore &store) : m_store(store) {
    }
    void run() override {
        m_store.drain();
    }

  private:
    EditDraftStore &m_store;
};

EditDraftStore::EditDraftStore(WorkerPool &pool) : m_pool(pool), m_dir(draftDirectory()) {
}

EditDraftStore::~EditDraftStore() {
    m_pool.waitForOwner(PoolOwner::EditDrafts);
}

QString EditDraftStore::pathOf(const QUuid &id) const {
    return QDir(m_dir).filePath(id.toString(QUuid::WithoutBraces) + QStringLiteral(".png"));
}

void EditDraftStore::write(const QUuid &id, const QString &sourcePath, std::shared_ptr<const QImage> pixels) {
    enqueue(id, {sourcePath, std::move(pixels)});
}

void EditDraftStore::remove(const QUuid &id) {
    enqueue(id, {});
}

void EditDraftStore::enqueue(const QUuid &id, Change change) {
    std::lock_guard lock(m_mutex);
    m_pending.insert(id, std::move(change));
    if(!m_writing) {
        m_writing = true;
        m_pool.submit(PoolOwner::EditDrafts, 0, std::make_unique<Writer>(*this));
    }
}

void EditDraftStore::drain() {
    for(;;) {
        QUuid id;
        Change change;
        {
            std::lock_guard lock(m_mutex);
            if(m_pending.isEmpty()) {
                m_writing = false;
                return;
            }
            auto next = m_pending.begin();
            id = next.key();
            change = std::move(next.value());
            m_pending.erase(next);
        }
        const bool success = apply(id, change);
        {
            std::lock_guard lock(m_mutex);
            if(success)
                m_failed.remove(id);
            else
                m_failed.insert(id);
        }
        if(!success)
            QMetaObject::invokeMethod(this, &EditDraftStore::writeFailed, Qt::QueuedConnection);
    }
}

bool EditDraftStore::apply(const QUuid &id, const Change &change) const {
    if(m_dir.isEmpty())
        return false;
    const QString path = pathOf(id);
    if(!change.pixels)
        return !QFile::exists(path) || QFile::remove(path);
    if(!QDir().mkpath(m_dir))
        return false;
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
    QImageWriter writer(&file, "png");
    writer.setText(QString::fromLatin1(kDraftTextKey), QString::fromLatin1(kDraftPrefix) + change.sourcePath);
    return writer.write(*change.pixels) && file.commit();
}

bool EditDraftStore::flush() {
    m_pool.waitForOwner(PoolOwner::EditDrafts);
    std::lock_guard lock(m_mutex);
    return m_failed.isEmpty();
}

QList<RecoverableEdit> EditDraftStore::recoverable() const {
    QList<RecoverableEdit> result;
    if(m_dir.isEmpty())
        return result;
    const auto files = QDir(m_dir).entryInfoList({QStringLiteral("*.png")}, QDir::Files | QDir::NoSymLinks);
    for(const auto &file : files) {
        const QUuid id = QUuid::fromString(file.completeBaseName());
        if(!id.isNull()) {
            QImageReader reader(file.absoluteFilePath(), "png");
            const QString text = reader.text(QString::fromLatin1(kDraftTextKey));
            // a Description this store did not write is not a draft: the PNG
            // may carry someone else's comment, and offering that as a
            // recoverable source path would be a lie
            if(text.startsWith(QString::fromLatin1(kDraftPrefix)))
                result.append({id, text.mid(int(sizeof(kDraftPrefix) - 1))});
        }
    }
    return result;
}

QImage EditDraftStore::read(const QUuid &id) const {
    return m_dir.isEmpty() || id.isNull() ? QImage() : QImage(pathOf(id), "png");
}

bool EditDraftStore::exportNew(const QImage &pixels, const QString &destPath) {
    const QFileInfo dest(destPath);
    if(dest.suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0 || dest.exists() || dest.isSymLink())
        return false;
    QTemporaryFile file(dest.absoluteDir().filePath(QStringLiteral(".impulse-edit-XXXXXX")));
    if(!file.open())
        return false;
    QImage exported = pixels;
    exported.setText(QString::fromLatin1(kDraftTextKey), QString());
    if(!exported.save(&file, "png") || !file.flush())
        return false;
    file.close();
    if(!file.rename(dest.absoluteFilePath()))
        return false;
    file.setAutoRemove(false);
    return true;
}
