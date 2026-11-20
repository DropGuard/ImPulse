#include "iconbutton.h"

IconButton::IconButton(QWidget *parent) : IconWidget(parent), m_checkable(false), m_checked(false), m_pressed(false) {
}

void IconButton::setCheckable(bool mode) {
    m_checkable = mode;
}

bool IconButton::isChecked() {
    return m_checked;
}

void IconButton::setChecked(bool mode) {
    if(m_checkable && mode != m_checked) {
        m_pressed = false;
        setProperty("pressed", false);
        m_checked = mode;
        setProperty("checked", m_checked);
        style()->unpolish(this);
        style()->polish(this);
    }
}

void IconButton::mousePressEvent(QMouseEvent *event) {
    Q_UNUSED(event)
    if(m_checkable) {
        setChecked(!this->property("checked").toBool());
        emit toggled(m_checked);
    } else {
        m_pressed = true;
        setProperty("pressed", true);
    }
    style()->unpolish(this);
    style()->polish(this);
}

void IconButton::mouseReleaseEvent(QMouseEvent *event) {
    Q_UNUSED(event)
    m_pressed = false;
    if(rect().contains(event->pos()) && !m_checkable) {
        emit clicked();
    }
    if(!m_checked) {
        setProperty("pressed", false);
        style()->unpolish(this);
        style()->polish(this);
    }
}

void IconButton::mouseMoveEvent(QMouseEvent *event) {
    if(m_checked || !m_pressed)
        return;
    if(rect().contains(event->pos())) {
        if(!property("pressed").toBool()) {
            setProperty("pressed", true);
            style()->unpolish(this);
            style()->polish(this);
        }
    } else {
        if(property("pressed").toBool()) {
            setProperty("pressed", false);
            style()->unpolish(this);
            style()->polish(this);
        }
    }
}
