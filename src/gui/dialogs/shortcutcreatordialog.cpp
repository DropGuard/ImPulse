#include "shortcutcreatordialog.h"
#include "ui_shortcutcreatordialog.h"

#include "actions_table.h"
#include <QStandardItemModel>
#include <QMessageBox>
#include <QCoreApplication>

ShortcutCreatorDialog::ShortcutCreatorDialog(QWidget *parent) : QDialog(parent), m_ui(new Ui::ShortcutCreatorDialog) {
    m_ui->setupUi(this);
    setWindowTitle("Add shortcut");
    m_scriptList = scriptManager->scriptNames();

    // The chooser reads the action table: display names grouped by category,
    // with the stable name in the item data (the visible text is translated
    // and must never be parsed back). Reserved actions are not offered — the
    // one policy that means "this action has no user-facing key" — while
    // System actions ARE offered, with their policy readable on the item so
    // the rebinding confirmation can name what is being rebound.
    for(const ActionDesc &row : ActionTable::kActions) {
        if(row.policy == ActionDesc::Policy::Reserved)
            continue;
        const QString category = tr(row.category);
        if(m_actionList.isEmpty() || m_lastCategory != category) {
            // a disabled header item makes the grouping visible without a
            // second widget; it carries no action name, so it cannot be picked
            m_ui->actionsComboBox->addItem(QStringLiteral("— ") + category + QStringLiteral(" —"));
            auto *model = qobject_cast<QStandardItemModel *>(m_ui->actionsComboBox->model());
            if(model) {
                QStandardItem *header = model->item(m_ui->actionsComboBox->count() - 1);
                header->setFlags(header->flags() & ~Qt::ItemIsEnabled);
            }
            m_lastCategory = category;
        }
        m_ui->actionsComboBox->addItem(tr(row.display));
        const int idx = m_ui->actionsComboBox->count() - 1;
        m_ui->actionsComboBox->setItemData(idx, QString::fromLatin1(row.name), Qt::UserRole);
        m_ui->actionsComboBox->setItemData(idx, static_cast<int>(row.policy), Qt::UserRole + 1);
        m_actionList << QString::fromLatin1(row.name);
    }
    // land on the first real action, not on a header
    for(int i = 0; i < m_ui->actionsComboBox->count(); ++i) {
        if(!m_ui->actionsComboBox->itemData(i, Qt::UserRole).toString().isEmpty()) {
            m_ui->actionsComboBox->setCurrentIndex(i);
            break;
        }
    }

    m_ui->scriptsComboBox->addItems(m_scriptList);
    m_ui->scriptsComboBox->setCurrentIndex(0);
}

ShortcutCreatorDialog::~ShortcutCreatorDialog() {
    delete m_ui;
}

QString ShortcutCreatorDialog::selectedAction() {
    if(m_ui->actionsRadioButton->isChecked()) {
        // the item DATA, never the visible text: the text is translated
        const QString name = m_ui->actionsComboBox->currentData(Qt::UserRole).toString();
        return name; // empty on a category header, which save treats as "no action"
    }
    return "s:" + m_ui->scriptsComboBox->currentText();
}

QString ShortcutCreatorDialog::selectedShortcut() {
    return m_ui->sequenceEdit->sequence();
}

void ShortcutCreatorDialog::onShortcutEdited() {
    // one key may legitimately live in several scopes — the warning names
    // each table that already holds it, so "replace?" is an informed click.
    // ONE call answers it: the walk is actionManager's, and its answer
    // includes the global table (labelled "(global)") alongside the scoped
    // ones. The dialog used to stitch that half on itself through a separate
    // reader that saw only the global table, which is exactly how the
    // three-table hand-written version forgot the document table once.
    const QString sequence = m_ui->sequenceEdit->sequence();
    const QStringList held = actionManager->scopeClaimsForToken(sequence);
    if(!held.isEmpty())
        m_ui->warningLabel->setText("This shortcut is used for: " + held.join(QStringLiteral(", ")) + ". Replace?");
    else
        m_ui->warningLabel->setText("");
}

void ShortcutCreatorDialog::setAction(const QString &action) {
    if(action.startsWith(QLatin1String("s:"))) {
        m_ui->scriptsRadioButton->setChecked(true);
        const int index = m_ui->scriptsComboBox->findText(action.mid(2));
        if(index != -1)
            m_ui->scriptsComboBox->setCurrentIndex(index);
        return;
    }
    // by the stable name in the item data — findText would search the display
    // text, which a translated UI changes
    for(int i = 0; i < m_ui->actionsComboBox->count(); ++i) {
        if(m_ui->actionsComboBox->itemData(i, Qt::UserRole).toString() == action) {
            m_ui->actionsComboBox->setCurrentIndex(i);
            return;
        }
    }
}

void ShortcutCreatorDialog::setShortcut(const QString &shortcut) {
    m_ui->sequenceEdit->setText(shortcut);
}

// The confirmation a System action's rebinding needs. The dialog does not
// want the policy itself: it asks the action table, which is the one place
// the question "may this be rebound freely" is answered. Static and public so
// the decision — not just the call site — is testable without a modal.
bool ShortcutCreatorDialog::confirmRebinding(const QString &action, QWidget *parent) {
    if(ActionTable::policyFor(action) != ActionDesc::Policy::System)
        return true;
    const ActionDesc *row = ActionTable::descFor(action);
    const QString display = row ? QCoreApplication::translate("ShortcutCreatorDialog", row->display) : action;
    return QMessageBox::question(parent,
                                 QCoreApplication::translate("ShortcutCreatorDialog", "Rebind a protected action"),
                                 QCoreApplication::translate(
                                     "ShortcutCreatorDialog",
                                     "\"%1\" is one of the actions the app protects by default: it deletes, exits, "
                                     "or is the way out. Rebinding it moves that danger onto a key you choose, where a "
                                     "mis-tap costs whatever that key used to do.\n\nRebind it anyway?")
                                     .arg(display),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}
