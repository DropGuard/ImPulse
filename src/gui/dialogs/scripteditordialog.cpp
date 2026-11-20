#include "scripteditordialog.h"
#include "ui_scripteditordialog.h"

ScriptEditorDialog::ScriptEditorDialog(QWidget *parent)
    : QDialog(parent), m_ui(new Ui::ScriptEditorDialog), m_editMode(false) {
    m_ui->setupUi(this);
    this->setWindowTitle(tr("New application/script"));
    m_ui->keywordsLabel->setText(tr("Keywords:") + " %file%");
    connect(m_ui->nameLineEdit, &QLineEdit::textChanged, this, &ScriptEditorDialog::onNameChanged);
    this->onNameChanged(m_ui->nameLineEdit->text());
}

ScriptEditorDialog::ScriptEditorDialog(const QString &name, const Script &script, QWidget *parent)
    : QDialog(parent), m_ui(new Ui::ScriptEditorDialog), m_editMode(true) {
    m_ui->setupUi(this);
    this->setWindowTitle(tr("Edit"));
    this->onNameChanged(m_ui->nameLineEdit->text());
    m_editTarget = name;
    connect(m_ui->nameLineEdit, &QLineEdit::textChanged, this, &ScriptEditorDialog::onNameChanged);
    m_ui->nameLineEdit->setText(name);
    m_ui->pathLineEdit->setText(script.command);
    m_ui->blockingCheckBox->setChecked(script.blocking);
    this->onNameChanged(m_ui->nameLineEdit->text());
}

ScriptEditorDialog::~ScriptEditorDialog() {
    delete m_ui;
}

QString ScriptEditorDialog::scriptName() {
    return m_ui->nameLineEdit->text();
}

Script ScriptEditorDialog::script() {
    return Script(m_ui->pathLineEdit->text(), m_ui->blockingCheckBox->isChecked());
}

void ScriptEditorDialog::onNameChanged(const QString &name) {
    if(name.isEmpty()) {
        m_ui->messageLabel->setText(tr("Enter script name"));
        m_ui->acceptButton->setEnabled(false);
        return;
    } else {
        m_ui->acceptButton->setEnabled(true);
    }

    QString okBtnText;
    m_ui->messageLabel->clear();

    if(m_editMode) {
        if(name != m_editTarget && scriptManager->scriptExists(name)) {
            m_ui->messageLabel->setText(tr("A script with this same name exists"));
            okBtnText = "Replace";
        } else {
            okBtnText = "Save";
        }
    } else {
        if(scriptManager->scriptExists(name)) {
            m_ui->messageLabel->setText(tr("A script with this same name exists"));
            okBtnText = "Replace";
        } else {
            okBtnText = "Create";
        }
    }
    m_ui->acceptButton->setText(okBtnText);
}

void ScriptEditorDialog::selectScriptPath() {
    QFileDialog dialog;
    QString file;
#ifdef _WIN32
    file = dialog.getOpenFileName(this, tr("Select an executable/script"), "", "Executable/script (*.exe *.bat)");
#else
    file = dialog.getOpenFileName(this, tr("Select a script file"), "", "Shell script (*.sh)");
#endif
    if(!file.isEmpty()) {
        m_ui->pathLineEdit->setText("\"" + file + "\"" + " %file%");
    }
}
