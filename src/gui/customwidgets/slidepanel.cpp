#include "slidepanel.h"

SlidePanel::SlidePanel(FloatingWidgetContainer *parent)
    : FloatingWidget(parent), m_panelSize(50), m_slideAmount(40), m_widget(nullptr) {
    m_layout.setSpacing(0);
    m_layout.setContentsMargins(0, 0, 0, 0);
    this->setLayout(&m_layout);

    //fade effect
    m_fadeEffect = new QGraphicsOpacityEffect(this);
    this->setGraphicsEffect(m_fadeEffect);

    m_startPosition = geometry().topLeft();

    m_outCurve.setType(QEasingCurve::OutQuart);

    m_timeline.setDuration(m_animationDuration);
    m_timeline.setEasingCurve(QEasingCurve::Linear);
    m_timeline.setStartFrame(0);
    m_timeline.setEndFrame(m_animationDuration);
    // For some reason 16 feels janky on windows. Linux is fine.
#ifdef _WIN32
    m_timeline.setUpdateInterval(8);
#else
    m_timeline.setUpdateInterval(16);
#endif

    connect(&m_timeline, &QTimeLine::frameChanged, this, &SlidePanel::animationUpdate);
    connect(&m_timeline, &QTimeLine::finished, this, &SlidePanel::onAnimationFinish);

    this->setAttribute(Qt::WA_NoMousePropagation, true);
    this->setFocusPolicy(Qt::NoFocus);

    setPosition(PANEL_TOP);

    QWidget::hide();
}

SlidePanel::~SlidePanel() {
}

void SlidePanel::hide() {
    m_timeline.stop();
    QWidget::hide();
}

void SlidePanel::hideAnimated() {
    if(layoutManaged())
        hide();
    else if(!this->isHidden() && m_timeline.state() != QTimeLine::Running)
        m_timeline.start();
}

bool SlidePanel::layoutManaged() {
    return m_layoutManaged;
}

void SlidePanel::setLayoutManaged(bool mode) {
    m_layoutManaged = mode;
    if(!mode)
        recalculateGeometry();
}

void SlidePanel::setWidget(QWidget *w) {
    if(!w)
        return;
    if(hasWidget())
        layout()->removeWidget(m_widget);
    m_widget = w;
    m_widget->setParent(this);
    m_layout.insertWidget(0, m_widget);
}

bool SlidePanel::hasWidget() {
    return (m_widget != nullptr);
}

void SlidePanel::show() {
    if(hasWidget()) {
        m_timeline.stop();
        m_fadeEffect->setOpacity(m_panelVisibleOpacity);
        setProperty("pos", m_startPosition);
        QWidget::show();
        QWidget::raise();
    } else {
        qDebug() << "Warning: Trying to show panel containing no widget!";
    }
}

// save current geometry so it is accessible during "pos" animation
void SlidePanel::saveStaticGeometry(QRect geometry) {
    m_staticGeometry = geometry;
}

QRect SlidePanel::staticGeometry() {
    return m_staticGeometry;
}

void SlidePanel::animationUpdate(int frame) {
    // Calculate local cursor position; correct for the current pos() animation
    QPoint adjustedPos = mapFromGlobal(QCursor::pos()) + this->pos();
    if(triggerRect().contains(adjustedPos, true)) {
        // Cancel the animation if cursor is back at the panel
        m_timeline.stop();
        m_fadeEffect->setOpacity(m_panelVisibleOpacity);
        setProperty("pos", m_startPosition);
    } else {
        // Apply the animation frame
        qreal value = m_outCurve.valueForProgress(static_cast<qreal>(frame) / m_animationDuration);
        QPoint newPosOffset = QPoint(static_cast<int>((m_endPosition.x() - m_startPosition.x()) * value),
                                     static_cast<int>((m_endPosition.y() - m_startPosition.y()) * value));
        setProperty("pos", m_startPosition + newPosOffset);
        m_fadeEffect->setOpacity(1 - value);
    }
    qApp->processEvents();
}

void SlidePanel::setAnimationRange(QPoint start, QPoint end) {
    m_startPosition = start;
    m_endPosition = end;
}

void SlidePanel::onAnimationFinish() {
    QWidget::hide();
    m_fadeEffect->setOpacity(m_panelVisibleOpacity);
    setProperty("pos", m_startPosition);
}

QRect SlidePanel::triggerRect() {
    return m_triggerRect;
}

void SlidePanel::setPosition(PanelPosition p) {
    if(p == PANEL_TOP || p == PANEL_BOTTOM)
        m_layout.setDirection(QBoxLayout::LeftToRight);
    else
        m_layout.setDirection(QBoxLayout::BottomToTop);
    m_position = p;
    recalculateGeometry();
}

PanelPosition SlidePanel::position() {
    return m_position;
}

void SlidePanel::recalculateGeometry() {
    if(layoutManaged())
        return;
    if(m_position == PANEL_TOP) {
        setAnimationRange(QPoint(0, 0), QPoint(0, 0) - QPoint(0, m_slideAmount));
        saveStaticGeometry(QRect(QPoint(0, 0), QPoint(containerSize().width() - 1, height() - 1)));
    } else if(m_position == PANEL_BOTTOM) {
        setAnimationRange(QPoint(0, containerSize().height() - height()),
                          QPoint(0, containerSize().height() - height() + m_slideAmount));
        saveStaticGeometry(QRect(QPoint(0, containerSize().height() - height()),
                                 QPoint(containerSize().width() - 1, containerSize().height())));
    } else if(m_position == PANEL_LEFT) {
        setAnimationRange(QPoint(0, 0), QPoint(0, 0) - QPoint(m_slideAmount, 0));
        saveStaticGeometry(QRect(0, 0, width(), containerSize().height()));

    } else { // right
        setAnimationRange(QPoint(containerSize().width() - width(), 0),
                          QPoint(containerSize().width() - width(), 0) + QPoint(m_slideAmount, 0));
        saveStaticGeometry(
            QRect(containerSize().width() - width(), 0, containerSize().width(), containerSize().height()));
    }
    this->setGeometry(staticGeometry());
    updateTriggerRect();
}

void SlidePanel::updateTriggerRect() {
    m_triggerRect = staticGeometry();
}

void SlidePanel::setOrientation() {
}
