#include "actionbutton.h"

#include <utility>

ActionButton::ActionButton(QWidget *parent) : IconButton(parent), m_triggerMode(TriggerMode::ClickTrigger) {
    this->setFocusPolicy(Qt::NoFocus);
    this->setProperty("checked", false);
}

ActionButton::ActionButton(QString _actionName, QString _iconPath, QWidget *parent) : ActionButton(parent) {
    setIconPath(std::move(_iconPath));
    setAction(std::move(_actionName));
}

ActionButton::ActionButton(QString _actionName, QString _iconPath, int _size, QWidget *parent)
    : ActionButton(std::move(_actionName), std::move(_iconPath), parent) {
    if(_size > 0)
        setFixedSize(_size, _size);
}

void ActionButton::setAction(QString _actionName) {
    m_actionName = std::move(_actionName);
}

void ActionButton::setTriggerMode(TriggerMode mode) {
    m_triggerMode = mode;
}

TriggerMode ActionButton::triggerMode() {
    return m_triggerMode;
}

void ActionButton::mousePressEvent(QMouseEvent *event) {
    IconButton::mousePressEvent(event);
    if(m_triggerMode == TriggerMode::PressTrigger && event->button() == Qt::LeftButton)
        actionManager->invokeAction(m_actionName);
}

void ActionButton::mouseReleaseEvent(QMouseEvent *event) {
    IconButton::mouseReleaseEvent(event);
    if(m_triggerMode == TriggerMode::ClickTrigger && rect().contains(event->pos()) && event->button() == Qt::LeftButton)
        actionManager->invokeAction(m_actionName);
}
