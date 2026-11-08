#pragma once

#include <QScreen>
#include <QStyleOption>
#include <QTimer>
#include <QPainter>
#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"

namespace Ui {
class ContextMenu;
}

class ContextMenuItem;

class ContextMenu : public QWidget {
    Q_OBJECT
public:
    explicit ContextMenu(QWidget *parent = nullptr);
    ~ContextMenu() override;
    void setImageEntriesEnabled(bool mode);
    // keyboard navigation state, for tests and for the keymap docs
    int navigationIndex() const { return selectedIndex; }

public slots:
    void showAt(QPoint pos);
    void setGeometry(QRect geom);

private:
    Ui::ContextMenu *ui;
    QTimer mTimer;
    int selectedIndex = -1;

    QList<ContextMenuItem*> visibleItems() const;
    void moveSelection(int delta);
    void activateSelected();
    void applySelection();
    void clearSelection();
    void fillOpenWithMenu();

signals:
    void showScriptSettings();

protected:
    void mousePressEvent(QMouseEvent*) override;
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;
private slots:
    void switchToMainPage();
    void switchToScriptsPage();
};
