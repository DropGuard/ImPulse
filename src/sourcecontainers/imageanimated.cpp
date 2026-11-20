#include "imageanimated.h"
#include <time.h>

// TODO: this class is kinda useless now. redesign?

ImageAnimated::ImageAnimated(std::unique_ptr<DocumentInfo> _info) : Image(std::move(_info)) {
    m_size.setWidth(0);
    m_size.setHeight(0);
    load();
}

ImageAnimated::~ImageAnimated() {
}

void ImageAnimated::load() {
    if(m_loadAttempted)
        return;
    loadMovie();
    m_loadAttempted = true;
}

void ImageAnimated::loadMovie() {
    m_movie.reset(new QMovie());
    m_movie->setFileName(m_path);
    m_movie->setFormat(m_docInfo->format().toStdString().c_str());
    m_movie->jumpToFrame(0);
    m_size = m_movie->frameRect().size();
    m_frameCount = m_movie->frameCount();
}

int ImageAnimated::frameCount() {
    return m_frameCount;
}

// TODO: overwrite (self included)
bool ImageAnimated::save(QString destPath, int) {
    QFile file(m_path);
    if(file.exists()) {
        if(!file.copy(destPath)) {
            qDebug() << "Unable to save file.";
            return false;
        } else {
            qDebug() << destPath << this->filePath();
            if(destPath == this->filePath()) {
                m_docInfo->refresh();
            }
            return true;
        }
    } else {
        qDebug() << "Unable to save file. Perhaps the source file was deleted?";
        return false;
    }
}

// in case of gif returns current frame

std::shared_ptr<const QImage> ImageAnimated::getImage() {
    std::shared_ptr<const QImage> img(new QImage(m_path, m_docInfo->format().toStdString().c_str()));
    return img;
}

std::shared_ptr<QMovie> ImageAnimated::getMovie() {
    if(m_movie == nullptr)
        loadMovie();
    return m_movie;
}

int ImageAnimated::height() {
    return m_size.height();
}

int ImageAnimated::width() {
    return m_size.width();
}

QSize ImageAnimated::size() {
    return m_size;
}
