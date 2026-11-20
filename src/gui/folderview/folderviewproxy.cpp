#include "folderviewproxy.h"

FolderViewProxy::FolderViewProxy(QWidget *parent) : QWidget(parent), m_folderView(nullptr) {
    m_stateBuf.sortingMode = settings->sortingMode();
    m_layout.setContentsMargins(0, 0, 0, 0);
}

void FolderViewProxy::init() {
    qApp->processEvents(); // chew through events in case we have something that alters stateBuf in queue
    QMutexLocker ml(&m_m);
    if(m_folderView)
        return;
    m_folderView = new FolderView();
    m_folderView->setParent(this);
    ml.unlock();
    m_layout.addWidget(m_folderView);
    this->setFocusProxy(m_folderView);
    this->setLayout(&m_layout);

    connect(m_folderView, &FolderView::itemActivated, this, &FolderViewProxy::itemActivated);
    connect(m_folderView, &FolderView::thumbnailsRequested, this, &FolderViewProxy::thumbnailsRequested);
    connect(m_folderView, &FolderView::sortingSelected, this, &FolderViewProxy::sortingSelected);
    connect(m_folderView, &FolderView::showFoldersChanged, this, &FolderViewProxy::showFoldersChanged);
    connect(m_folderView, &FolderView::directorySelected, this, &FolderViewProxy::directorySelected);
    connect(m_folderView, &FolderView::draggedOut, this, &FolderViewProxy::draggedOut);
    connect(m_folderView, &FolderView::copyUrlsRequested, this, &FolderViewProxy::copyUrlsRequested);
    connect(m_folderView, &FolderView::moveUrlsRequested, this, &FolderViewProxy::moveUrlsRequested);
    connect(m_folderView, &FolderView::droppedInto, this, &FolderViewProxy::droppedInto);
    connect(m_folderView, &FolderView::draggedOver, this, &FolderViewProxy::draggedOver);

    m_folderView->show();

    // apply buffer
    if(!m_stateBuf.directory.isEmpty())
        m_folderView->setDirectoryPath(m_stateBuf.directory);
    m_folderView->onFullscreenModeChanged(m_stateBuf.fullscreenMode);
    m_folderView->populate(m_stateBuf.itemCount);
    m_folderView->select(m_stateBuf.selection);
    // wait till layout stuff happens
    // before calling focusOn()
    qApp->processEvents();
    m_folderView->focusOnSelection();
    m_folderView->onSortingChanged(m_stateBuf.sortingMode);
}

void FolderViewProxy::populate(int count) {
    QMutexLocker ml(&m_m);
    m_stateBuf.itemCount = count;
    if(m_folderView) {
        ml.unlock();
        m_folderView->populate(m_stateBuf.itemCount);
    } else {
        m_stateBuf.selection.clear();
    }
}

void FolderViewProxy::setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) {
    if(m_folderView) {
        m_folderView->setThumbnail(pos, thumb);
    }
}

void FolderViewProxy::select(QList<int> indices) {
    if(m_folderView) {
        m_folderView->select(indices);
    } else {
        m_stateBuf.selection = indices;
    }
}

void FolderViewProxy::select(int index) {
    if(m_folderView) {
        m_folderView->select(index);
    } else {
        m_stateBuf.selection.clear();
        m_stateBuf.selection << index;
    }
}

QList<int> FolderViewProxy::selection() {
    if(m_folderView) {
        return m_folderView->selection();
    } else {
        return m_stateBuf.selection;
    }
}

void FolderViewProxy::focusOn(int index) {
    if(m_folderView) {
        m_folderView->focusOn(index);
    }
}

void FolderViewProxy::focusOnSelection() {
    if(m_folderView) {
        m_folderView->focusOnSelection();
    }
}

void FolderViewProxy::setDirectoryPath(QString path) {
    if(m_folderView) {
        m_folderView->setDirectoryPath(path);
    } else {
        m_stateBuf.directory = path;
    }
}

const QString &FolderViewProxy::directoryPath() const {
    return m_stateBuf.directory;
}

