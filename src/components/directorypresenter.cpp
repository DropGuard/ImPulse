#include "directorypresenter.h"

#include <utility>

DirectoryPresenter::DirectoryPresenter(Thumbnailer &_thumbnailer, QObject *parent)
    : QObject(parent), m_thumbnailer(_thumbnailer), m_showDirs(false) {
    connect(&m_thumbnailer, &Thumbnailer::thumbnailReady, this, &DirectoryPresenter::onThumbnailReady);
}

void DirectoryPresenter::unsetModel() {
    disconnect(m_model.get(), &DirectoryModel::fileRemoved, this, &DirectoryPresenter::onFileRemoved);
    disconnect(m_model.get(), &DirectoryModel::fileAdded, this, &DirectoryPresenter::onFileAdded);
    disconnect(m_model.get(), &DirectoryModel::fileModified, this, &DirectoryPresenter::onFileModified);
    disconnect(m_model.get(), &DirectoryModel::fileRenamed, this, &DirectoryPresenter::onFileRenamed);
    disconnect(m_model.get(), &DirectoryModel::dirRemoved, this, &DirectoryPresenter::onDirRemoved);
    disconnect(m_model.get(), &DirectoryModel::dirAdded, this, &DirectoryPresenter::onDirAdded);
    disconnect(m_model.get(), &DirectoryModel::dirRenamed, this, &DirectoryPresenter::onDirRenamed);
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

    // filesystem changes
    connect(m_model.get(), &DirectoryModel::fileRemoved, this, &DirectoryPresenter::onFileRemoved);
    connect(m_model.get(), &DirectoryModel::fileAdded, this, &DirectoryPresenter::onFileAdded);
    connect(m_model.get(), &DirectoryModel::fileModified, this, &DirectoryPresenter::onFileModified);
    connect(m_model.get(), &DirectoryModel::fileRenamed, this, &DirectoryPresenter::onFileRenamed);
    connect(m_model.get(), &DirectoryModel::dirRemoved, this, &DirectoryPresenter::onDirRemoved);
    connect(m_model.get(), &DirectoryModel::dirAdded, this, &DirectoryPresenter::onDirAdded);
    connect(m_model.get(), &DirectoryModel::dirRenamed, this, &DirectoryPresenter::onDirRenamed);
}

void DirectoryPresenter::reloadModel() {
    populateView();
}

void DirectoryPresenter::populateView() {
    if(!m_model || !m_view)
        return;
    m_view->populate(m_showDirs ? m_model->totalCount() : m_model->fileCount());
    selectAndFocus(0);
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
    m_viewSender = nullptr;
    m_view = nullptr;
}

//------------------------------------------------------------------------------

void DirectoryPresenter::onFileRemoved(const QString &filePath, int index) {
    Q_UNUSED(filePath)
    if(!m_view)
        return;
    m_view->removeItem(m_showDirs ? index + m_model->dirCount() : index);
}

void DirectoryPresenter::onFileAdded(const QString &filePath) {
    if(!m_view)
        return;
    int index = m_model->indexOfFile(filePath);
    m_view->insertItem(m_showDirs ? m_model->dirCount() + index : index);
}

void DirectoryPresenter::onFileModified(const QString &filePath) {
    if(!m_view)
        return;
    int index = m_model->indexOfFile(filePath);
    m_view->reloadItem(m_showDirs ? m_model->dirCount() + index : index);
}

// The row a rename moves is identified by the index the *event* carries, not by
// a lookup: the mirror has already spliced, so the old path is gone from the
// model by the time this runs. The destination index, on the other hand, is the
// model's own answer — under name sort a rename repositions, and only the
// listing knows where the entry ended up.
void DirectoryPresenter::onFileRenamed(const QString &filePath, const QString &newPath, int oldIndex) {
    Q_UNUSED(filePath)
    if(!m_view)
        return;
    int index = m_model->indexOfFile(newPath);
    if(index < 0)
        return;
    m_view->renameItem(m_showDirs ? m_model->dirCount() + oldIndex : oldIndex,
                       m_showDirs ? m_model->dirCount() + index : index, m_model->fileNameAt(index));
}

void DirectoryPresenter::onDirRemoved(const QString &dirPath, int index) {
    Q_UNUSED(dirPath)
    if(!m_view || !m_showDirs)
        return;
    m_view->removeItem(index);
}

