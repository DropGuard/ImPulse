#include "selectionquery.h"

#include "appstate.h"
#include "components/directorypresenter.h"

SelectionQuery::SelectionQuery(const State &state, const DirectoryPresenter &folderView)
    : m_state(state), m_folderView(folderView) {
}

void SelectionQuery::attachModel(const DirectoryModel *model) {
    m_model = model;
}

QString SelectionQuery::selectedPath() const {
    if(!m_model)
        return "";
    else if(m_state.viewMode.value() == MODE_FOLDERVIEW)
        return m_folderView.selectedPaths().constLast();
    else
        return m_state.currentFilePath;
}

QList<QString> SelectionQuery::currentSelection() const {
    if(!m_model)
        return QList<QString>();
    else if(m_state.viewMode.value() == MODE_FOLDERVIEW)
        return m_folderView.selectedPaths();
    else
        return QList<QString>() << m_state.currentFilePath;
}
