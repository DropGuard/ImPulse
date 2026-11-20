#include "pathselectormenuitem.h"

PathSelectorMenuItem::PathSelectorMenuItem(QWidget *parent) : MenuItem(parent), m_directory(""), m_path("") {
    setFocusPolicy(Qt::NoFocus);
    setIconPath(":/res/icons/common/menuitem/folder16.png");
    m_iconWidget.setAttribute(Qt::WA_TransparentForMouseEvents, false);
    connect(&m_iconWidget, &IconButton::clicked, this, [this]() {
        QFileDialog dialog;
        dialog.setDirectory(m_directory);
        dialog.setWindowTitle("Select directory");
        dialog.setWindowModality(Qt::ApplicationModal);
        dialog.setFileMode(QFileDialog::Directory);
        dialog.setOption(QFileDialog::ShowDirsOnly);
        dialog.setOption(QFileDialog::DontResolveSymlinks);
        connect(&dialog, &QFileDialog::fileSelected, this, &PathSelectorMenuItem::setDirectory);
        dialog.exec();
    });
}

const QString &PathSelectorMenuItem::directory() {
    return m_directory;
}

const QString &PathSelectorMenuItem::path() {
    return m_path;
}

void PathSelectorMenuItem::setDirectory(const QString &path) {
    m_directory = path;
    m_path = path;
    QString stripped;
    stripped = m_directory.split("/", Qt::SkipEmptyParts).last();
    this->m_textLabel.setText(stripped);
}

void PathSelectorMenuItem::onPress() {
    if(!m_directory.isEmpty())
        emit directorySelected(m_directory);
}
