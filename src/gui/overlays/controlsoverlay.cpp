#include "controlsoverlay.h"

ControlsOverlay::ControlsOverlay(FloatingWidgetContainer *parent) : FloatingWidget(parent) {
    m_folderViewButton = new ActionButton("folderView", ":/res/icons/common/buttons/panel/folderview20.png", 30);
    m_folderViewButton->setAccessibleName("ButtonSmall");
    m_settingsButton = new ActionButton("openSettings", ":/res/icons/common/buttons/panel/settings20.png", 30);
    m_settingsButton->setAccessibleName("ButtonSmall");
    m_closeButton = new ActionButton("exit", ":/res/icons/common/buttons/panel/close16.png", 30);
    m_closeButton->setAccessibleName("ButtonSmall");

    QWidget *horizontalLineWidget = new QWidget;
    horizontalLineWidget->setFixedSize(5, 22);
    horizontalLineWidget->setStyleSheet(QString("background-color: #707070; margin-left: 2px; margin-right: 2px"));

    m_layout.setContentsMargins(0, 0, 0, 0);
    this->setContentsMargins(0, 0, 0, 0);
    m_layout.setSpacing(0);
    m_layout.addWidget(m_folderViewButton);
    m_layout.addWidget(horizontalLineWidget);
    m_layout.addWidget(m_settingsButton);
    m_layout.addWidget(m_closeButton);
    setLayout(&m_layout);
    fitToContents();

    setMouseTracking(true);

    m_fadeEffect = new QGraphicsOpacityEffect(this);
    this->setGraphicsEffect(m_fadeEffect);
    m_fadeAnimation = new QPropertyAnimation(m_fadeEffect, "opacity", this);
    m_fadeAnimation->setDuration(230);
    m_fadeAnimation->setStartValue(1.0f);
    m_fadeAnimation->setEndValue(0);
    m_fadeAnimation->setEasingCurve(QEasingCurve::OutQuart);

    if(parent)
        setContainerSize(parent->size());
    //this->show();
}

void ControlsOverlay::show() {
    m_fadeEffect->setOpacity(0.0);
    FloatingWidget::show();
}

QSize ControlsOverlay::contentsSize() {
    QSize newSize(0, 0);
    for(int i = 0; i < m_layout.count(); i++) {
        newSize.setWidth(newSize.width() + m_layout.itemAt(i)->widget()->width());
        newSize.setHeight(m_layout.itemAt(i)->widget()->height());
    }
    return newSize;
}

void ControlsOverlay::fitToContents() {
    this->setFixedSize(contentsSize());
    recalculateGeometry();
}

void ControlsOverlay::recalculateGeometry() {
    setGeometry(containerSize().width() - width(), 0, width(), height());
}

void ControlsOverlay::enterEvent(QEnterEvent *event) {
    Q_UNUSED(event)
    m_fadeAnimation->stop();
    m_fadeEffect->setOpacity(1.0);
}

void ControlsOverlay::leaveEvent(QEvent *event) {
    Q_UNUSED(event)
    m_fadeAnimation->start();
}
