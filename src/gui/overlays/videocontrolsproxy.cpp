#include "videocontrolsproxy.h"

VideoControlsProxyWrapper::VideoControlsProxyWrapper(FloatingWidgetContainer *parent)
    : m_container(parent), m_videoControls(nullptr) {
}

VideoControlsProxyWrapper::~VideoControlsProxyWrapper() {
    // owned by the container; see ZoomIndicatorOverlayProxy
}

void VideoControlsProxyWrapper::init() {
    if(m_videoControls)
        return;
    m_videoControls = new VideoControls(m_container);

    connect(m_videoControls, &VideoControls::seekBackward, this, &VideoControlsProxyWrapper::seekBackward);
    connect(m_videoControls, &VideoControls::seekForward, this, &VideoControlsProxyWrapper::seekForward);
    connect(m_videoControls, &VideoControls::seek, this, &VideoControlsProxyWrapper::seek);

    m_videoControls->setMode(m_stateBuf.mode);
    m_videoControls->setPlaybackDuration(m_stateBuf.duration);
    m_videoControls->setPlaybackPosition(m_stateBuf.position);
    m_videoControls->onPlaybackPaused(m_stateBuf.paused);
    m_videoControls->onVideoMuted(m_stateBuf.videoMuted);
}

void VideoControlsProxyWrapper::show() {
    init();
    m_videoControls->show();
}

void VideoControlsProxyWrapper::hide() {
    if(m_videoControls)
        m_videoControls->hide();
}

void VideoControlsProxyWrapper::setPlaybackDuration(int _duration) {
    if(m_videoControls) {
        m_videoControls->setPlaybackDuration(_duration);
    } else {
        m_stateBuf.duration = _duration;
    }
}

void VideoControlsProxyWrapper::setPlaybackPosition(int _position) {
    if(m_videoControls) {
        m_videoControls->setPlaybackPosition(_position);
    } else {
        m_stateBuf.position = _position;
    }
}

void VideoControlsProxyWrapper::setMode(PlaybackMode _mode) {
    if(m_videoControls) {
        m_videoControls->setMode(_mode);
    } else {
        m_stateBuf.mode = _mode;
    }
}

void VideoControlsProxyWrapper::onPlaybackPaused(bool _mode) {
    if(m_videoControls) {
        m_videoControls->onPlaybackPaused(_mode);
    } else {
        m_stateBuf.paused = _mode;
    }
}

void VideoControlsProxyWrapper::onVideoMuted(bool _mode) {
    if(m_videoControls) {
        m_videoControls->onVideoMuted(_mode);
    } else {
        m_stateBuf.videoMuted = _mode;
    }
}

bool VideoControlsProxyWrapper::underMouse() {
    return m_videoControls ? m_videoControls->underMouse() : false;
}

bool VideoControlsProxyWrapper::isVisible() {
    return m_videoControls ? m_videoControls->isVisible() : false;
}
