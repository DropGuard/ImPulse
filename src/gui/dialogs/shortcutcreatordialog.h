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
    // True when the caller may proceed: a User-policy action always, a System
    // one only after the user confirms. Static because the policy question is
    // the action table's and needs no dialog instance.
    static bool confirmRebinding(const QString &action, QWidget *parent);
    void setShortcut(const QString &);

  private slots:
    void onShortcutEdited();

  private:
    Ui::ShortcutCreatorDialog *m_ui;
    QList<QString> m_actionList, m_scriptList;
    QString m_lastCategory; // the chooser's grouping cursor
};
