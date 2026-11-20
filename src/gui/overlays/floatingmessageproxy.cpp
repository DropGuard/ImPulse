#include "floatingmessageproxy.h"

#include <utility>

FloatingMessageProxy::FloatingMessageProxy(FloatingWidgetContainer *parent) : m_container(parent), m_overlay(nullptr) {
}

FloatingMessageProxy::~FloatingMessageProxy() {
    // owned by the container; see ZoomIndicatorOverlayProxy
}

void FloatingMessageProxy::showMessage(QString text, FloatingMessageIcon icon, int duration) {
    init();
    m_overlay->showMessage(std::move(text), icon, duration);
}

void FloatingMessageProxy::showMessage(QString text, FloatingWidgetPosition position, FloatingMessageIcon icon,
                                       int duration) {
    init();
    m_overlay->showMessage(std::move(text), position, icon, duration);
}

void FloatingMessageProxy::init() {
    if(m_overlay)
        return;
    m_overlay = new FloatingMessage(m_container);
}
