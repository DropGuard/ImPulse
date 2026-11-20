#include "filereplacedialog.h"
#include "ui_filereplacedialog.h"

FileReplaceDialog::FileReplaceDialog(QWidget *parent) : QDialog(parent), m_ui(new Ui::FileReplaceDialog) {
    m_ui->setupUi(this);
    m_multi = false;
    connect(m_ui->yesButton, &QPushButton::clicked, this, &FileReplaceDialog::onYesClicked);
    connect(m_ui->noButton, &QPushButton::clicked, this, &FileReplaceDialog::onNoClicked);
    connect(m_ui->cancelButton, &QPushButton::clicked, this, &FileReplaceDialog::onCancelClicked);
}

FileReplaceDialog::~FileReplaceDialog() {
    delete m_ui;
}

void FileReplaceDialog::setSource(const QString &src) {
    m_ui->srcLabel->setText(src);
}

void FileReplaceDialog::setDestination(const QString &dst) {
    m_ui->dstLabel->setText(dst);
}

void FileReplaceDialog::setMode(FileReplaceMode mode) {
    if(mode == FILE_TO_FILE) {
        setWindowTitle("File already exists");
        m_ui->titleLabel->setText("Replace destination file?");
    } else if(mode == DIR_TO_DIR) {
        setWindowTitle("Directory already exists");
        m_ui->titleLabel->setText("Merge directories?");
    } else if(mode == DIR_TO_FILE) {
        setWindowTitle("Destination already exists");
        m_ui->titleLabel->setText("There is a file with that name. Replace?");
    } else { // FILE_TO_DIR
        setWindowTitle("Destination already exists");
        m_ui->titleLabel->setText("There is a folder with that name. Replace?");
    }
}

void FileReplaceDialog::setMulti(bool _multi) {
    m_multi = _multi;
    m_ui->applyAllCheckBox->setVisible(m_multi);
}

DialogResult FileReplaceDialog::getResult() {
    return m_result;
}

void FileReplaceDialog::onYesClicked() {
    m_result.yes = true;
    m_result.all = m_ui->applyAllCheckBox->isChecked();
    m_result.cancel = false;
    this->close();
}

void FileReplaceDialog::onNoClicked() {
    m_result.yes = false;
    m_result.all = m_ui->applyAllCheckBox->isChecked();
    m_result.cancel = false;
    this->close();
}

void FileReplaceDialog::onCancelClicked() {
    m_result.cancel = true;
    this->close();
}
