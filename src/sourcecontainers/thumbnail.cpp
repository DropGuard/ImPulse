#include "thumbnail.h"

Thumbnail::Thumbnail(QString _name, QString _info, int _size, std::shared_ptr<QImage> _image)
    : m_name(_name), m_info(_info), m_image(_image), m_size(_size) {
    if(_image)
        m_hasAlphaChannel = _image->hasAlphaChannel();
}

const QString &Thumbnail::name() {
    return m_name;
}

const QString &Thumbnail::info() {
    return m_info;
}

int Thumbnail::size() {
    return m_size;
}

bool Thumbnail::hasAlphaChannel() {
    return m_hasAlphaChannel;
}

std::shared_ptr<QImage> Thumbnail::image() {
    return m_image;
}
