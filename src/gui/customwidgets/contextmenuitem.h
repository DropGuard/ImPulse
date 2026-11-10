// A menu item that executes an action on press

#pragma once

#include "gui/customwidgets/menuitem.h"
#include "components/actionmanager/actionmanager.h"

class ContextMenuItem : public MenuItem {
    Q_OBJECT
  public:
    ContextMenuItem(QWidget *parent = nullptr);
    ~ContextMenuItem() override;
    void setAction(QString mAction);
    // keyboard activation: the same path a mouse press takes
    void activate() {
        onPress();
    }

  signals:
    void pressed();

  protected:
    void onPress() override;

  private:
    QString mAction;
};
