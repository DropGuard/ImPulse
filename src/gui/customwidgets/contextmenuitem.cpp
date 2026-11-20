#include "contextmenuitem.h"

#include <utility>

ContextMenuItem::ContextMenuItem(QWidget *parent) : MenuItem(parent), m_action("") {
}

ContextMenuItem::~ContextMenuItem() {
}

void ContextMenuItem::setAction(QString text) {
    this->m_action = std::move(text);
    setShortcutText(actionManager->shortcutForAction(m_action));
}

void ContextMenuItem::onPress() {
    emit pressed();
    if(!m_action.isEmpty())
        actionManager->invokeAction(m_action);
}
