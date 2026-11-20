#pragma once

#include <QImage>
#include <QImageWriter>
#include <QSemaphore>
#include <QCryptographicHash>
#include "image.h"
#include "utils/imagelib.h"
#include <settings.h>
#include <QIcon>

class ImageStatic : public Image {
  public:
    ImageStatic(std::unique_ptr<DocumentInfo> _info, bool scaled, const LoadPolicy &policy);
    bool isScaledRequest() const {
        return m_scaledRequest;
    }
    bool isScaledResult() const override {
        return m_scaledApplied;
    }
    ~ImageStatic() override;

    std::shared_ptr<const QImage> getSourceImage();
    std::shared_ptr<const QImage> getImage() override;

    int height() override;
    int width() override;
    QSize size() override;
    QSize originalSize() override;

    bool setEditedImage(std::unique_ptr<const QImage> imageEditedNew);
    bool discardEditedImage();

  public slots:
    void crop(QRect newRect);
    bool save(QString destPath, int jpegQuality) override;

  private:
    bool m_scaledRequest = false;
    bool m_scaledApplied = false;
    QSize m_originalSize;
    int m_allocationLimitMb;
    void load() override;
    std::shared_ptr<const QImage> m_image, m_imageEdited;
    void loadGeneric();
    void loadICO();
};
