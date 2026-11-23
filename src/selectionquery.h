#pragma once

// "The selection" as every command reads it: the folder view's selected paths
// in view order, or the file on screen in document view. One read-only query,
// shared by Core and its controllers. It used to be two private Core methods,
// which made every command that needed the selection a Core method as well.

#include <QList>
#include <QString>

struct State;
class DirectoryModel;
class DirectoryPresenter;

class SelectionQuery {
  public:
    SelectionQuery(const State &state, const DirectoryPresenter &folderView);
    SelectionQuery(const SelectionQuery &) = delete;
    SelectionQuery &operator=(const SelectionQuery &) = delete;
    // the listing materializes on first load; until then nothing is selected
    void attachModel(const DirectoryModel *model);
    // the last selected path: the file on screen outside the folder view
    QString selectedPath() const;
    // every selected path, in view order
    QList<QString> currentSelection() const;

  private:
    const State &m_state;
    const DirectoryPresenter &m_folderView;
    const DirectoryModel *m_model = nullptr;
};
