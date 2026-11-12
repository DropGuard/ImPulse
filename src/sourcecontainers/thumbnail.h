#pragma once

#include <QString>
#include <QImage>
#include <memory>

class Thumbnail {
  public:
    Thumbnail(QString _name, QString _info, int _size, std::shared_ptr<QImage> _image);
    const QString &name();
    const QString &info();
    int size();
    bool hasAlphaChannel();
    std::shared_ptr<QImage> image();

  private:
    QString mName, mInfo;
    std::shared_ptr<QImage> mImage;
    int mSize;
    bool mHasAlphaChannel;
};
