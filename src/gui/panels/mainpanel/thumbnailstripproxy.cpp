#include "thumbnailstripproxy.h"

#include "gui/customwidgets/selectionremap.h"

ThumbnailStripProxy::ThumbnailStripProxy(QWidget *parent) : QWidget(parent) {
    m_layout.setContentsMargins(0, 0, 0, 0);
}

void ThumbnailStripProxy::init() {
    if(m_thumbnailStrip)
        return;
    qApp->processEvents(); // chew through events in case we have something that alters stateBuf in queue
    QMutexLocker ml(&m_m);
    m_thumbnailStrip = new ThumbnailStrip();
    m_thumbnailStrip->setParent(this);
    ml.unlock();
    m_layout.addWidget(m_thumbnailStrip);
    this->setFocusProxy(m_thumbnailStrip);
    this->setLayout(&m_layout);

    connect(m_thumbnailStrip, &ThumbnailStrip::itemActivated, this, &ThumbnailStripProxy::itemActivated);
    connect(m_thumbnailStrip, &ThumbnailStrip::thumbnailsRequested, this, &ThumbnailStripProxy::thumbnailsRequested);

    m_thumbnailStrip->show();

    // apply buffer
    m_thumbnailStrip->populate(m_stateBuf.itemCount);
    m_thumbnailStrip->select(m_stateBuf.selection);
    // wait till layout stuff happens
    // before calling focusOn()
    qApp->processEvents();
    m_thumbnailStrip->focusOnSelection();
}

bool ThumbnailStripProxy::isInitialized() {
    return (m_thumbnailStrip != nullptr);
}

void ThumbnailStripProxy::populate(int count) {
    QMutexLocker ml(&m_m);
    m_stateBuf.itemCount = count;
    if(m_thumbnailStrip) {
        ml.unlock();
        m_thumbnailStrip->populate(m_stateBuf.itemCount);
    } else {
        m_stateBuf.selection.clear();
    }
}

void ThumbnailStripProxy::setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) {
    if(m_thumbnailStrip) {
        m_thumbnailStrip->setThumbnail(pos, thumb);
    }
}

void ThumbnailStripProxy::select(QList<int> indices) {
    if(m_thumbnailStrip) {
        m_thumbnailStrip->select(indices);
    } else {
        m_stateBuf.selection = indices;
    }
}

void ThumbnailStripProxy::select(int index) {
    if(m_thumbnailStrip) {
        m_thumbnailStrip->select(index);
    } else {
        m_stateBuf.selection.clear();
        m_stateBuf.selection << index;
    }
}

QList<int> ThumbnailStripProxy::selection() {
    if(m_thumbnailStrip) {
        return m_thumbnailStrip->selection();
    } else {
        return m_stateBuf.selection;
    }
}

void ThumbnailStripProxy::focusOn(int index) {
    if(m_thumbnailStrip) {
        m_thumbnailStrip->focusOn(index);
    }
}

void ThumbnailStripProxy::focusOnSelection() {
    if(m_thumbnailStrip) {
        m_thumbnailStrip->focusOnSelection();
    }
}

void ThumbnailStripProxy::insertItem(int index) {
    if(m_thumbnailStrip) {
        m_thumbnailStrip->insertItem(index);
    } else {
        m_stateBuf.itemCount++;
    }
}

void ThumbnailStripProxy::removeItem(int index) {
    if(m_thumbnailStrip) {
        m_thumbnailStrip->removeItem(index);
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

void ThumbnailStripProxy::reloadItem(int index) {
    if(m_thumbnailStrip)
        m_thumbnailStrip->reloadItem(index);
}

// The buffered selection is the only row state a viewless proxy holds, and
// init() replays it — so the row move has to reach it here too.
void ThumbnailStripProxy::renameItem(int oldIndex, int newIndex, const QString &name) {
    if(m_thumbnailStrip) {
        m_thumbnailStrip->renameItem(oldIndex, newIndex, name);
    } else {
        remapSelectionForRename(m_stateBuf.selection, oldIndex, newIndex);
    }
}

void ThumbnailStripProxy::setDragHover(int index) {
    if(m_thumbnailStrip)
        m_thumbnailStrip->setDragHover(index);
}

void ThumbnailStripProxy::setDirectoryPath(QString path) {
    Q_UNUSED(path)
}

void ThumbnailStripProxy::addItem() {
    if(m_thumbnailStrip) {
        m_thumbnailStrip->addItem();
    } else {
        m_stateBuf.itemCount++;
    }
}

QSize ThumbnailStripProxy::itemSize() {
    return m_thumbnailStrip->itemSize();
}

void ThumbnailStripProxy::readSettings() {
    if(m_thumbnailStrip)
        m_thumbnailStrip->readSettings();
}

void ThumbnailStripProxy::showEvent(QShowEvent *event) {
    init();
    QWidget::showEvent(event);
}
