#include "directorypresenter.h"

#include <QHash>
#include <QSet>
#include <utility>

DirectoryPresenter::DirectoryPresenter(Thumbnailer &_thumbnailer, QObject *parent)
    : QObject(parent), m_thumbnailer(_thumbnailer), m_showDirs(false) {
    connect(&m_thumbnailer, &Thumbnailer::thumbnailReady, this, &DirectoryPresenter::onThumbnailReady);
}

void DirectoryPresenter::unsetModel() {
    if(m_model)
        disconnect(&m_model->listing(), &DirectoryManager::listingChanged, this, &DirectoryPresenter::syncRows);
    m_rows.clear();
    m_model = nullptr;
    // also empty view?
}

void DirectoryPresenter::setModel(std::shared_ptr<DirectoryModel> newModel) {
    if(m_model)
        unsetModel();
    if(!newModel)
        return;
    m_model = newModel;
    populateView();

    connect(&m_model->listing(), &DirectoryManager::listingChanged, this, &DirectoryPresenter::syncRows);
}

void DirectoryPresenter::populateView() {
    m_rows.clear();
    syncRows();
}

void DirectoryPresenter::disconnectView() {
    if(!m_view)
        return;
    // the QPointer is the liveness test: a destroyed widget has already had
    // Qt drop its connections, and a null sender passed to disconnect()
    // would mean "every sender to this receiver" — the model wiring included
    // — so the severing happens only while the widget is alive.
    if(m_viewSender)
        QObject::disconnect(m_viewSender, nullptr, this, nullptr);
    m_rows.clear();
    m_viewSender = nullptr;
    m_view = nullptr;
}

//------------------------------------------------------------------------------

void DirectoryPresenter::syncRows() {
    if(!m_model || !m_viewSender)
        return;
    const auto &listing = m_model->listing();
    QHash<quint64, int> previous;
    QSet<QString> previousPaths;
    previous.reserve(m_rows.size());
    previousPaths.reserve(m_rows.size());
    for(int index = 0; index < m_rows.size(); ++index) {
        const auto &row = m_rows[index];
        previous.insert(row.id, index);
        previousPaths.insert(row.path);
    }
    QList<ViewEntry> next;
    QList<DirectoryViewRow> plan;
    const int dirs = m_showDirs ? int(listing.dirCount()) : 0;
    const int count = dirs + int(listing.fileCount());
    next.reserve(count);
    plan.reserve(count);
    for(int index = 0; index < count; ++index) {
        const auto &entry = index < dirs ? listing.dirEntryAt(index) : listing.fileEntryAt(index - dirs);
        const int oldIndex = previous.value(entry.id, -1);
        DirectoryViewRow row;
        row.previousIndex = oldIndex;
        if(oldIndex >= 0) {
            const auto &old = m_rows[oldIndex];
            if(old.name != entry.name)
                row.newName = entry.name;
            row.contentChanged = old.contentRevision != entry.contentRevision;
        } else {
            row.contentChanged = !entry.isDirectory && previousPaths.contains(entry.path);
        }
        plan.append(std::move(row));
        next.append(ViewEntry{entry.id, entry.name, entry.path, entry.contentRevision});
    }
    const bool firstRows = m_rows.isEmpty() && !next.isEmpty();
    m_rows = std::move(next);
    m_view->applyRows(plan);
    if(firstRows)
        selectAndFocus(0);
}

bool DirectoryPresenter::showDirs() {
    return m_showDirs;
}

void DirectoryPresenter::setShowDirs(bool mode) {
    if(mode == m_showDirs)
        return;
    m_showDirs = mode;
    syncRows();
}

QList<QString> DirectoryPresenter::selectedPaths() const {
    QList<QString> paths;
    if(!m_view)
        return paths;
    // const local: iterating a by-value temporary directly would detach it
    const QList<int> selection = m_view->selection();
    if(m_showDirs) {
        for(const int i : selection) {
            if(i < static_cast<int>(m_model->listing().dirCount()))
                paths << m_model->listing().dirPathAt(i);
            else
                paths << m_model->listing().filePathAt(i - m_model->listing().dirCount());
        }
    } else {
        for(const int i : selection) {
            paths << m_model->listing().filePathAt(i);
        }
    }
    return paths;
}

