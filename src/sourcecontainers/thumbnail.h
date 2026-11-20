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
    QString m_name, m_info;
    std::shared_ptr<QImage> m_image;
    int m_size;
    bool m_hasAlphaChannel;
};
