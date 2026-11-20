#include "filesystemmodelcustom.h"

FileSystemModelCustom::FileSystemModelCustom(QObject *parent) : QFileSystemModel(parent) {
    qreal dpr = qApp->devicePixelRatio();
    QString iconPath = ":/res/icons/common/menuitem/folder16.png";
    if(dpr >= (1.0 + 0.001))
        iconPath.replace(".", "@2x.");
    m_folderIcon.load(iconPath);
    ImageLib::recolor(this->m_folderIcon, settings->colorScheme().icons);

    // `settings` outlives every object, so bind the connection to this one
    connect(settings, &Settings::settingsChanged, this, [this]() {
        ImageLib::recolor(this->m_folderIcon, settings->colorScheme().icons);
    });
}

QVariant FileSystemModelCustom::data(const QModelIndex &index, int role) const {
    if(role == Qt::DecorationRole)
        return m_folderIcon;
    return QFileSystemModel::data(index, role);
}

Qt::ItemFlags FileSystemModelCustom::flags(const QModelIndex &index) const {
    if(!index.isValid()) {
        return Qt::NoItemFlags; // 0
        //return Qt::ItemIsDropEnabled;    // Allow drops in the top-level (no parent)
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
}
