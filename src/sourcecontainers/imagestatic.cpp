#include "imagestatic.h"

#include "utils/fileoperations.h"

#include <QImageIOHandler>
#include <QScreen>
#include <time.h>

ImageStatic::ImageStatic(std::unique_ptr<DocumentInfo> _info, bool scaled, const LoadPolicy &policy)
    : Image(std::move(_info)), m_scaledRequest(scaled), m_allocationLimitMb(policy.allocationLimitMb) {
    load();
}

ImageStatic::~ImageStatic() {
}

ImageStatic::ImageStatic(const ImageStatic &source, const QString &path, std::shared_ptr<const QImage> pixels)
    : Image(source.m_docInfo->atPath(path)), m_originalSize(source.m_originalSize),
      m_allocationLimitMb(source.m_allocationLimitMb), m_image(source.m_image), m_imageEdited(std::move(pixels)) {
    m_loadAttempted = true;
    m_edited = true;
}

std::shared_ptr<ImageStatic> ImageStatic::editedCopy(const QString &path, std::shared_ptr<const QImage> pixels) const {
    return std::shared_ptr<ImageStatic>(new ImageStatic(*this, path, std::move(pixels)));
}

//load image data from disk
void ImageStatic::load() {
    if(m_loadAttempted) {
        return;
    }
    if(m_docInfo->mimeType().name() == "image/vnd.microsoft.icon")
        loadICO();
    else
        loadGeneric();
}

void ImageStatic::loadGeneric() {
    // two-tier decode: scaled target = primary screen at devicePixelRatio
    /* QImageReader::read() seems more reliable than just reading via QImage.
     * For example: "Invalid JPEG file structure: two SOF markers"
     * QImageReader::read() returns false, but still reads an image. Meanwhile QImage just fails.
     * I havent checked qimage's code, but it seems like it sees an exception
     * from libjpeg or whatever and just gives up on reading the file.
     *
     * tldr: qimage bad
     */
    QImageReader r(m_path, m_docInfo->format().toStdString().c_str());
    r.setAllocationLimit(m_allocationLimitMb);
    // the file's own dimensions, read from the header — available before any
    // pixel decode; the title/info surfaces report these so a tier-2 upgrade
    // never rewrites them. Rotating transformations (EXIF 6/8 arrive as the
    // Rotate90 flag) swap what the header calls width and height
    m_originalSize = r.size();
    if(m_originalSize.isValid() &&
       (unsigned(m_docInfo->exifOrientation()) & unsigned(QImageIOHandler::TransformationRotate90)) != 0)
        m_originalSize.transpose();
    if(m_scaledRequest && m_docInfo->format() == "jpg") {
        // tier-1: decode at screen resolution via libjpeg DCT scaling
        // (1/2 edge = 1/4 pixels). Full-res follows as tier-2 upgrade.
        // JPEG-only: other formats scale AFTER full decode in Qt, which
        // costs more than it saves (measured: PNG scaled 84ms vs full 46ms).
        QSize orig = r.size();
        QSize screen = QApplication::primaryScreen()->size() * QApplication::primaryScreen()->devicePixelRatio();
        if(orig.isValid() && (orig.width() > screen.width() || orig.height() > screen.height())) {
            r.setScaledSize(orig.scaled(screen, Qt::KeepAspectRatio));
            m_scaledApplied = true;
        }
    }
    QImage *tmp = new QImage();
    r.read(tmp);
    std::unique_ptr<const QImage> img(tmp);
    img = ImageLib::exifRotated(std::move(img), m_docInfo.get()->exifOrientation());
    // scaling this format via qt results in transparent background
    // it rare enough so lets just convert it to the closest working thing
    if(img->format() == QImage::Format_Mono) {
        QImage *imgConverted = new QImage();
        // a 1-bit BMP carries a real color palette — Grayscale8 threw the
        // palette away and colored art opened as black and white (upstream
        // issue #653). RGB32 keeps the palette while still dodging the
        // transparent-background scaling that motivated the conversion.
        *imgConverted = img->convertToFormat(QImage::Format_RGB32);
        m_image.reset(imgConverted);
    } else {
        // set image
        m_image = std::move(img);
    }
    m_loadAttempted = true;
}

// TODO: move this out somewhere to use in other places
void ImageStatic::loadICO() {
    // Big brain code. It's mostly for small ico files so whatever. I'm not patching Qt for this.
    QIcon icon(m_path);
    const QList<QSize> sizes = icon.availableSizes();
    QSize maxSize(0, 0);
    for(const QSize &sz : sizes)
        if(maxSize.width() < sz.width())
            maxSize = sz;
    QPixmap iconPix = icon.pixmap(maxSize);
    std::unique_ptr<const QImage> img(new QImage(iconPix.toImage()));
    m_image = std::move(img);
    m_loadAttempted = true;
}

bool ImageStatic::save(QString destPath, int jpegQuality) {
    // The backup, the extension/quality decision and the rollback live in
    // FileOperations::saveWithBackup now, shared with the clipboard import:
    // two copies of that sequence had already diverged once.
    const bool wasEdited = isEdited();
    // The source is where the metadata comes from, and it is deliberately the
    // ORIGINAL path even when saving elsewhere: an edit changes pixels, not
    // provenance, so a Save As copy keeps the camera and GPS the file was shot
    // with. An edit that never happened has nothing to carry.
    const QString metadataSource = wasEdited ? m_path : QString();
    const bool success =
        FileOperations::saveWithBackup(*(wasEdited ? m_imageEdited : m_image), destPath, jpegQuality, metadataSource);
    // An edit is consumed by a write that actually happened. Swapping and
    // discarding before consulting `success` threw the user's unsaved edit away
    // on every failed save, and left the in-memory image claiming it was saved.
    if(success && wasEdited) {
        m_image.swap(m_imageEdited);
        discardEditedImage();
    }
    if(destPath == m_path && success)
        m_docInfo->refresh();
    return success;
}

std::shared_ptr<const QImage> ImageStatic::getSourceImage() {
    return m_image;
}

std::shared_ptr<const QImage> ImageStatic::getImage() {
    return isEdited() ? m_imageEdited : m_image;
}

int ImageStatic::height() {
    return isEdited() ? m_imageEdited->height() : m_image->height();
}

int ImageStatic::width() {
    return isEdited() ? m_imageEdited->width() : m_image->width();
}

QSize ImageStatic::size() {
    return isEdited() ? m_imageEdited->size() : m_image->size();
}

QSize ImageStatic::originalSize() {
    return m_originalSize.isValid() ? m_originalSize : size();
}

bool ImageStatic::setEditedImage(std::unique_ptr<const QImage> imageEditedNew) {
    if(imageEditedNew && imageEditedNew->width() != 0) {
        discardEditedImage();
        m_imageEdited = std::move(imageEditedNew);
        m_edited = true;
        return true;
    }
    return false;
}

bool ImageStatic::discardEditedImage() {
    if(m_imageEdited) {
        m_imageEdited.reset();
        m_edited = false;
        return true;
    }
    return false;
}
