#include "thumbnail.h"

Thumbnail::Thumbnail(QString _name, QString _info, int _size, std::shared_ptr<QImage> _image)
    : mName(_name), mInfo(_info), mImage(_image), mSize(_size) {
    if(_image)
        mHasAlphaChannel = _image->hasAlphaChannel();
}

const QString &Thumbnail::name() {
    return mName;
}

const QString &Thumbnail::info() {
    return mInfo;
}

int Thumbnail::size() {
    return mSize;
}

bool Thumbnail::hasAlphaChannel() {
    return mHasAlphaChannel;
}

std::shared_ptr<QImage> Thumbnail::image() {
    return mImage;
}