void DirectoryPresenter::onDirAdded(const QString &dirPath) {
    if(!m_view || !m_showDirs)
        return;
    int index = m_model->indexOfDir(dirPath);
    m_view->insertItem(index);
}

// Dirs hold the leading rows, so no offset on either side; the same rule as
// onFileRenamed applies to the indices — the old row comes from the event, the
// new one from the listing.
void DirectoryPresenter::onDirRenamed(const QString &dirPath, const QString &newPath, int oldIndex) {
    Q_UNUSED(dirPath)
    if(!m_view || !m_showDirs)
        return;
    int index = m_model->indexOfDir(newPath);
    if(index < 0)
        return;
    m_view->renameItem(oldIndex, index, m_model->dirNameAt(index));
}

bool DirectoryPresenter::showDirs() {
    return m_showDirs;
}

void DirectoryPresenter::setShowDirs(bool mode) {
    if(mode == m_showDirs)
        return;
    m_showDirs = mode;
    populateView();
}

QList<QString> DirectoryPresenter::selectedPaths() const {
    QList<QString> paths;
    if(!m_view)
        return paths;
    // const local: iterating a by-value temporary directly would detach it
    const QList<int> selection = m_view->selection();
    if(m_showDirs) {
        for(const int i : selection) {
            if(i < m_model->dirCount())
                paths << m_model->dirPathAt(i);
            else
                paths << m_model->filePathAt(i - m_model->dirCount());
        }
    } else {
        for(const int i : selection) {
            paths << m_model->filePathAt(i);
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
            m_thumbnailer.getThumbnailAsync(m_model->filePathAt(i), size, crop, force);
        return;
    }
    for(int i : indexes) {
        if(i < m_model->dirCount()) {
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
                new Thumbnail(m_model->dirNameAt(i), "Folder", size, std::shared_ptr<QImage>(image)));
            // ^----------------------------------------------------------------
            m_view->setThumbnail(i, thumb);
        } else {
            QString path = m_model->filePathAt(i - m_model->dirCount());
            m_thumbnailer.getThumbnailAsync(path, size, crop, force);
        }
    }
}

void DirectoryPresenter::onThumbnailReady(std::shared_ptr<Thumbnail> thumb, const QString &filePath) {
    if(!m_view || !m_model)
        return;
    int index = m_model->indexOfFile(filePath);
    if(index == -1)
        return;
    m_view->setThumbnail(m_showDirs ? m_model->dirCount() + index : index, thumb);
}

void DirectoryPresenter::onItemActivated(int absoluteIndex) {
    if(!m_model)
        return;
    if(!m_showDirs) {
        emit fileActivated(m_model->filePathAt(absoluteIndex));
        return;
    }
    if(absoluteIndex < m_model->dirCount())
        emit dirActivated(m_model->dirPathAt(absoluteIndex));
    else
        emit fileActivated(m_model->filePathAt(absoluteIndex - m_model->dirCount()));
}

void DirectoryPresenter::onDraggedOut() {
    emit draggedOut(selectedPaths());
}

void DirectoryPresenter::onDraggedOver(int index) {
    if(!m_model || m_view->selection().contains(index))
        return;
    if(showDirs() && index < m_model->dirCount())
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
    if(showDirs() && targetIndex >= m_model->dirCount())
        return;

    // convert urls to qstrings
    QStringList pathList;
    QList<QUrl> urlList = data->urls();
    for(int i = 0; i < urlList.size(); ++i)
        pathList.append(urlList.at(i).toLocalFile());

    // get target dir path
    QString destDir;
    if(showDirs() && targetIndex < m_model->dirCount())
        destDir = m_model->dirPathAt(targetIndex);
    if(destDir.isEmpty()) // fallback to the current dir
        destDir = m_model->directoryPath();
    pathList.removeAll(destDir); // remove target dir from source list

    // pass to core
    emit droppedInto(pathList, destDir);
}

void DirectoryPresenter::selectAndFocus(const QString &path) {
    if(!m_model || !m_view || path.isEmpty())
        return;
    if(m_model->containsDir(path) && showDirs()) {
        int dirIndex = m_model->indexOfDir(path);
        m_view->select(dirIndex);
        m_view->focusOn(dirIndex);
    } else if(m_model->containsFile(path)) {
        int fileIndex = showDirs() ? m_model->indexOfFile(path) + m_model->dirCount() : m_model->indexOfFile(path);
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