void FolderViewProxy::insertItem(int index) {
    if(m_folderView) {
        m_folderView->insertItem(index);
    } else {
        m_stateBuf.itemCount++;
    }
}

void FolderViewProxy::removeItem(int index) {
    if(m_folderView) {
        m_folderView->removeItem(index);
    } else {
        m_stateBuf.itemCount--;
        m_stateBuf.selection.removeAll(index);
        for(int i = 0; i < m_stateBuf.selection.count(); i++) {
            if(m_stateBuf.selection[i] > index)
                m_stateBuf.selection[i]--;
        }
        if(!m_stateBuf.selection.count())
            m_stateBuf.selection << ((index >= m_stateBuf.itemCount) ? m_stateBuf.itemCount - 1 : index);
    }
}

void FolderViewProxy::reloadItem(int index) {
    if(m_folderView)
        m_folderView->reloadItem(index);
}

// No row state but the selection lives in the buffer, and init() replays
// exactly that — so the move has to reach it even before the view exists.
void FolderViewProxy::renameItem(int oldIndex, int newIndex, const QString &name) {
    if(m_folderView) {
        m_folderView->renameItem(oldIndex, newIndex, name);
    } else if(oldIndex != newIndex) {
        for(int i = 0; i < m_stateBuf.selection.count(); i++) {
            if(m_stateBuf.selection[i] == oldIndex)
                m_stateBuf.selection[i] = newIndex;
            else if(oldIndex < newIndex && m_stateBuf.selection[i] > oldIndex && m_stateBuf.selection[i] <= newIndex)
                m_stateBuf.selection[i]--;
            else if(newIndex < oldIndex && m_stateBuf.selection[i] >= newIndex && m_stateBuf.selection[i] < oldIndex)
                m_stateBuf.selection[i]++;
        }
    }
}

void FolderViewProxy::setDragHover(int index) {
    if(m_folderView)
        m_folderView->setDragHover(index);
}

void FolderViewProxy::addItem() {
    if(m_folderView) {
        m_folderView->addItem();
    } else {
        m_stateBuf.itemCount++;
    }
}

void FolderViewProxy::onFullscreenModeChanged(bool mode) {
    if(m_folderView) {
        m_folderView->onFullscreenModeChanged(mode);
    } else {
        m_stateBuf.fullscreenMode = mode;
    }
}

void FolderViewProxy::onSortingChanged(SortingMode mode) {
    if(m_folderView) {
        m_folderView->onSortingChanged(mode);
    } else {
        m_stateBuf.sortingMode = mode;
    }
}

// the grid scope actions: forward-only — there is nothing to buffer, since
// movement without a materialized grid has nothing to move
void FolderViewProxy::gridSelectNext() {
    if(m_folderView)
        m_folderView->gridSelectNext();
}

void FolderViewProxy::gridSelectPrev() {
    if(m_folderView)
        m_folderView->gridSelectPrev();
}

void FolderViewProxy::gridSelectAbove() {
    if(m_folderView)
        m_folderView->gridSelectAbove();
}

void FolderViewProxy::gridSelectBelow() {
    if(m_folderView)
        m_folderView->gridSelectBelow();
}

void FolderViewProxy::gridPageUp() {
    if(m_folderView)
        m_folderView->gridPageUp();
}

void FolderViewProxy::gridPageDown() {
    if(m_folderView)
        m_folderView->gridPageDown();
}

void FolderViewProxy::gridSelectFirst() {
    if(m_folderView)
        m_folderView->gridSelectFirst();
}

void FolderViewProxy::gridSelectLast() {
    if(m_folderView)
        m_folderView->gridSelectLast();
}

void FolderViewProxy::gridSelectAll() {
    if(m_folderView)
        m_folderView->gridSelectAll();
}

void FolderViewProxy::activateCurrent() {
    if(m_folderView)
        m_folderView->activateCurrent();
}

void FolderViewProxy::showEvent(QShowEvent *event) {
    init();
    QWidget::showEvent(event);
}
