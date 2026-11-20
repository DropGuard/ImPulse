#include "imageinfooverlayproxy.h"

ImageInfoOverlayProxy::ImageInfoOverlayProxy(FloatingWidgetContainer *parent)
    : m_container(parent), m_overlay(nullptr) {
}

ImageInfoOverlayProxy::~ImageInfoOverlayProxy() {
    // owned by the container; see ZoomIndicatorOverlayProxy
}

void ImageInfoOverlayProxy::show() {
    init();
    m_overlay->show();
}

void ImageInfoOverlayProxy::hide() {
    if(m_overlay)
        m_overlay->hide();
}

void ImageInfoOverlayProxy::init() {
    if(m_overlay)
        return;
    m_overlay = new ImageInfoOverlay(m_container);
    m_overlay->setExifInfo(m_stateBuf.info);
}

bool ImageInfoOverlayProxy::isHidden() {
    return m_overlay ? m_overlay->isHidden() : true;
}

void ImageInfoOverlayProxy::setExifInfo(const QMap<QString, QString> &_info) {
    if(m_overlay)
        m_overlay->setExifInfo(_info);
    else
        m_stateBuf.info = _info;
}