void DirectoryPresenter::generateThumbnails(const QList<int> &indexes, int size, bool crop, bool force) {
    if(!m_view || !m_model)
        return;
    m_thumbnailer.clearTasks();
    if(!m_showDirs) {
        for(int i : indexes)
            m_thumbnailer.getThumbnailAsync(m_model->listing().filePathAt(i), size, crop, force);
        return;
    }
    for(int i : indexes) {
        if(i < static_cast<int>(m_model->listing().dirCount())) {
            // tmp ------------------------------------------------------------
            // gen thumb for a directory
            // TODO: optimize & move dir icon loading to shared res; then overlay
            // the mini-thumbs on top (similar to dolphin)
            QSvgRenderer svgRenderer;
            svgRenderer.load(QString(":/res/icons/common/other/folder32-scalable.svg"));
            int factor = (size * 0.90f) / svgRenderer.defaultSize().width();
            QImage *image = new QImage(svgRenderer.defaultSize() * factor, QImage::Format_ARGB32);
            image->fill(Qt::transparent);
            QPainter pixPainter(image);
            svgRenderer.render(&pixPainter);
            pixPainter.end();

            ImageLib::recolor(*image, settings->colorScheme().icons);

            std::shared_ptr<Thumbnail> thumb(
                new Thumbnail(m_model->listing().dirNameAt(i), "Folder", size, std::shared_ptr<QImage>(image)));
            // ^----------------------------------------------------------------
            m_view->setThumbnail(i, thumb);
        } else {
            QString path = m_model->listing().filePathAt(i - m_model->listing().dirCount());
            m_thumbnailer.getThumbnailAsync(path, size, crop, force);
        }
    }
}

void DirectoryPresenter::onThumbnailReady(std::shared_ptr<Thumbnail> thumb, const QString &filePath) {
    if(!m_view || !m_model)
        return;
    int index = m_model->listing().indexOfFile(filePath);
    if(index == -1)
        return;
    m_view->setThumbnail(m_showDirs ? m_model->listing().dirCount() + index : index, thumb);
}

void DirectoryPresenter::onItemActivated(int absoluteIndex) {
    if(!m_model)
        return;
    if(!m_showDirs) {
        emit fileActivated(m_model->listing().filePathAt(absoluteIndex));
        return;
    }
    if(absoluteIndex < static_cast<int>(m_model->listing().dirCount()))
        emit dirActivated(m_model->listing().dirPathAt(absoluteIndex));
    else
        emit fileActivated(m_model->listing().filePathAt(absoluteIndex - m_model->listing().dirCount()));
}

void DirectoryPresenter::onDraggedOut() {
    emit draggedOut(selectedPaths());
}

void DirectoryPresenter::onDraggedOver(int index) {
    if(!m_model || m_view->selection().contains(index))
        return;
    if(showDirs() && index < static_cast<int>(m_model->listing().dirCount()))
        m_view->setDragHover(index);
}

void DirectoryPresenter::onDroppedInto(const QMimeData *data, QObject *source, int targetIndex) {
    if(!data->hasUrls())
        return;

    // ignore drops into selected / current folder when we are the source of dropEvent
    if(source && (m_view->selection().contains(targetIndex) || targetIndex == -1))
        return;
    // ignore drops into a file
    // todo: drop into a current dir when target is a file
    if(showDirs() && targetIndex >= static_cast<int>(m_model->listing().dirCount()))
        return;

    // convert urls to qstrings
    QStringList pathList;
    QList<QUrl> urlList = data->urls();
    for(int i = 0; i < urlList.size(); ++i)
        pathList.append(urlList.at(i).toLocalFile());

    // get target dir path
    QString destDir;
    if(showDirs() && targetIndex < static_cast<int>(m_model->listing().dirCount()))
        destDir = m_model->listing().dirPathAt(targetIndex);
    if(destDir.isEmpty()) // fallback to the current dir
        destDir = m_model->listing().directoryPath();
    pathList.removeAll(destDir); // remove target dir from source list

    // pass to core
    emit droppedInto(pathList, destDir);
}

void DirectoryPresenter::selectAndFocus(const QString &path) {
    if(!m_model || !m_view || path.isEmpty())
        return;
    if(m_model->listing().containsDir(path) && showDirs()) {
        int dirIndex = m_model->listing().indexOfDir(path);
        m_view->select(dirIndex);
        m_view->focusOn(dirIndex);
    } else if(m_model->listing().containsFile(path)) {
        int fileIndex = showDirs() ? m_model->listing().indexOfFile(path) + m_model->listing().dirCount()
                                   : m_model->listing().indexOfFile(path);
        m_view->select(fileIndex);
        m_view->focusOn(fileIndex);
    }
}

void DirectoryPresenter::selectAndFocus(int absoluteIndex) {
    if(!m_model || !m_view)
        return;
    m_view->select(absoluteIndex);
    m_view->focusOn(absoluteIndex);
}
