#include "zoomindicatoroverlay.h"

ZoomIndicatorOverlay::ZoomIndicatorOverlay(FloatingWidgetContainer *parent)
    : OverlayWidget(parent), m_fm(QApplication::font()) {
    m_hideDelay = 2000;
    m_visibilityTimer.setSingleShot(true);
    m_visibilityTimer.setInterval(m_hideDelay);

    m_layout.setContentsMargins(0, 0, 0, 0);
    m_layout.addWidget(&m_label);
    m_label.setAlignment(Qt::AlignCenter);

    this->setLayout(&m_layout);
    this->setPosition(FloatingWidgetPosition::BOTTOMLEFT);
    setHorizontalMargin(0);
    setVerticalMargin(16);

    setFadeEnabled(true);
    setFadeDuration(300);

    connect(&m_visibilityTimer, &QTimer::timeout, this, &ZoomIndicatorOverlay::hideAnimated);

    if(parent)
        setContainerSize(parent->size());
}

void ZoomIndicatorOverlay::setScale(qreal scale) {
    m_label.setText(QString::number(qRound(scale * 100.0)) + "%");
    m_label.setFixedSize(m_fm.horizontalAdvance(m_label.text()) + 14, m_fm.height() + 12);
    recalculateGeometry();
}

void ZoomIndicatorOverlay::recalculateGeometry() {
    OverlayWidget::recalculateGeometry();
}

void ZoomIndicatorOverlay::show() {
    OverlayWidget::show();
}

// "blink" the widget; show then fade out immediately
void ZoomIndicatorOverlay::show(int duration) {
    m_visibilityTimer.stop();
    OverlayWidget::show();
    // fade out after delay
    m_visibilityTimer.setInterval(duration);
    m_visibilityTimer.start();
}
