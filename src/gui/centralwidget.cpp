#include "centralwidget.h"

CentralWidget::CentralWidget(DocumentWidget *_docWidget, FolderViewProxy *_folderView, QWidget *parent)
    : QStackedWidget(parent), m_documentView(_docWidget), m_folderView(_folderView) {
    setMouseTracking(true);
    if(!m_documentView || !m_folderView)
        qDebug() << "[CentralWidget] Error: child widget is null. We will crash now.  Bye.";

    // docWidget - 0, folderView - 1
    addWidget(m_documentView);
    if(m_folderView)
        addWidget(m_folderView);
    showDocumentView();
}

void CentralWidget::showDocumentView() {
    if(m_mode == MODE_DOCUMENT)
        return;
    m_mode = MODE_DOCUMENT;
    setCurrentIndex(0);
    widget(0)->setFocus();
    m_documentView->viewWidget()->startPlayback();
}

void CentralWidget::showFolderView() {
    if(m_mode == MODE_FOLDERVIEW)
        return;

    m_mode = MODE_FOLDERVIEW;
    setCurrentIndex(1);
    widget(1)->show();
    widget(1)->setFocus();
    m_documentView->viewWidget()->stopPlayback();
}

void CentralWidget::toggleViewMode() {
    (m_mode == MODE_DOCUMENT) ? showFolderView() : showDocumentView();
}

ViewMode CentralWidget::currentViewMode() {
    return m_mode;
}
