#include "fullscreeninfooverlayproxy.h"

FullscreenInfoOverlayProxy::FullscreenInfoOverlayProxy(FloatingWidgetContainer *parent)
    : m_container(parent), m_infoOverlay(nullptr) {
}

FullscreenInfoOverlayProxy::~FullscreenInfoOverlayProxy() {
    // owned by the container; see ZoomIndicatorOverlayProxy
}

void FullscreenInfoOverlayProxy::show() {
    m_stateBuf.showImmediately = true;
    init();
}

void FullscreenInfoOverlayProxy::showWhenReady() {
    if(!m_infoOverlay)
        m_stateBuf.showImmediately = true;
    else
        m_infoOverlay->show();
}

void FullscreenInfoOverlayProxy::hide() {
    m_stateBuf.showImmediately = false;
    if(m_infoOverlay)
        m_infoOverlay->hide();
}

void FullscreenInfoOverlayProxy::setInfo(const QString &_position, const QString &_fileName, const QString &_info) {
    if(m_infoOverlay) {
        m_infoOverlay->setInfo(_position, _fileName, _info);
    } else {
        m_stateBuf.position = _position;
        m_stateBuf.fileName = _fileName;
        m_stateBuf.info = _info;
    }
}

void FullscreenInfoOverlayProxy::init() {
    if(m_infoOverlay)
        return;
    m_infoOverlay = new FullscreenInfoOverlay(m_container);
    if(!m_stateBuf.fileName.isEmpty())
        setInfo(m_stateBuf.position, m_stateBuf.fileName, m_stateBuf.info);
    if(m_stateBuf.showImmediately)
        m_infoOverlay->show();
}
