#include "video.h"
#include <time.h>

Video::Video(std::unique_ptr<DocumentInfo> _info) : Image(std::move(_info)) {
    Video::load();
}

void Video::load() {
    if(m_loadAttempted)
        return;

    // the ffmpeg/mpv resolution probe that used to live here is retired:
    // it never ran (commented out), and video metadata comes from the
    // container via DocumentInfo

    m_loadAttempted = true;
}

std::shared_ptr<const QImage> Video::getImage() {
    qDebug() << "[Video] getImage() is not implemented.";
    //TODO: find out some easy way to get frames from video source
    return nullptr;
}

int Video::height() {
    return m_srcHeight;
}

int Video::width() {
    return m_srcWidth;
}

QSize Video::size() {
    return QSize(m_srcWidth, m_srcHeight);
}
