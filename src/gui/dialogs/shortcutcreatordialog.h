#pragma once

#include <QDialog>
#include <QComboBox>
#include <QRadioButton>
#include "shortcutbuilder.h"
#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"
// TODO: separate gui from components
// OR move script & action stuff to project root?

namespace Ui {
class ShortcutCreatorDialog;
}

class ShortcutCreatorDialog : public QDialog {
    Q_OBJECT

  public:
    explicit ShortcutCreatorDialog(QWidget *parent = nullptr);
    ~ShortcutCreatorDialog() override;
    QString selectedAction();
    QString selectedShortcut();
    void setAction(const QString &);
    void setShortcut(const QString &);

  private slots:
    void onShortcutEdited();

  private:
    Ui::ShortcutCreatorDialog *ui;
    QList<QString> actionList, scriptList;
    QString lastCategory; // the chooser's grouping cursor
};
