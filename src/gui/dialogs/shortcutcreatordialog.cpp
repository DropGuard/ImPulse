#include "shortcutcreatordialog.h"
#include "ui_shortcutcreatordialog.h"

ShortcutCreatorDialog::ShortcutCreatorDialog(QWidget *parent) : QDialog(parent), ui(new Ui::ShortcutCreatorDialog) {
    ui->setupUi(this);
    setWindowTitle("Add shortcut");
    actionList = actionManager->actionList();
    scriptList = scriptManager->scriptNames();

    ui->actionsComboBox->addItems(actionList);
    ui->actionsComboBox->setCurrentIndex(0);

    ui->scriptsComboBox->addItems(scriptList);
    ui->scriptsComboBox->setCurrentIndex(0);
}

ShortcutCreatorDialog::~ShortcutCreatorDialog() {
    delete ui;
}

QString ShortcutCreatorDialog::selectedAction() {
    if(ui->actionsRadioButton->isChecked())
        return ui->actionsComboBox->currentText();
    else
        return "s:" + ui->scriptsComboBox->currentText();
}

QString ShortcutCreatorDialog::selectedShortcut() {
    return ui->sequenceEdit->sequence();
}

void ShortcutCreatorDialog::onShortcutEdited() {
    // one key may legitimately live in several scopes — the warning names
    // each table that already holds it, so "replace?" is an informed click.
    // ONE call answers it: the walk is actionManager's, and its answer
    // includes the global table (labelled "(global)") alongside the scoped
    // ones. The dialog used to stitch that half on itself through a separate
    // reader that saw only the global table, which is exactly how the
    // three-table hand-written version forgot the document table once.
    const QString sequence = ui->sequenceEdit->sequence();
    const QStringList held = actionManager->scopeClaimsForToken(sequence);
    if(!held.isEmpty())
        ui->warningLabel->setText("This shortcut is used for: " + held.join(QStringLiteral(", ")) + ". Replace?");
    else
        ui->warningLabel->setText("");
}

void ShortcutCreatorDialog::setAction(QString action) {
    auto cbox = ui->actionsComboBox;
    if(action.startsWith("s:")) {
        action = action.remove(0, 2);
        cbox = ui->scriptsComboBox;
        ui->scriptsRadioButton->setChecked(true);
    }
    int index = cbox->findText(action);
    if(index != -1)
        cbox->setCurrentIndex(index);
}

void ShortcutCreatorDialog::setShortcut(const QString &shortcut) {
    ui->sequenceEdit->setText(shortcut);
}
