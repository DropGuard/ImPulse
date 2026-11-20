#include "zoomindicatoroverlayproxy.h"

ZoomIndicatorOverlayProxy::ZoomIndicatorOverlayProxy(FloatingWidgetContainer *parent)
    : m_container(parent), m_overlay(nullptr) {
}

ZoomIndicatorOverlayProxy::~ZoomIndicatorOverlayProxy() {
    // The overlay is the container's child, so the tree reclaims it: deleting
    // it from here was a proxy destroying a widget it never owned. The
    // use-after-free that shape allowed is closed by the member being a
    // QPointer, which nulls itself when the overlay dies - pinned by
    // proxyOutlivesItsContainer in core_nav_tests.cpp.
}

void ZoomIndicatorOverlayProxy::show() {
    init();
    m_overlay->show();
}

void ZoomIndicatorOverlayProxy::show(int duration) {
    init();
    m_overlay->show(duration);
}

void ZoomIndicatorOverlayProxy::hide() {
    if(m_overlay)
        m_overlay->hide();
}

void ZoomIndicatorOverlayProxy::setScale(qreal scale) {
    if(m_overlay) {
        m_overlay->setScale(scale);
    } else {
        m_stateBuf.scale = scale;
    }
}

void ZoomIndicatorOverlayProxy::init() {
    if(m_overlay)
        return;
    m_overlay = new ZoomIndicatorOverlay(m_container);
    m_overlay->setScale(m_stateBuf.scale);
}
