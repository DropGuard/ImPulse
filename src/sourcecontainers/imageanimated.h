#pragma once

#include "image.h"
#include <QMovie>
#include <QTimer>

class ImageAnimated : public Image {
  public:
    ImageAnimated(std::unique_ptr<DocumentInfo> _info);
    ~ImageAnimated() override;

    std::shared_ptr<const QImage> getImage() override;
    std::shared_ptr<QMovie> getMovie();
    int height() override;
    int width() override;
    QSize size() override;

    bool isEdited();

    int frameCount();
  public slots:
    bool save(QString destPath, int jpegQuality) override;

  private:
    void load() override;
    QSize m_size;
    int m_frameCount;
    std::shared_ptr<QMovie> m_movie;
    void loadMovie();
};
