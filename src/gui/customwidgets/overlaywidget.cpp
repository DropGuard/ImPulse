#include "overlaywidget.h"

OverlayWidget::OverlayWidget(FloatingWidgetContainer *parent)
    : FloatingWidget(parent), m_horizontalMargin(20), m_verticalMargin(35), m_fadeEnabled(false) {
    m_opacityEffect = new QGraphicsOpacityEffect(this);
    m_opacityEffect->setOpacity(1.0);
    this->setGraphicsEffect(m_opacityEffect);
    m_fadeAnimation = new QPropertyAnimation(this, "opacity");
    m_fadeAnimation->setDuration(120);
    m_fadeAnimation->setStartValue(1.0f);
    m_fadeAnimation->setEndValue(0.0f);
    m_fadeAnimation->setEasingCurve(QEasingCurve::OutQuad);
    connect(m_fadeAnimation, &QPropertyAnimation::finished, this, [this]() {
        QWidget::hide();
    });
}

OverlayWidget::~OverlayWidget() {
    delete m_opacityEffect;
    delete m_fadeAnimation;
}

qreal OverlayWidget::opacity() const {
    return m_opacityEffect->opacity();
}

void OverlayWidget::setOpacity(qreal opacity) {
    m_opacityEffect->setOpacity(opacity);
    update();
    emit opacityChanged();
}

void OverlayWidget::setHorizontalMargin(int margin) {
    m_horizontalMargin = margin;
    recalculateGeometry();
}

void OverlayWidget::setVerticalMargin(int margin) {
    m_verticalMargin = margin;
    recalculateGeometry();
}

int OverlayWidget::horizontalMargin() {
    return m_horizontalMargin;
}

int OverlayWidget::verticalMargin() {
    return m_verticalMargin;
}

void OverlayWidget::setPosition(FloatingWidgetPosition pos) {
    m_position = pos;
    recalculateGeometry();
}

void OverlayWidget::setFadeDuration(int duration) {
    m_fadeAnimation->setDuration(duration);
}

void OverlayWidget::setFadeEnabled(bool mode) {
    this->m_fadeEnabled = mode;
}

void OverlayWidget::show() {
    m_fadeAnimation->stop();
    m_opacityEffect->setOpacity(1.0);
    FloatingWidget::show();
}

void OverlayWidget::hideAnimated() {
    if(m_fadeEnabled && !this->isHidden()) {
        m_fadeAnimation->stop();
        m_fadeAnimation->start(QPropertyAnimation::KeepWhenStopped);
    } else {
        FloatingWidget::hide();
    }
}

void OverlayWidget::hide() {
    m_fadeAnimation->stop();
    FloatingWidget::hide();
}

void OverlayWidget::recalculateGeometry() {
    QRect newRect = QRect(QPoint(0, 0), sizeHint());
    QPoint pos(0, 0);
    switch(m_position) {
    case FloatingWidgetPosition::LEFT:
        pos.setX(m_horizontalMargin);
        pos.setY((containerSize().height() - newRect.height()) / 2);
        break;
    case FloatingWidgetPosition::RIGHT:
        pos.setX(containerSize().width() - newRect.width() - m_horizontalMargin);
        pos.setY((containerSize().height() - newRect.height()) / 2);
        break;
    case FloatingWidgetPosition::BOTTOM:
        pos.setX((containerSize().width() - newRect.width()) / 2);
        pos.setY(containerSize().height() - newRect.height() - m_verticalMargin);
        break;
    case FloatingWidgetPosition::TOP:
        pos.setX((containerSize().width() - newRect.width()) / 2);
        pos.setY(m_verticalMargin);
        break;
    case FloatingWidgetPosition::TOPLEFT:
        pos.setX(m_horizontalMargin);
        pos.setY(m_verticalMargin);
        break;
    case FloatingWidgetPosition::TOPRIGHT:
        pos.setX(containerSize().width() - newRect.width() - m_horizontalMargin);
        pos.setY(m_verticalMargin);
        break;
    case FloatingWidgetPosition::BOTTOMLEFT:
        pos.setX(m_horizontalMargin);
        pos.setY(containerSize().height() - newRect.height() - m_verticalMargin);
        break;
    case FloatingWidgetPosition::BOTTOMRIGHT:
        pos.setX(containerSize().width() - newRect.width() - m_horizontalMargin);
        pos.setY(containerSize().height() - newRect.height() - m_verticalMargin);
        break;
    case FloatingWidgetPosition::CENTER:
        pos.setX((containerSize().width() - newRect.width()) / 2);
        pos.setY((containerSize().height() - newRect.height()) / 2);
    }
    // apply position
    newRect.moveTopLeft(pos);
    setGeometry(newRect);
}
