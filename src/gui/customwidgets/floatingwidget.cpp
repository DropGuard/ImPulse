#include "floatingwidget.h"

FloatingWidget::FloatingWidget(FloatingWidgetContainer *parent) : QWidget(parent), m_acceptKeyboardFocus(false) {
    this->setAccessibleName("OverlayWidget");
    connect(parent, &FloatingWidgetContainer::resized, this, &FloatingWidget::onContainerResized);
    hide();
}

QSize FloatingWidget::containerSize() {
    return m_container;
}

void FloatingWidget::setContainerSize(QSize sz) {
    this->m_container = sz;
    recalculateGeometry();
}

void FloatingWidget::onContainerResized(QSize size) {
    setContainerSize(size);
}

void FloatingWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

bool FloatingWidget::acceptKeyboardFocus() const {
    return m_acceptKeyboardFocus;
}
void FloatingWidget::setAcceptKeyboardFocus(bool mode) {
    m_acceptKeyboardFocus = mode;
}

void FloatingWidget::recalculateGeometry() {
}

void FloatingWidget::mousePressEvent(QMouseEvent *event) {
    event->accept();
}

void FloatingWidget::mouseReleaseEvent(QMouseEvent *event) {
    event->accept();
}

void FloatingWidget::wheelEvent(QWheelEvent *event) {
    event->accept();
}

void FloatingWidget::hide() {
    QWidget::hide();
    if(this->hasFocus() || isAncestorOf(qApp->focusWidget()))
        parentWidget()->setFocus();
}
