#include "image.h"

Image::Image(std::unique_ptr<DocumentInfo> _info)
    : m_docInfo(std::move(_info)), m_loadAttempted(false), m_edited(false), m_path(m_docInfo->filePath()) {
}

Image::~Image() {
}

const QString &Image::filePath() const {
    return m_path;
}

DocumentType Image::type() const {
    return m_docInfo->type();
}

QString Image::fileName() const {
    return m_docInfo->fileName();
}

QString Image::baseName() const {
    return m_docInfo->baseName();
}

bool Image::isEdited() const {
    return m_edited;
}

qint64 Image::fileSize() const {
    return m_docInfo->fileSize();
}

QDateTime Image::lastModified() const {
    return m_docInfo->lastModified();
}

QMap<QString, QString> Image::getExifTags() {
    return m_docInfo->getExifTags();
}
